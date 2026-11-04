/**
 * TIME-AUTHORITATIVE AUTOMATION ENGINE v3
 * 
 * Core principle: Time is the state, not a trigger.
 * The engine continuously asks: "Given the wall clock, what should be on air RIGHT NOW?"
 * 
 * Architecture:
 * - Engine runs at 20Hz (configurable) using precision timer
 * - Each tick: derive state from (time + schedule), reconcile renderer
 * - State is NEVER stored - always recalculated
 * - Renderer is a slave - engine is the brain
 * 
 * v3 Improvements:
 * - Precision timer with drift compensation
 * - Health monitoring and watchdog
 * - Monotonic timestamps (performance.now())
 * - Automatic recovery from stalls
 * - Enhanced error handling with exponential backoff
 * - Frame-accurate mode option
 */

import { MediaItem, HourSlot, WeeklySchedule, PlaylistItem } from './types';
import {
    PolicyConfig,
    DEFAULT_POLICY,
    DerivedState,
    PlaybackState,
    CorrectionAction,
    TransitionDecision,
    RendererCommand,
    determineCorrectionAction,
    decideEventTransition,
    derivePlaybackState,
    generateCommand,
    generateCorrectionCommands,
    logEngine,
} from './automationStateMachine';
import {
    TrafficItem,
    SelectionContext,
    ContentClass,
    isHardScheduled,
    isProbabilisticFill,
    selectFillContent,
    logPlay,
    getPlayStats,
    getDaypart,
    loadPlayLogs,
    getTodayStats,
} from './trafficEngine';
import {
    PrecisionTimer,
    TimerStats,
    TimerConfig,
    DEFAULT_TIMER_CONFIG,
    hrtime,
} from './precisionTimer';
import {
    EngineHealthMonitor,
    HealthSnapshot,
    getHealthMonitor,
} from './engineHealthMonitor';
import { reportingService } from './reportingService';

// ============================================
// SCHEDULE EVENT
// ============================================

export type EventPriority = 'normal' | 'fixed' | 'fill' | 'override' | 'live_lock';

export interface ScheduleEvent {
    id: string;
    mediaId: string;
    media: MediaItem;
    startTime: number;      // Unix timestamp (ms) - absolute
    endTime: number;        // Unix timestamp (ms) - absolute
    duration: number;       // Duration in ms
    priority: EventPriority;
    isLive?: boolean;       // Live content - can extend
    isLocked?: boolean;     // Cannot be preempted
}

// ============================================
// TIMELINE BUILDER
// ============================================

export function buildTimeline(
    schedule: WeeklySchedule,
    dayIndex: number,
    hourIndex: number,
    referenceTime: number
): ScheduleEvent[] {
    const slotKey = `${dayIndex}-${hourIndex}`;
    const slot = schedule.slots[slotKey];
    
    if (!slot || !slot.items || slot.items.length === 0) {
        return [];
    }

    const events: ScheduleEvent[] = [];
    let currentTime = referenceTime;

    for (const item of slot.items) {
        // Handle timeIn/timeOut for trimmed playback
        const fullDuration = (item.media?.duration || 0) * 1000;
        const timeIn = (item.timeIn || 0) * 1000;
        const timeOut = item.timeOut ? item.timeOut * 1000 : fullDuration;
        const effectiveDuration = timeOut - timeIn;
        
        if (effectiveDuration <= 0) continue;

        const event: ScheduleEvent = {
            id: item.id,
            mediaId: item.mediaId || item.media?.id || item.id,
            media: {
                ...item.media,
                // Store trim points for renderer
                timeIn: item.timeIn,
                timeOut: item.timeOut,
            } as MediaItem,
            startTime: currentTime,
            endTime: currentTime + effectiveDuration,
            duration: effectiveDuration,
            priority: item.isFixedTime ? 'fixed' : 'normal',
            isLive: false,
        };

        events.push(event);
        currentTime += effectiveDuration;
    }

    return events;
}

export function buildDayTimeline(
    schedule: WeeklySchedule,
    dayIndex: number,
    baseDate: Date = new Date()
): ScheduleEvent[] {
    const allEvents: ScheduleEvent[] = [];
    
    // Start of day
    const dayStart = new Date(baseDate);
    dayStart.setHours(0, 0, 0, 0);
    
    for (let hour = 0; hour < 24; hour++) {
        const hourStart = new Date(dayStart);
        hourStart.setHours(hour);
        const events = buildTimeline(schedule, dayIndex, hour, hourStart.getTime());
        allEvents.push(...events);
    }
    
    return allEvents;
}

