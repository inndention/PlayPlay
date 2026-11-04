/**
 * TRAFFIC ENGINE - Probabilistic Ad Scheduling
 * 
 * Core principles:
 * - Programs and hardware events are HARD SCHEDULED (fixed times)
 * - Ads are PROBABILISTIC FILL (selected by weight/probability)
 * - Everything played is LOGGED
 * - Probabilities are DERIVED from play history vs targets
 * 
 * This is how real broadcast traffic systems work.
 */

import { MediaItem, PlaylistItem } from './types';

// ============================================
// TYPES
// ============================================

export type ContentClass = 
    | 'program'         // Hard scheduled - owns time slot
    | 'promo'           // Station promos - fill priority 1
    | 'commercial'      // Paid ads - probabilistic
    | 'psa'             // Public service - low priority fill
    | 'id'              // Station ID - fixed intervals (FCC)
    | 'bumper'          // In/out of break
    | 'filler'          // Emergency fill
    | 'hardware';       // External source (live, satellite)

export interface TrafficItem extends MediaItem {
    contentClass: ContentClass;
    
    // Contract requirements (for ads)
    clientId?: string;
    contractId?: string;
    targetPlays?: number;       // Total plays contracted
    targetPerDay?: number;      // Daily minimum
    targetPerHour?: number;     // Hourly maximum (separation)
    
    // Daypart restrictions
    allowedDayparts?: string[]; // e.g., ['morning', 'primetime']
    blockedDayparts?: string[];
    
    // Category for separation rules
    category?: string;          // e.g., 'auto', 'fast-food', 'pharma'
    
    // Weighting
    baseWeight: number;         // 1-100, higher = more likely
    priorityBoost?: number;     // Temporary boost for underdelivery
    
    // Scheduling constraints
    minSeparation?: number;     // Min seconds between plays
    notBefore?: number;         // Unix timestamp
    notAfter?: number;          // Unix timestamp (flight end)
    
    // Status
    isActive: boolean;
    isPaused: boolean;
}

export interface PlayLog {
    id: string;
    mediaId: string;
    contentClass: ContentClass;
    clientId?: string;
    timestamp: number;          // When it played
    duration: number;           // How long it played
    scheduled: boolean;         // Was it scheduled or fill?
    position: 'scheduled' | 'fill' | 'makeup';
    daypart: string;
    hour: number;
    completed: boolean;         // Did it play to completion?
}

export interface PlayStats {
    mediaId: string;
    totalPlays: number;
    todayPlays: number;
    thisHourPlays: number;
    lastPlayed: number | null;
    avgCompletionRate: number;
    playsByDaypart: Record<string, number>;
    playsByHour: Record<number, number>;
}

export interface SelectionContext {
    now: number;
    daypart: string;
    hour: number;
    availableDuration: number;  // Seconds to fill
    recentPlays: PlayLog[];     // Last N plays for separation
    dayStats: Map<string, PlayStats>;
}

export interface SelectionResult {
    item: TrafficItem;
    probability: number;
    reason: string;
    weight: number;
}

// ============================================
// DAYPART HELPERS
// ============================================

export function getDaypart(hour: number): string {
    if (hour >= 6 && hour < 9) return 'early_morning';
    if (hour >= 9 && hour < 12) return 'morning';
    if (hour >= 12 && hour < 15) return 'midday';
    if (hour >= 15 && hour < 17) return 'afternoon';
    if (hour >= 17 && hour < 20) return 'early_fringe';
    if (hour >= 20 && hour < 23) return 'primetime';
    if (hour >= 23 || hour < 1) return 'late_night';
    return 'overnight';
}

export function getDaypartDisplay(daypart: string): string {
    const names: Record<string, string> = {
        'early_morning': 'Early Morning (6-9)',
        'morning': 'Morning (9-12)',
        'midday': 'Midday (12-3)',
        'afternoon': 'Afternoon (3-5)',
        'early_fringe': 'Early Fringe (5-8)',
        'primetime': 'Primetime (8-11)',
        'late_night': 'Late Night (11-1)',
        'overnight': 'Overnight (1-6)',
    };
    return names[daypart] || daypart;
}

// ============================================
// PLAY LOGGING
// ============================================

const PLAY_LOG_KEY = 'playplay_play_log';
const PLAY_STATS_KEY = 'playplay_play_stats';

export function logPlay(
    item: TrafficItem | MediaItem,
    context: {
        scheduled: boolean;
        position: 'scheduled' | 'fill' | 'makeup';
        completed: boolean;
        duration?: number;
    }
): PlayLog {
    const now = Date.now();
    const hour = new Date(now).getHours();
    
    const log: PlayLog = {
        id: `log_${now}_${Math.random().toString(36).substr(2, 6)}`,
        mediaId: item.id,
        contentClass: (item as TrafficItem).contentClass || item.type as ContentClass || 'filler',
        clientId: (item as TrafficItem).clientId,
        timestamp: now,
        duration: context.duration || item.duration,
        scheduled: context.scheduled,
        position: context.position,
        daypart: getDaypart(hour),
        hour,
        completed: context.completed,
    };
    
    // Persist
    const logs = loadPlayLogs();
    logs.push(log);
    
    // Keep last 10000 logs
    if (logs.length > 10000) {
        logs.splice(0, logs.length - 10000);
    }
    
    savePlayLogs(logs);
    
    // Update stats
    updatePlayStats(log);
    
    return log;
}

export function loadPlayLogs(): PlayLog[] {
    try {
        const data = localStorage.getItem(PLAY_LOG_KEY);
        return data ? JSON.parse(data) : [];
    } catch {
        return [];
    }
}

export function savePlayLogs(logs: PlayLog[]) {
    try {
        localStorage.setItem(PLAY_LOG_KEY, JSON.stringify(logs));
    } catch (e) {
        if (e instanceof DOMException && e.name === 'QuotaExceededError') {
            // Trim aggressively and retry once
            try {
                const trimmed = logs.slice(-2000);
                localStorage.setItem(PLAY_LOG_KEY, JSON.stringify(trimmed));
            } catch {
                console.error('[TrafficEngine] Cannot persist play log — localStorage full');
            }
        }
    }
}

export function loadPlayStats(): Map<string, PlayStats> {
    try {
        const data = localStorage.getItem(PLAY_STATS_KEY);
        if (!data) return new Map();
        const obj = JSON.parse(data);
        return new Map(Object.entries(obj));
    } catch {
        return new Map();
    }
}

export function savePlayStats(stats: Map<string, PlayStats>) {
    try {
        const obj = Object.fromEntries(stats);
        localStorage.setItem(PLAY_STATS_KEY, JSON.stringify(obj));
    } catch (e) {
        if (e instanceof DOMException && e.name === 'QuotaExceededError') {
            console.error('[TrafficEngine] Cannot persist play stats — localStorage full');
        }
    }
}

function updatePlayStats(log: PlayLog) {
    const stats = loadPlayStats();
    const now = Date.now();
    const todayStart = new Date(now).setHours(0, 0, 0, 0);
    const currentHour = new Date(now).getHours();
    
    let stat = stats.get(log.mediaId);
    
    if (!stat) {
        stat = {
            mediaId: log.mediaId,
            totalPlays: 0,
            todayPlays: 0,
            thisHourPlays: 0,
            lastPlayed: null,
            avgCompletionRate: 1,
            playsByDaypart: {},
            playsByHour: {},
        };
    }
    
    // Update counts
    stat.totalPlays++;

    // Use the previous lastPlayed value to decide resets, then update
    const prevLastPlayed = stat.lastPlayed;
    stat.lastPlayed = log.timestamp;

    const currentHourStart = new Date(now).setMinutes(0, 0, 0);

    if (!prevLastPlayed || prevLastPlayed < todayStart) {
        // New day — full reset
        stat.todayPlays = 1;
        stat.thisHourPlays = 1;
    } else if (prevLastPlayed < currentHourStart) {
        // Same day, new hour — reset hourly counter only
        stat.todayPlays++;
        stat.thisHourPlays = 1;
    } else {
        // Same hour
        stat.todayPlays++;
        stat.thisHourPlays++;
    }
    
    // Daypart tracking
    stat.playsByDaypart[log.daypart] = (stat.playsByDaypart[log.daypart] || 0) + 1;
    stat.playsByHour[log.hour] = (stat.playsByHour[log.hour] || 0) + 1;
    
    // Completion rate (running average)
    const completionValue = log.completed ? 1 : 0;
    stat.avgCompletionRate = (stat.avgCompletionRate * (stat.totalPlays - 1) + completionValue) / stat.totalPlays;
    
    stats.set(log.mediaId, stat);
    savePlayStats(stats);
}