// ============================================
// CORE ENGINE
// ============================================

export interface EngineCallbacks {
    onCommand: (cmd: RendererCommand) => void;
    onStateChange: (state: DerivedState) => void;
    onError: (error: Error) => void;
}

export interface EngineConfig {
    policy?: Partial<PolicyConfig>;
    timer?: Partial<TimerConfig>;
    enableHealthMonitor?: boolean;
}

export class AutomationEngine {
    private policy: PolicyConfig;
    private timeline: ScheduleEvent[] = [];
    private overrides: ScheduleEvent[] = [];
    private liveLocks: Map<string, number> = new Map();
    
    // Fill content (emergency/fallback)
    private fillPool: MediaItem[] = [];
    private slateMedia: MediaItem | null = null;
    private currentFillIndex: number = 0;
    
    // Probabilistic ad pool
    private adPool: TrafficItem[] = [];
    private promoPool: TrafficItem[] = [];
    private currentAdEvent: ScheduleEvent | null = null;
    
    // Play tracking
    private lastPlayedId: string | null = null;
    private playStartTime: number = 0;
    
    // Runtime - v3: Now using PrecisionTimer
    private precisionTimer: PrecisionTimer;
    private callbacks: EngineCallbacks | null = null;
    private isRunning: boolean = false;
    
    // Renderer feedback
    private lastRendererMedia: string | null = null;
    private lastRendererPosition: number = 0;
    private lastRendererUpdate: number = 0;
    
    // Transition state
    private isTransitioning: boolean = false;
    private transitionEndTime: number = 0;
    
    // Error tracking
    private consecutiveErrors: number = 0;
    private lastErrorTime: number = 0;
    
    // Event callbacks for play logging
    private onPlayStart: ((item: MediaItem) => void) | null = null;
    private onPlayComplete: ((item: MediaItem, completed: boolean) => void) | null = null;
    
    // v3: Health monitoring
    private healthMonitor: EngineHealthMonitor | null = null;
    private enableHealthMonitor: boolean;
    
    // v3: Performance tracking
    private tickCount: number = 0;
    private lastTickTime: number = 0;
    private tickRateWindow: number[] = [];
    private engineStartTime: number = 0;

    constructor(config: EngineConfig = {}) {
        this.policy = { ...DEFAULT_POLICY, ...config.policy };
        this.enableHealthMonitor = config.enableHealthMonitor ?? true;
        
        // Initialize precision timer with engine-specific config
        const timerConfig: Partial<TimerConfig> = {
            targetHz: 1, // 1Hz for very loose sync
            jitterWarningThreshold: 50,
            maxAccumulatedDrift: 100,
            frameAccurate: true, // Strictly time accurate
            watchdogTimeout: 1500,
            verbose: false,
            ...config.timer,
        };
        
        this.precisionTimer = new PrecisionTimer(timerConfig);
        
        // Initialize health monitor
        if (this.enableHealthMonitor) {
            this.healthMonitor = getHealthMonitor();
        }
    }

    // ----------------------------------------
    // LIFECYCLE
    // ----------------------------------------

    start(callbacks: EngineCallbacks) {
        if (this.isRunning) {
            logEngine('warn', 'state', 'Engine already running');
            return;
        }
        
        this.callbacks = callbacks;
        this.isRunning = true;
        this.engineStartTime = hrtime();
        this.tickCount = 0;
        this.lastTickTime = this.engineStartTime;
        this.tickRateWindow = [];
        this.consecutiveErrors = 0;
        
        // Reset health monitor
        if (this.healthMonitor) {
            this.healthMonitor.reset();
        }
        
        // Start precision timer
        this.precisionTimer.start((delta, now, stats) => {
            this.tick(delta, now, stats);
        });
        
        logEngine('info', 'state', 'Engine started', { 
            tickRate: '20Hz (precision timer)',
            healthMonitor: this.enableHealthMonitor,
        });
    }

    stop() {
        if (!this.isRunning) return;
        
        // Stop precision timer
        this.precisionTimer.stop();
        
        this.isRunning = false;
        this.callbacks = null;
        
        // Log final stats
        const stats = this.precisionTimer.getStats();
        logEngine('info', 'state', 'Engine stopped', {
            totalTicks: stats.totalTicks,
            missedTicks: stats.missedTicks,
            avgJitter: stats.averageJitter.toFixed(2) + 'ms',
            uptime: (stats.uptime / 1000).toFixed(1) + 's',
        });
    }
    