export function getPlayStats(mediaId: string): PlayStats | null {
    const stats = loadPlayStats();
    return stats.get(mediaId) || null;
}

export function getTodayStats(): Map<string, PlayStats> {
    const stats = loadPlayStats();
    const todayStart = new Date().setHours(0, 0, 0, 0);
    
    // Filter to items played today
    const todayStats = new Map<string, PlayStats>();
    for (const [id, stat] of stats) {
        if (stat.lastPlayed && stat.lastPlayed >= todayStart) {
            todayStats.set(id, stat);
        }
    }
    
    return todayStats;
}

// ============================================
// PROBABILISTIC SELECTION
// ============================================

/**
 * Calculate the selection weight for an ad based on:
 * - Base weight
 * - Delivery status vs target
 * - Separation rules
 * - Daypart restrictions
 * - Recency
 */
export function calculateWeight(
    item: TrafficItem,
    context: SelectionContext
): { weight: number; eligible: boolean; reason: string } {
    
    // Check if active
    if (!item.isActive || item.isPaused) {
        return { weight: 0, eligible: false, reason: 'Item inactive or paused' };
    }
    
    // Check flight dates
    if (item.notBefore && context.now < item.notBefore) {
        return { weight: 0, eligible: false, reason: 'Before flight start' };
    }
    if (item.notAfter && context.now > item.notAfter) {
        return { weight: 0, eligible: false, reason: 'After flight end' };
    }
    
    // Check daypart restrictions
    if (item.blockedDayparts?.includes(context.daypart)) {
        return { weight: 0, eligible: false, reason: `Blocked in ${context.daypart}` };
    }
    if (item.allowedDayparts && !item.allowedDayparts.includes(context.daypart)) {
        return { weight: 0, eligible: false, reason: `Not allowed in ${context.daypart}` };
    }
    
    // Check duration fits
    if (item.duration > context.availableDuration) {
        return { weight: 0, eligible: false, reason: 'Too long for available time' };
    }
    
    // Check separation (same item)
    if (item.minSeparation) {
        const lastPlay = context.recentPlays.find(p => p.mediaId === item.id);
        if (lastPlay) {
            const elapsed = (context.now - lastPlay.timestamp) / 1000;
            if (elapsed < item.minSeparation) {
                return { weight: 0, eligible: false, reason: `Separation: ${Math.ceil(item.minSeparation - elapsed)}s remaining` };
            }
        }
    }
    
    // Check category separation (competitive separation)
    if (item.category) {
        const categoryPlays = context.recentPlays.filter(p => {
            // Would need to look up category from item - simplified here
            return false; // TODO: implement category lookup
        });
    }
    
    // Check hourly maximum
    const stats = context.dayStats.get(item.id);
    if (item.targetPerHour && stats) {
        if (stats.thisHourPlays >= item.targetPerHour) {
            return { weight: 0, eligible: false, reason: 'Hourly maximum reached' };
        }
    }
    
    // Base weight
    let weight = item.baseWeight || 50;
    
    // Adjust for delivery status
    if (item.targetPlays && stats) {
        const deliveryRate = stats.totalPlays / item.targetPlays;
        
        if (deliveryRate < 0.5) {
            // Under 50% delivery - significant boost
            weight *= 2.0;
        } else if (deliveryRate < 0.8) {
            // Under 80% - moderate boost
            weight *= 1.5;
        } else if (deliveryRate < 1.0) {
            // Approaching target - slight boost
            weight *= 1.2;
        } else if (deliveryRate >= 1.0) {
            // At or over target - reduce weight
            weight *= 0.3;
        }
    }
    
    // Adjust for daily target
    if (item.targetPerDay && stats) {
        const dailyRate = stats.todayPlays / item.targetPerDay;
        
        if (dailyRate < 0.5) {
            weight *= 1.5;
        } else if (dailyRate >= 1.0) {
            weight *= 0.5;
        }
    }
    
    // Priority boost (manual override)
    if (item.priorityBoost) {
        weight *= (1 + item.priorityBoost / 100);
    }
    
    // Recency penalty (avoid playing same thing too often)
    if (stats?.lastPlayed) {
        const hoursSinceLast = (context.now - stats.lastPlayed) / (1000 * 60 * 60);
        if (hoursSinceLast < 1) {
            weight *= 0.7;
        } else if (hoursSinceLast < 4) {
            weight *= 0.9;
        }
    }
    
    // Cap at 5× base so no single item can monopolise a break regardless of boost stacking
    const maxWeight = (item.baseWeight || 50) * 5;
    return { weight: Math.min(Math.max(0, weight), maxWeight), eligible: true, reason: 'Eligible' };
}