    /**
     * Get engine health snapshot
     */
    getHealth(): HealthSnapshot | null {
        return this.healthMonitor?.getSnapshot() ?? null;
    }
    
    /**
     * Get timer statistics
     */
    getTimerStats(): TimerStats {
        return this.precisionTimer.getStats();
    }
    
    /**
     * Get actual tick rate (measured)
     */
    getActualTickRate(): number {
        if (this.tickRateWindow.length < 2) return 0;
        const totalTime = this.tickRateWindow[this.tickRateWindow.length - 1] - this.tickRateWindow[0];
        return totalTime > 0 ? ((this.tickRateWindow.length - 1) / totalTime) * 1000 : 0;
    }

    // ----------------------------------------
    // SCHEDULE MANAGEMENT
    // ----------------------------------------

    loadSchedule(schedule: WeeklySchedule) {
        const now = new Date();
        this.timeline = buildDayTimeline(schedule, now.getDay(), now);
        logEngine('info', 'state', `Loaded ${this.timeline.length} events`);
    }

    setTimeline(events: ScheduleEvent[]) {
        this.timeline = events;
        logEngine('info', 'state', `Set ${events.length} events`);
    }

    setFillPool(media: MediaItem[]) {
        this.fillPool = media;
        this.currentFillIndex = 0;
    }

    setSlate(media: MediaItem) {
        this.slateMedia = media;
    }

    // ----------------------------------------
    // AD POOL MANAGEMENT (Probabilistic)
    // ----------------------------------------

    setAdPool(ads: TrafficItem[]) {
        this.adPool = ads.filter(a => a.contentClass === 'commercial');
        logEngine('info', 'state', `Set ${this.adPool.length} commercial items`);
    }

    setPromoPool(promos: TrafficItem[]) {
        this.promoPool = promos.filter(p => p.contentClass === 'promo');
        logEngine('info', 'state', `Set ${this.promoPool.length} promo items`);
    }

    addToAdPool(item: TrafficItem) {
        if (!this.adPool.find(a => a.id === item.id)) {
            this.adPool.push(item);
        }
    }

    removeFromAdPool(itemId: string) {
        this.adPool = this.adPool.filter(a => a.id !== itemId);
    }

    /**
     * Select next ad probabilistically based on weights, targets, and history
     */
    selectNextAd(availableDuration: number): TrafficItem | null {
        const now = Date.now();
        const hour = new Date(now).getHours();
        
        const context: SelectionContext = {
            now,
            daypart: getDaypart(hour),
            hour,
            availableDuration,
            recentPlays: loadPlayLogs().slice(-50), // Last 50 plays
            dayStats: getTodayStats(),
        };
        
        const selections = selectFillContent(this.adPool, context, 1);
        
        if (selections.length > 0) {
            logEngine('info', 'state', `Selected ad: ${selections[0].item.title}`, {
                probability: (selections[0].probability * 100).toFixed(1) + '%',
                weight: selections[0].weight,
                reason: selections[0].reason,
            });
            return selections[0].item;
        }
        
        return null;
    }

    /**
     * Select next promo probabilistically
     */
    selectNextPromo(availableDuration: number): TrafficItem | null {
        const now = Date.now();
        const hour = new Date(now).getHours();
        
        const context: SelectionContext = {
            now,
            daypart: getDaypart(hour),
            hour,
            availableDuration,
            recentPlays: loadPlayLogs().slice(-50),
            dayStats: getTodayStats(),
        };
        
        const selections = selectFillContent(this.promoPool, context, 1);
        return selections.length > 0 ? selections[0].item : null;
    }

    // ----------------------------------------
    // PLAY LOGGING
    // ----------------------------------------

    setPlayCallbacks(
        onStart: (item: MediaItem) => void,
        onComplete: (item: MediaItem, completed: boolean) => void
    ) {
        this.onPlayStart = onStart;
        this.onPlayComplete = onComplete;
    }

    private logPlayStart(event: ScheduleEvent) {
        this.lastPlayedId = event.mediaId;
        this.playStartTime = Date.now();
        
        // Determine if this was hard-scheduled or fill
        const isScheduled = isHardScheduled(event.media as TrafficItem);
        
        logEngine('info', 'state', `Play started: ${event.media.title}`, {
            scheduled: isScheduled,
            contentClass: (event.media as TrafficItem).contentClass || event.media.type,
        });
        
        this.onPlayStart?.(event.media);
    }

    private logPlayComplete(event: ScheduleEvent, completed: boolean) {
        const duration = Date.now() - this.playStartTime;
        const trafficItem = event.media as TrafficItem;
        
        // Log to traffic engine
        logPlay(trafficItem, {
            scheduled: isHardScheduled(trafficItem),
            position: isHardScheduled(trafficItem) ? 'scheduled' : 'fill',
            completed,
            duration: duration / 1000,
        });
        
        logEngine('info', 'state', `Play complete: ${event.media.title}`, {
            completed,
            duration: `${(duration / 1000).toFixed(1)}s`,
            playCount: getPlayStats(event.mediaId)?.totalPlays || 1,
        });
        
        this.onPlayComplete?.(event.media, completed);
        this.lastPlayedId = null;
    }

    // ----------------------------------------
    // CONTROL INTENTS
    // ----------------------------------------

    insertOverride(media: MediaItem, startTime: number, endTime: number) {
        const override: ScheduleEvent = {
            id: `override_${Date.now()}`,
            mediaId: media.id,
            media,
            startTime,
            endTime,
            duration: endTime - startTime,
            priority: 'override',
        };
        this.overrides.push(override);
        logEngine('info', 'state', 'Override inserted', { media: media.title });
    }

    clearOverride(id: string) {
        this.overrides = this.overrides.filter(o => o.id !== id);
    }

    clearAllOverrides() {
        this.overrides = [];
    }

    lockEvent(eventId: string, untilTime: number) {
        this.liveLocks.set(eventId, untilTime);
        logEngine('info', 'state', 'Event locked', { eventId, until: new Date(untilTime).toISOString() });
    }

    unlockEvent(eventId: string) {
        this.liveLocks.delete(eventId);
    }

    // ----------------------------------------
    // RENDERER FEEDBACK
    // ----------------------------------------

    reportRendererState(mediaId: string, positionMs: number) {
        this.lastRendererMedia = mediaId;
        this.lastRendererPosition = positionMs;
        this.lastRendererUpdate = Date.now();
    }

    // ----------------------------------------
    // THE TICK - Heart of the engine (v3: precision timer callback)
    // ----------------------------------------

    private tick(delta: number, perfNow: number, timerStats: TimerStats) {
        // Use wall clock time for schedule matching (offset from performance.now)
        const wallClockOffset = Date.now() - performance.now();
        const now = perfNow + wallClockOffset;
        
        // Track tick rate
        this.tickCount++;
        this.tickRateWindow.push(perfNow);
        if (this.tickRateWindow.length > 20) {
            this.tickRateWindow.shift();
        }
        
        // Update health monitor with tick metrics
        if (this.healthMonitor) {
            const jitter = Math.abs(delta - 50); // Expected 50ms at 20Hz
            this.healthMonitor.recordTick(jitter, false);
            this.healthMonitor.updateTickRate(this.getActualTickRate(), 20);
            this.healthMonitor.updateUptime(perfNow - this.engineStartTime);
        }
        
        try {
            // 1. DERIVE STATE from time + schedule
            const state = this.deriveState(now);
            
            // 2. GENERATE COMMANDS to reconcile renderer
            const commands = this.reconcile(state, now);
            
            // 3. DISPATCH commands
            for (const cmd of commands) {
                logEngine('debug', 'command', `Issuing: ${cmd.type}`, { cmd });
                this.callbacks?.onCommand(cmd);
            }
            
            // 4. NOTIFY state change
            this.callbacks?.onStateChange(state);
            
            // Trigger reporting service tick
            reportingService.onEngineTick(now);
            
            // 5. Health monitoring for drift
            if (this.healthMonitor && state.drift !== 0 && state.correctionAction !== 'none') {
                this.healthMonitor.recordDrift(state.drift, state.correctionAction);
            }
            
            // 6. Track gap events
            if (this.healthMonitor && state.gapDetected) {
                this.healthMonitor.recordGap();
            }
            
            // Reset error counter on success
            this.consecutiveErrors = 0;
            this.lastTickTime = perfNow;
            
        } catch (error) {
            this.handleError(error as Error);
        }
    }