/**
 * Select items to fill available time using weighted probability
 */
export function selectFillContent(
    pool: TrafficItem[],
    context: SelectionContext,
    count: number = 1
): SelectionResult[] {
    const results: SelectionResult[] = [];
    let remainingDuration = context.availableDuration;
    const usedIds = new Set<string>();
    
    for (let i = 0; i < count && remainingDuration > 0; i++) {
        // Calculate weights for all eligible items
        const candidates: { item: TrafficItem; weight: number; reason: string }[] = [];
        let totalWeight = 0;
        
        for (const item of pool) {
            if (usedIds.has(item.id)) continue;
            if (item.duration > remainingDuration) continue;
            
            const { weight, eligible, reason } = calculateWeight(item, {
                ...context,
                availableDuration: remainingDuration,
            });
            
            if (eligible && weight > 0) {
                candidates.push({ item, weight, reason });
                totalWeight += weight;
            }
        }
        
        if (candidates.length === 0 || totalWeight === 0) {
            break; // No eligible items
        }
        
        // Weighted random selection
        let random = Math.random() * totalWeight;
        let selected: typeof candidates[0] | null = null;
        
        for (const candidate of candidates) {
            random -= candidate.weight;
            if (random <= 0) {
                selected = candidate;
                break;
            }
        }
        
        if (!selected) {
            selected = candidates[candidates.length - 1];
        }
        
        const probability = selected.weight / totalWeight;
        
        results.push({
            item: selected.item,
            probability,
            reason: selected.reason,
            weight: selected.weight,
        });
        
        usedIds.add(selected.item.id);
        remainingDuration -= selected.item.duration;
    }
    
    return results;
}

// ============================================
// BREAK BUILDER
// ============================================

export interface BreakConfig {
    totalDuration: number;      // Total break length in seconds
    bumperIn?: TrafficItem;     // "Coming up" bumper
    bumperOut?: TrafficItem;    // "Back to..." bumper
    requireId?: boolean;        // Must include station ID?
    idPosition?: 'start' | 'end' | 'middle';
    minAds?: number;
    maxAds?: number;
}

export interface BreakContent {
    items: PlaylistItem[];
    totalDuration: number;
    adCount: number;
    fillTime: number;           // Time filled with ads
    remainingTime: number;      // Unfilled time
}

/**
 * Build a commercial break with probabilistic ad selection
 */