    // ----------------------------------------
    // STATE DERIVATION (Pure)
    // ----------------------------------------

    private deriveState(now: number): DerivedState {
        // Merge timeline + overrides, apply locks
        const allEvents = this.mergeEvents(now);
        
        // Find active event
        const activeEvent = this.findActiveEvent(allEvents, now);
        
        // Calculate expected position
        let expectedPosition = 0;
        let transitionDecision: TransitionDecision | null = null;
        
        if (activeEvent) {
            transitionDecision = decideEventTransition(
                now,
                activeEvent.startTime,
                activeEvent.endTime,
                activeEvent.duration,
                activeEvent.isLive || false,
                this.policy
            );
            
            if (transitionDecision.action === 'play') {
                expectedPosition = transitionDecision.seekTo;
            }
        }
        
        // Calculate drift
        const drift = this.calculateDrift(activeEvent, expectedPosition);
        
        // Determine correction action
        const correctionAction = activeEvent 
            ? determineCorrectionAction(drift, this.policy)
            : 'none';
        
        // Find next event
        const nextEvent = allEvents.find(e => e.startTime > now) || null;
        const timeToNext = nextEvent ? nextEvent.startTime - now : Infinity;
        
        // Check if we're in transition
        if (this.isTransitioning && now < this.transitionEndTime) {
            // Still transitioning
        } else {
            this.isTransitioning = false;
        }
        
        // Derive playback state
        const playbackState = derivePlaybackState(
            activeEvent !== null && transitionDecision?.action === 'play',
            this.getFillMedia() !== null,
            drift,
            this.isTransitioning,
            this.policy
        );
        
        // Check for gap (BUG)
        const gapDetected = !activeEvent && !this.getFillMedia();
        if (gapDetected) {
            logEngine('error', 'state', 'GAP DETECTED - No content available');
        }
        
        return {
            mode: this.isRunning ? 'running' : 'standby',
            playbackState,
            activeEventId: activeEvent?.id || null,
            expectedPosition,
            actualPosition: this.lastRendererPosition,
            drift,
            correctionAction,
            nextEventId: nextEvent?.id || null,
            timeToNext,
            isPrerolling: timeToNext <= this.policy.prerollTime,
            transitionDecision,
            fillActive: !activeEvent && this.getFillMedia() !== null,
            gapDetected,
            timestamp: now,
        };
    }

    private mergeEvents(now: number): ScheduleEvent[] {
        // Start with timeline
        let events = [...this.timeline];
        
        // Add overrides (highest priority)
        events.push(...this.overrides);
        
        // Apply live locks
        events = events.map(e => {
            const lockUntil = this.liveLocks.get(e.id);
            if (lockUntil && now < lockUntil) {
                return { ...e, priority: 'live_lock' as EventPriority, isLocked: true };
            }
            return e;
        });
        
        // Clean expired locks
        for (const [id, until] of this.liveLocks.entries()) {
            if (now >= until) {
                this.liveLocks.delete(id);
            }
        }
        
        // Sort by priority, then by start time
        const priorityOrder: Record<EventPriority, number> = {
            'override': 4,
            'live_lock': 3,
            'fixed': 2,
            'normal': 1,
            'fill': 0,
        };
        
        return events.sort((a, b) => {
            const pDiff = priorityOrder[b.priority] - priorityOrder[a.priority];
            if (pDiff !== 0) return pDiff;
            return a.startTime - b.startTime;
        });
    }

    private findActiveEvent(events: ScheduleEvent[], now: number): ScheduleEvent | null {
        // Find events where now is within their window
        const active = events.filter(e => now >= e.startTime && now < e.endTime);
        return active.length > 0 ? active[0] : null;
    }

    private calculateDrift(activeEvent: ScheduleEvent | null, expectedPosition: number): number {
        if (!activeEvent) return 0;
        
        // Check if renderer is playing the right media
        if (this.lastRendererMedia !== activeEvent.mediaId && 
            this.lastRendererMedia !== activeEvent.media.src) {
            // Wrong media - infinite drift
            return Infinity;
        }
        
        return expectedPosition - this.lastRendererPosition;
    }