export function buildBreak(
    config: BreakConfig,
    adPool: TrafficItem[],
    context: SelectionContext
): BreakContent {
    const items: PlaylistItem[] = [];
    let availableTime = config.totalDuration;
    let adCount = 0;
    
    // Add bumper in
    if (config.bumperIn) {
        items.push(trafficToPlaylistItem(config.bumperIn));
        availableTime -= config.bumperIn.duration;
    }
    
    // Reserve time for bumper out
    if (config.bumperOut) {
        availableTime -= config.bumperOut.duration;
    }
    
    // Reserve time for station ID if required
    const idPool = adPool.filter(i => i.contentClass === 'id');
    let stationId: TrafficItem | null = null;
    if (config.requireId && idPool.length > 0) {
        stationId = idPool[Math.floor(Math.random() * idPool.length)];
        availableTime -= stationId.duration;
    }
    
    // Fill with ads
    const commercials = adPool.filter(i => i.contentClass === 'commercial');
    const selections = selectFillContent(commercials, {
        ...context,
        availableDuration: availableTime,
    }, config.maxAds || 10);
    
    for (const selection of selections) {
        if (adCount >= (config.minAds || 0) && availableTime < selection.item.duration) {
            break;
        }
        
        items.push(trafficToPlaylistItem(selection.item));
        availableTime -= selection.item.duration;
        adCount++;
        
        if (config.maxAds && adCount >= config.maxAds) {
            break;
        }
    }
    
    // Insert station ID at configured position
    if (stationId) {
        const idItem = trafficToPlaylistItem(stationId);
        switch (config.idPosition) {
            case 'start':
                items.splice(config.bumperIn ? 1 : 0, 0, idItem);
                break;
            case 'end':
                items.push(idItem);
                break;
            case 'middle':
            default:
                const midPoint = Math.floor(items.length / 2);
                items.splice(midPoint, 0, idItem);
                break;
        }
    }
    
    // Add bumper out
    if (config.bumperOut) {
        items.push(trafficToPlaylistItem(config.bumperOut));
    }
    
    const totalDuration = items.reduce((sum, i) => sum + (i.media?.duration || 0), 0);
    
    return {
        items,
        totalDuration,
        adCount,
        fillTime: config.totalDuration - availableTime,
        remainingTime: availableTime,
    };
}

function trafficToPlaylistItem(item: TrafficItem): PlaylistItem {
    return {
        id: `pl_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`,
        mediaId: item.id,
        media: item,
        isFixedTime: false,
    };
}

// ============================================
// SCHEDULE INTEGRATION
// ============================================

/**
 * Check if an item is hard-scheduled (program/hardware)
 */
export function isHardScheduled(item: TrafficItem | MediaItem): boolean {
    const contentClass = (item as TrafficItem).contentClass || item.type;
    return contentClass === 'program' || contentClass === 'hardware';
}

/**
 * Check if an item is probabilistic fill (ads, promos, etc.)
 */
export function isProbabilisticFill(item: TrafficItem | MediaItem): boolean {
    const contentClass = (item as TrafficItem).contentClass || item.type;
    return ['commercial', 'promo', 'psa', 'filler'].includes(contentClass as string);
}

/**
 * Get play count for an item
 */
export function getPlayCount(mediaId: string): number {
    const stats = getPlayStats(mediaId);
    return stats?.totalPlays || 0;
}

/**
 * Get today's play count for an item
 */
export function getTodayPlayCount(mediaId: string): number {
    const stats = getPlayStats(mediaId);
    return stats?.todayPlays || 0;
}

/**
 * Calculate probability of an item being selected next
 */
export function getSelectionProbability(
    item: TrafficItem,
    pool: TrafficItem[],
    context: SelectionContext
): number {
    // Calculate all weights
    let totalWeight = 0;
    let itemWeight = 0;
    
    for (const poolItem of pool) {
        const { weight, eligible } = calculateWeight(poolItem, context);
        if (eligible) {
            totalWeight += weight;
            if (poolItem.id === item.id) {
                itemWeight = weight;
            }
        }
    }
    
    if (totalWeight === 0) return 0;
    return itemWeight / totalWeight;
}