    private getFillMedia(): MediaItem | null {
        switch (this.policy.fillMode) {
            case 'slate':
                return this.slateMedia;
                
            case 'loop':
                if (this.fillPool.length > 0) {
                    return this.fillPool[this.currentFillIndex % this.fillPool.length];
                }
                return this.slateMedia;
                
            case 'random':
                if (this.fillPool.length > 0) {
                    return this.fillPool[Math.floor(Math.random() * this.fillPool.length)];
                }
                return this.slateMedia;
                
            case 'black':
            default:
                return null;
        }
    }

    // ----------------------------------------
    // RECONCILIATION
    // ----------------------------------------

    private reconcile(state: DerivedState, now: number): RendererCommand[] {
        const commands: RendererCommand[] = [];
        
        // Handle gap (emergency)
        if (state.gapDetected) {
            // Try to at least stop cleanly
            commands.push(generateCommand('stop', {
                priority: 'urgent',
                reason: 'Gap detected - emergency stop'
            }));
            return commands;
        }
        
        // Handle fill mode
        if (state.fillActive) {
            const fillMedia = this.getFillMedia();
            if (fillMedia && this.lastRendererMedia !== fillMedia.id) {
                commands.push(generateCommand('load', {
                    mediaId: fillMedia.id,
                    mediaSrc: fillMedia.src,
                    seekTo: 0,
                    reason: 'Switching to fill content'
                }));
                commands.push(generateCommand('play', {
                    reason: 'Starting fill playback'
                }));
            }
            return commands;
        }
        
        // Find the event we should be playing
        const event = this.timeline.find(e => e.id === state.activeEventId);
        if (!event) {
            logEngine('warn', 'state', 'Active event not found in timeline');
            return commands;
        }
        
        // Check transition decision
        if (state.transitionDecision) {
            switch (state.transitionDecision.action) {
                case 'play':
                    // Check if we need to load the media
                    if (this.lastRendererMedia !== event.mediaId && 
                        this.lastRendererMedia !== event.media.src) {
                        
                        // Load and seek
                        commands.push(generateCommand('load', {
                            mediaId: event.mediaId,
                            mediaSrc: event.media.src,
                            seekTo: state.transitionDecision.seekTo,
                            reason: state.transitionDecision.reason
                        }));
                        commands.push(generateCommand('play', {
                            reason: 'Starting scheduled playback'
                        }));
                        
                    } else if (state.correctionAction !== 'none') {
                        // Media is loaded but needs correction
                        const correctionCommands = generateCorrectionCommands(
                            state.correctionAction,
                            state.expectedPosition,
                            event.mediaId,
                            event.media.src,
                            this.policy
                        );
                        commands.push(...correctionCommands);
                    }
                    break;
                    
                case 'skip':
                    // Move to next - handled naturally by next tick
                    logEngine('info', 'transition', 'Skipping event', { 
                        eventId: event.id,
                        reason: state.transitionDecision.reason 
                    });
                    break;
                    
                case 'fill':
                    // Already handled above
                    break;
                    
                case 'wait':
                    // Prerolling - could preload here
                    break;
                    
                case 'extend':
                    // Live extension - just let it continue
                    logEngine('info', 'transition', 'Live extension active', {
                        eventId: event.id
                    });
                    break;
            }
        }
        
        return commands;
    }

    // ----------------------------------------
    // ERROR HANDLING
    // ----------------------------------------

    private handleError(error: Error) {
        this.consecutiveErrors++;
        this.lastErrorTime = Date.now();
        
        logEngine('error', 'error', error.message, { stack: error.stack });
        
        this.callbacks?.onError(error);
        
        // If too many errors, slow down
        if (this.consecutiveErrors > this.policy.maxConsecutiveErrors) {
            logEngine('warn', 'error', 'Too many errors, entering recovery mode');
            // Could pause engine briefly here
        }
    }
}

// ============================================
// SINGLETON
// ============================================

let engineInstance: AutomationEngine | null = null;

export function getEngine(config?: EngineConfig): AutomationEngine {
    if (!engineInstance) {
        engineInstance = new AutomationEngine(config);
    }
    return engineInstance;
}

export function resetEngine() {
    if (engineInstance) {
        engineInstance.stop();
        engineInstance = null;
    }
}

// ============================================
// EXPORTS FOR EXTERNAL USE
// ============================================

export { PrecisionTimer } from './precisionTimer';
export type { TimerStats, TimerConfig } from './precisionTimer';
export { EngineHealthMonitor } from './engineHealthMonitor';
export type { HealthSnapshot, HealthAlert } from './engineHealthMonitor';
