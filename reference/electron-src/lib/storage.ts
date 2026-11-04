// ============================================
// PLAY PLAY - Storage Utilities
// LocalStorage persistence for app state
// ============================================

import {
    AppState,
    WeeklySchedule,
    FreePlaylist,
    AdClient,
    CGElement,
    CGPreset,
    AutomationSettings,
    MediaStats,
    PlayEvent,
    HourSlot,
    OBSCommand,
    MediaItem,
    PlaylistItem,
    Campaign,
    CGTemplatePreset,
    CGHistoryItem,
    ExtendedSettings,
    OBSSession,
    SiteState,
    AsPlayedEntry,
} from './types';
import { normalizeMediaPriorityFields } from './mediaPriority';
import { DEFAULT_ENGINE_APPROACHES } from './engineApproaches';
import { resolveFileUrlSync, getEnvironment } from './fileResolver';
import { persistentLogger } from './persistentLogger';

const STORAGE_KEYS = {
    APP_STATE: 'playplay_state',
    WEEKLY_SCHEDULE: 'playplay_weekly',
    FREE_PLAYLISTS: 'playplay_playlists',
    CLIENTS: 'playplay_clients',
    LIBRARY_MEDIA: 'playplay_library_media',
    CG_ELEMENTS: 'playplay_cg',
    CG_PRESETS: 'playplay_cg_presets',
    SETTINGS: 'playplay_settings',
    MEDIA_STATS: 'playplay_stats',
    OBS_COMMANDS: 'playplay_obs_commands',
    LOCAL_MEDIA: 'playplay_local_media',
    CAMPAIGNS: 'playplay_campaigns',
    CG_TEMPLATE_PRESETS: 'playplay_cg_template_presets',
    CG_HISTORY: 'playplay_cg_history',
    CONNECTION_SETTINGS: 'connection_settings',
    ENGINE_PATHS: 'engine_paths',
    SITE_STATES: 'playplay_site_states',
    SITE_VISIBILITY: 'playplay_site_visibility',
    AS_PLAYED_LOG: 'playplay_as_played',
} as const;

// --- DEFAULT VALUES ---

export const DEFAULT_SETTINGS: AutomationSettings = {
    alwaysPlayLatestEpisode: true,
    shuffleAdsInHour: true,
    fillGapsWithIds: true,
    fillGapsWithBumpers: true,
    maxFillDuration: 60,
    autoAdvance: true,
    loopAtEndOfSchedule: false,
    defaultCGAnimationIn: 'fade',
    defaultCGAnimationOut: 'fade',
    defaultCGDuration: 5000,
    trackPlayHistory: true,
    maxHistoryDays: 30,
    facebookWebhookUrl: '',
    facebookWebhookEnabled: true,
    facebookGraphEnabled: false,
    facebookGraphPageId: '',
    facebookGraphAccessToken: '',
    facebookPreferGraph: true,
    openaiApiKey: '', // OpenAI API key for AI-assisted content generation
    ttsProvider: 'auto',
    kokoroTtsBaseUrl: 'http://127.0.0.1:8880',
    kokoroVoice: 'af_heart',
    sttProvider: 'auto',
    localWhisperBaseUrl: 'http://127.0.0.1:9000',
    ttsAnnouncementsEnabled: false,
    ttsAnnounceHourlyTime: false,
    ttsWarnNoContent3Min: true,
    ttsAnnounceQueuedStarts: false,
    engineApproaches: { ...DEFAULT_ENGINE_APPROACHES },
};

export const DEFAULT_SCHEDULING_RULE = {
    allowedHours: [],
    allowedDays: [],
    maxPlaysPerHour: 0,
    maxPlaysPerDay: 0,
    minIntervalMinutes: 0,
    priority: 5,
};

// --- UTILITY FUNCTIONS ---

/**
 * Convert a file path to a proper file:// URL
 * Handles Windows paths correctly (C:\Users\... -> file:///C:/Users/...)
 */
function pathToFileUrl(filePath: string): string {
    if (!filePath) return '';

    // If already a file:// URL, return as-is
    if (filePath.startsWith('file://')) {
        return filePath;
    }

    // Convert backslashes to forward slashes
    let normalized = filePath.replace(/\\/g, '/');

    // Windows drive letter handling: C:/ -> /C:/
    // Match patterns like "C:/" or "C:\" (already converted to "C:/")
    const isWindowsPath = /^[A-Za-z]:/.test(normalized);
    if (isWindowsPath) {
        normalized = '/' + normalized;
    }

    // URL encode the path (handles spaces and special chars)
    // But preserve the drive letter format (C:)
    const parts = normalized.split('/');
    const encodedParts = parts.map((part, index) => {
        // Don't encode the first empty part
        if (index === 0 && part === '') return '';
        // Don't encode drive letter (e.g., "C:")
        if (index === 1 && isWindowsPath && /^[A-Za-z]:$/.test(part)) {
            return part;
        }
        // Encode everything else
        return encodeURIComponent(part);
    });

    // Reconstruct with proper separators
    const encodedPath = encodedParts.join('/');

    // Windows paths already have leading slash, use file://
    // Unix paths need file:///
    if (encodedPath.startsWith('/')) {
        return `file://${encodedPath}`;
    } else {
        return `file:///${encodedPath}`;
    }
}

function safeJsonParse<T>(json: string | null, fallback: T): T {
    if (!json) return fallback;
    try {
        return JSON.parse(json) as T;
    } catch {
        return fallback;
    }
}

function generateId(): string {
    return `${Date.now().toString(36)}_${Math.random().toString(36).substr(2, 9)}`;
}

// Ensure MediaItem.src is a valid URL when we have a persisted filePath.
// Uses the universal file resolver for consistent cross-platform handling.
function normalizeMediaSrc(media: MediaItem): MediaItem {
    if (!media) return media;

    try {
        const env = getEnvironment();

        // If we have a real file path and we're in Electron, use universal resolver
        if (media.filePath && env.isElectron) {
            const src = resolveFileUrlSync(media.filePath);
            if (src) {
                return {
                    ...media,
                    src,
                };
            }
        }

        // If src is a non-blob, non-empty value, keep it
        if (media.src && !media.src.startsWith('blob:')) {
            return media;
        }

        // Dead blob or no src - use test pattern fallback so playback doesn't break
        // The rehydration in PlayoutController will try harder to find the real source
        console.warn('[storage] Media has dead/missing src, using fallback:', media.title);
        return {
            ...media,
            src: 'signal:bars',
            title: media.title ? `⚠ ${media.title}` : '⚠ Missing Media',
        };
    } catch {
        // If anything goes wrong (e.g., window not available), just return media as-is.
        return media;
    }
}

function normalizePlaylistItems(items: PlaylistItem[]): PlaylistItem[] {
    if (!items || items.length === 0) return items;
    return items.map(item => ({
        ...item,
        media: normalizeMediaSrc(item.media),
    }));
}

// --- WEEKLY SCHEDULE ---

export function createEmptyWeeklySchedule(): WeeklySchedule {
    const slots: Record<string, HourSlot> = {};

    for (let day = 0; day < 7; day++) {
        for (let hour = 0; hour < 24; hour++) {
            const id = `${day}-${hour}`;
            slots[id] = {
                id,
                dayOfWeek: day,
                hour,
                items: [],
                isLooping: false,
                randomizeAds: true,
                keepProgramOrder: true,
            };
        }
    }

    return {
        slots,
        version: 1,
        lastModified: Date.now(),
    };
}

/**
 * Resolve the storage key for the active channel's schedule. The renderer sets
 * `window.__activeChannelId` whenever the active channel changes (see
 * PlayoutController). 'ch-1' uses the legacy unsuffixed key + DB path so
 * existing data is preserved.
 */
function activeChannelKey(): string {
    const id = (typeof window !== 'undefined' && (window as any).__activeChannelId) || null;
    if (!id || id === 'ch-1' || id === 'default') return STORAGE_KEYS.WEEKLY_SCHEDULE;
    return STORAGE_KEYS.WEEKLY_SCHEDULE + '__' + String(id).replace(/[^A-Za-z0-9_-]/g, '_');
}

export async function loadWeeklySchedule(forceRefresh: boolean = false): Promise<WeeklySchedule> {
    // For non-default channels, route entirely to per-channel localStorage so
    // ch-2, ch-3 etc. each have their own grid without stomping ch-1's DB rows.
    const id = (typeof window !== 'undefined' && (window as any).__activeChannelId) || null;
    if (id && id !== 'ch-1' && id !== 'default') {
        const stored = localStorage.getItem(activeChannelKey());
        const schedule = safeJsonParse(stored, createEmptyWeeklySchedule());
        const normalizedSlots: Record<string, HourSlot> = {};
        Object.entries(schedule.slots || {}).forEach(([k, slot]) => {
            normalizedSlots[k] = { ...slot, items: normalizePlaylistItems(slot.items || []) };
        });
        return { ...schedule, slots: normalizedSlots };
    }

    // Default channel (ch-1): preserve existing DB-first path
    // Try DB first (Electron), fallback to localStorage
    const w = typeof window !== 'undefined' ? (window as any) : null;
    if (w?.electronAPI?.scheduleGetAllSlots) {
        try {
            const { dbGetAllScheduleSlots, dbGetScheduleMeta } = await import('./scheduleDbClient');
            // Always get fresh data from DB (no caching)
            const slots = await dbGetAllScheduleSlots();
            const meta = await dbGetScheduleMeta();

            const schedule: WeeklySchedule = {
                slots: {},
                version: parseInt(meta.version || '1', 10),
                lastModified: parseInt(meta.lastModified || '0', 10),
            };

            slots.forEach(slot => {
                schedule.slots[slot.id] = {
                    ...slot,
                    items: normalizePlaylistItems(slot.items || []),
                };
            });

            return schedule;
        } catch (e) {
            console.error('[Storage] Failed to load schedule from DB, falling back to localStorage:', e);
        }
    }

    // Fallback to localStorage
    const stored = localStorage.getItem(STORAGE_KEYS.WEEKLY_SCHEDULE);
    const schedule = safeJsonParse(stored, createEmptyWeeklySchedule());

    // Rebuild stable media.src for all items so schedules stay valid across sessions.
    const normalizedSlots: Record<string, HourSlot> = {};
    Object.entries(schedule.slots || {}).forEach(([id, slot]) => {
        normalizedSlots[id] = {
            ...slot,
            items: normalizePlaylistItems(slot.items || []),
        };
    });

    return {
        ...schedule,
        slots: normalizedSlots,
    };
}

export async function saveWeeklySchedule(schedule: WeeklySchedule, channelId?: string): Promise<void> {
    // Create a copy to avoid mutating the original object (important for React state)
    const scheduleToSave: WeeklySchedule = {
        ...schedule,
        lastModified: Date.now(),
        version: (schedule.version || 0) + 1,
    };

    // Save queue ensures we never overlap schedule writes.
    const runPersist = async () => {
        // Channel-aware key: non-default channels write to their own slot,
        // ch-1 / default writes to the shared key + the DB.
        const key = channelId
            ? STORAGE_KEYS.WEEKLY_SCHEDULE + '__' + String(channelId).replace(/[^A-Za-z0-9_-]/g, '_')
            : activeChannelKey();
        localStorage.setItem(key, JSON.stringify(scheduleToSave));

        // Skip DB write for non-default channels — they're isolated to localStorage
        const isDefault = key === STORAGE_KEYS.WEEKLY_SCHEDULE;
        if (!isDefault) return;

        // Try DB first (Electron), with retries.
        const w = typeof window !== 'undefined' ? (window as any) : null;
        if (!w?.electronAPI?.scheduleGetAllSlots) return;

        const { dbSaveWeeklySchedule, dbGetScheduleMeta } = await import('./scheduleDbClient');
        const maxAttempts = 3;
        let lastError: unknown = null;

        for (let attempt = 1; attempt <= maxAttempts; attempt++) {
            try {
                await dbSaveWeeklySchedule(scheduleToSave);
                const persistedMeta = await dbGetScheduleMeta();
                if (String(persistedMeta.lastModified || '') !== String(scheduleToSave.lastModified)) {
                    throw new Error('Post-save verification failed (lastModified mismatch)');
                }
                return;
            } catch (e) {
                lastError = e;
                if (attempt < maxAttempts) {
                    await new Promise((resolve) => setTimeout(resolve, attempt * 150));
                }
            }
        }

        throw lastError instanceof Error ? lastError : new Error(String(lastError));
    };

    const globalObj = globalThis as typeof globalThis & {
        __playplayScheduleSaveQueue?: Promise<void>;
    };
    if (!globalObj.__playplayScheduleSaveQueue) {
        globalObj.__playplayScheduleSaveQueue = Promise.resolve();
    }

    const queuedSave = globalObj.__playplayScheduleSaveQueue
        .catch(() => {})
        .then(runPersist);
    globalObj.__playplayScheduleSaveQueue = queuedSave;
    await queuedSave;

    // Report to sales endpoint if configured
    try {
        const settings = loadSettings();
        const extendedSettings = settings as any; // ExtendedSettings extends AutomationSettings
        if (extendedSettings.salesReportingEndpoint) {
            await reportScheduleToEndpoint(scheduleToSave, channelId || 'ch-1', extendedSettings.salesReportingEndpoint);
        }
    } catch (error) {
        console.error('[Storage] Error reporting schedule to endpoint:', error);
    }

    // Notify listeners (ticker, schedulers) that the schedule changed
    // Dispatch events for each modified slot so playout can reload current hour if needed
    if (typeof window !== 'undefined') {
        // Dispatch general update event
        window.dispatchEvent(new CustomEvent('schedule-updated', { 
            detail: { 
                source: 'storage', 
                version: scheduleToSave.version, 
                channelId,
                allSlots: true // Indicates all slots may have changed
            } 
        }));
        // Dedicated save event for playout snap/reload hooks.
        window.dispatchEvent(new CustomEvent('schedule-saved', {
            detail: {
                source: 'storage',
                version: scheduleToSave.version,
                channelId,
            },
        }));
        
        // Keep broadcast deterministic: one event per save avoids reload storms.
    }
}

// Report schedule to sales endpoint
async function reportScheduleToEndpoint(
    schedule: WeeklySchedule,
    channelId: string,
    endpointUrl: string
): Promise<void> {
    try {
        // Collect all asset hashes from schedule
        const assetHashes: string[] = [];
        const { createAssetHash } = await import('./assetHasher');
        
        for (const slot of Object.values(schedule.slots)) {
            for (const item of slot.items) {
                if (item.media) {
                    const hashEntry = await createAssetHash(item.media);
                    assetHashes.push(hashEntry.hash);
                }
            }
        }

        const response = await fetch(`${endpointUrl}/api/schedule/sync`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                channelId,
                scheduleId: schedule.startDate || 'default',
                scheduleData: schedule,
                assetHashes: [...new Set(assetHashes)], // Unique hashes
                version: schedule.version,
                timestamp: schedule.lastModified,
            }),
        });

        if (!response.ok) {
            console.error('[Storage] Failed to report schedule to endpoint:', response.statusText);
        }
    } catch (error) {
        console.error('[Storage] Error reporting schedule:', error);
    }
}

// --- FREE PLAYLISTS ---

export async function loadFreePlaylists(): Promise<FreePlaylist[]> {
    // Try DB first (Electron), fallback to localStorage
    const w = typeof window !== 'undefined' ? (window as any) : null;
    if (w?.electronAPI?.scheduleGetAllPlaylists) {
        try {
            const { dbGetAllFreePlaylists } = await import('./scheduleDbClient');
            const playlists = await dbGetAllFreePlaylists();

            // Normalize embedded media so free playlists also survive restarts cleanly.
            return playlists.map(pl => ({
                ...pl,
                items: normalizePlaylistItems(pl.items || []),
            }));
        } catch (e) {
            console.error('[Storage] Failed to load playlists from DB, falling back to localStorage:', e);
        }
    }

    // Fallback to localStorage
    const stored = localStorage.getItem(STORAGE_KEYS.FREE_PLAYLISTS);
    const playlists = safeJsonParse<FreePlaylist[]>(stored, []);

    // Normalize embedded media so free playlists also survive restarts cleanly.
    return playlists.map(pl => ({
        ...pl,
        items: normalizePlaylistItems(pl.items || []),
    }));
}

export async function saveFreePlaylists(playlists: FreePlaylist[]): Promise<void> {
    // Try DB first (Electron), fallback to localStorage
    const w = typeof window !== 'undefined' ? (window as any) : null;
    if (w?.electronAPI?.scheduleSavePlaylist) {
        try {
            const { dbSaveFreePlaylist, dbDeleteFreePlaylist, dbGetAllFreePlaylists } = await import('./scheduleDbClient');

            // Get existing playlists from DB
            const existing = await dbGetAllFreePlaylists();
            const existingIds = new Set(existing.map(p => p.id));
            const newIds = new Set(playlists.map(p => p.id));

            // Delete playlists that were removed
            for (const existingId of existingIds) {
                if (!newIds.has(existingId)) {
                    await dbDeleteFreePlaylist(existingId);
                }
            }

            // Save/update all playlists
            for (const playlist of playlists) {
                await dbSaveFreePlaylist(playlist);
            }

            // Also save to localStorage as backup
            localStorage.setItem(STORAGE_KEYS.FREE_PLAYLISTS, JSON.stringify(playlists));
            if (typeof window !== 'undefined') {
                window.dispatchEvent(new CustomEvent('playlist-saved', {
                    detail: { source: 'storage', count: playlists.length },
                }));
            }
            return;
        } catch (e) {
            console.error('[Storage] Failed to save playlists to DB, falling back to localStorage:', e);
        }
    }

    // Fallback to localStorage
    localStorage.setItem(STORAGE_KEYS.FREE_PLAYLISTS, JSON.stringify(playlists));
    if (typeof window !== 'undefined') {
        window.dispatchEvent(new CustomEvent('playlist-saved', {
            detail: { source: 'storage', count: playlists.length },
        }));
    }
}

export function createFreePlaylist(name: string, sourceSlotId?: string): FreePlaylist {
    return {
        id: generateId(),
        name,
        items: [],
        isLocked: false,
        createdAt: Date.now(),
        createdFrom: sourceSlotId,
    };
}

// --- CLIENTS ---

export function loadClients(): AdClient[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CLIENTS);
    return safeJsonParse(stored, []);
}

export function saveClients(clients: AdClient[]): void {
    localStorage.setItem(STORAGE_KEYS.CLIENTS, JSON.stringify(clients));
}

export function createClient(name: string): AdClient {
    const colors = ['#ef4444', '#f97316', '#eab308', '#22c55e', '#06b6d4', '#3b82f6', '#8b5cf6', '#ec4899'];
    const now = new Date().toISOString();
    return {
        id: generateId(),
        name,
        color: colors[Math.floor(Math.random() * colors.length)],
        contactEmail: '',
        contactName: '',
        contactPhone: '',
        address: '',
        website: '',
        contractStart: '',
        contractEnd: '',
        budget: 0,
        ratePerPlay: 0,
        billingCode: '',
        terms: '',
        rules: { ...DEFAULT_SCHEDULING_RULE },
        status: 'active',
        notes: '',
        createdAt: now,
        updatedAt: now,
    };
}

// --- CG ELEMENTS ---

export function loadCGElements(): CGElement[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CG_ELEMENTS);
    return safeJsonParse(stored, []);
}

export function saveCGElements(elements: CGElement[]): void {
    localStorage.setItem(STORAGE_KEYS.CG_ELEMENTS, JSON.stringify(elements));
}

export function createCGElement(type: CGElement['type'], name: string): CGElement {
    const defaults: Record<CGElement['type'], Partial<CGElement>> = {
        lower_third: {
            position: { x: 5, y: 75, width: 40, height: 15, anchor: 'bottom-left' },
            fontSize: 24,
        },
        ticker: {
            position: { x: 0, y: 90, width: 100, height: 8, anchor: 'bottom-left' },
            fontSize: 18,
            loop: 'ticker_scroll',
        },
        bug: {
            position: { x: 95, y: 5, width: 8, height: 8, anchor: 'top-right' },
        },
        clock: {
            position: { x: 90, y: 5, width: 10, height: 5, anchor: 'top-right' },
            fontSize: 16,
        },
        weather: {
            position: { x: 5, y: 5, width: 15, height: 10, anchor: 'top-left' },
            fontSize: 14,
        },
        custom: {
            position: { x: 50, y: 50, width: 20, height: 10, anchor: 'center' },
        },
    };

    return {
        id: generateId(),
        name,
        type,
        title: name,
        colors: {
            primary: '#1e40af',
            secondary: '#3b82f6',
            text: '#ffffff',
            gradient: true,
            gradientDirection: 'horizontal',
        },
        effects: {
            shadow: true,
            shadowColor: '#000000',
            shadowBlur: 10,
            glow: false,
            borderRadius: 4,
            opacity: 1,
        },
        position: { x: 50, y: 50, width: 30, height: 10, anchor: 'center' },
        animationIn: 'fade',
        animationOut: 'fade',
        animationDuration: 500,
        loop: 'none',
        isVisible: false,
        isPreset: false,
        ...defaults[type],
    };
}

// --- CG PRESETS ---

export function loadCGPresets(): CGPreset[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CG_PRESETS);
    const custom = safeJsonParse<CGPreset[]>(stored, []);
    return [...getBuiltInPresets(), ...custom];
}

export function saveCGPresets(presets: CGPreset[]): void {
    // Only save custom presets, not built-in
    const custom = presets.filter(p => !p.id.startsWith('builtin_'));
    localStorage.setItem(STORAGE_KEYS.CG_PRESETS, JSON.stringify(custom));
}

function getBuiltInPresets(): CGPreset[] {
    return [];
}

// --- SETTINGS ---

// Settings - now handled by settingsManager
// Keeping for backward compatibility
export function loadSettings(): AutomationSettings {
    try {
        const { settingsManager } = require('./settingsManager');
        return settingsManager.getSettings();
    } catch {
        const stored = localStorage.getItem(STORAGE_KEYS.SETTINGS);
        return { ...DEFAULT_SETTINGS, ...safeJsonParse(stored, {}) };
    }
}

export function saveSettings(settings: AutomationSettings): void {
    try {
        const { settingsManager } = require('./settingsManager');
        settingsManager.updateAutomationSettings(settings);
    } catch {
        localStorage.setItem(STORAGE_KEYS.SETTINGS, JSON.stringify(settings));
    }
}

// --- MEDIA STATS ---

export function loadMediaStats(): Record<string, MediaStats> {
    const stored = localStorage.getItem(STORAGE_KEYS.MEDIA_STATS);
    return safeJsonParse(stored, {});
}

export function saveMediaStats(stats: Record<string, MediaStats>): void {
    localStorage.setItem(STORAGE_KEYS.MEDIA_STATS, JSON.stringify(stats));
}

const MAX_AS_PLAYED = 5000;
const AS_PLAYED_RETENTION_MS = 90 * 24 * 60 * 60 * 1000; // 90 days

export function loadAsPlayedLog(): AsPlayedEntry[] {
    const stored = localStorage.getItem(STORAGE_KEYS.AS_PLAYED_LOG);
    const parsed = safeJsonParse(stored, []);
    if (!Array.isArray(parsed)) return [];
    const cutoff = Date.now() - AS_PLAYED_RETENTION_MS;
    return parsed.filter((e: AsPlayedEntry) => e.timestamp >= cutoff);
}

export function appendAsPlayed(entry: AsPlayedEntry): void {
    const log = loadAsPlayedLog();
    log.push(entry);
    const cutoff = Date.now() - AS_PLAYED_RETENTION_MS;
    const trimmed = log.filter(e => e.timestamp >= cutoff);
    const result = trimmed.slice(-MAX_AS_PLAYED);
    localStorage.setItem(STORAGE_KEYS.AS_PLAYED_LOG, JSON.stringify(result));
    persistentLogger.info('As-played logged', entry, 'AsPlayed');
}

export function recordPlayEvent(mediaId: string, event: PlayEvent): void {
    const allStats = loadMediaStats();
    const stats = allStats[mediaId] || { playCount: 0, totalPlayTime: 0, playHistory: [] };

    stats.playCount++;
    stats.totalPlayTime += event.duration;
    stats.lastPlayed = event.timestamp;
    stats.playHistory.push(event);

    // Prune old history (keep last 30 days by default)
    const settings = loadSettings();
    const cutoff = Date.now() - (settings.maxHistoryDays * 24 * 60 * 60 * 1000);
    stats.playHistory = stats.playHistory.filter(e => e.timestamp > cutoff);

    allStats[mediaId] = stats;
    saveMediaStats(allStats);

    // Also push to a global as-played log for compliance / export
    const actualStart = event.actualStart || event.timestamp;
    const actualEnd = event.actualEnd || (event.timestamp + event.duration * 1000);
    appendAsPlayed({
        mediaId,
        ...event,
        actualStart,
        actualEnd,
    });
}

export function getMediaStats(mediaId: string): MediaStats | null {
    const allStats = loadMediaStats();
    return allStats[mediaId] || null;
}

// --- OBS COMMANDS ---

export function loadOBSCommands(): OBSCommand[] {
    const stored = localStorage.getItem(STORAGE_KEYS.OBS_COMMANDS);
    return safeJsonParse(stored, []);
}

export function saveOBSCommands(commands: OBSCommand[]): void {
    localStorage.setItem(STORAGE_KEYS.OBS_COMMANDS, JSON.stringify(commands));
}

export function loadOBSSessions(): OBSSession[] {
    const settings = loadSettings() as ExtendedSettings;
    return settings.obsSessions || [];
}

// --- LOCAL MEDIA ---

export function loadLocalMedia(): MediaItem[] {
    const stored = localStorage.getItem(STORAGE_KEYS.LOCAL_MEDIA);
    return safeJsonParse(stored, []);
}

export function saveLocalMedia(media: MediaItem[]): void {
    // Filter out any items with blob URLs - they can't persist
    // Only save items with filePath (full absolute paths)
    const persistable = media.map(item => {
        // If item has blob URL but no filePath, remove the blob URL from saved version
        if (item.src && item.src.startsWith('blob:') && !item.filePath) {
            // Don't save items without filePath - they're temporary
            return null;
        }
        // If item has filePath, ensure src is rebuilt from it (not blob)
        if (item.filePath) {
            return {
                ...item,
                src: pathToFileUrl(item.filePath), // Always rebuild from filePath
            };
        }
        return item;
    }).filter((item): item is MediaItem => item !== null);

    localStorage.setItem(STORAGE_KEYS.LOCAL_MEDIA, JSON.stringify(persistable));
}

// --- LIBRARY MEDIA (in-app library + user edits; persisted in localStorage) ---

export function loadLibraryMedia(defaults: MediaItem[] = []): MediaItem[] {
    const stored = localStorage.getItem(STORAGE_KEYS.LIBRARY_MEDIA);
    const saved = safeJsonParse<MediaItem[]>(stored, []);

    // If nothing saved yet, return provided defaults (use [] for no sample data)
    const base = (!saved || saved.length === 0) ? (defaults ?? []) : saved;
    const specialItems: MediaItem[] = [
        {
            id: 'special-manual-gap',
            title: '[MANUAL GAP]',
            duration: 15,
            src: 'signal:bars',
            type: 'gap',
            notes: 'Operator-confirmed hold/gap item',
        },
        {
            id: 'special-obs',
            title: '[OBS]',
            duration: 1,
            src: 'command:obs',
            type: 'live',
            notes: 'OBS automation action item (simple/advanced)',
        },
        {
            id: 'special-atem',
            title: '[ATEM]',
            duration: 1,
            src: 'command:atem',
            type: 'live',
            notes: 'ATEM automation action item (simple/advanced)',
        },
    ];
    const merged = [...base];
    specialItems.forEach((item) => {
        if (!merged.some((m) => m.id === item.id || m.title === item.title)) merged.push(item);
    });
    return merged.map((m) => normalizeMediaPriorityFields(m));
}

export function saveLibraryMedia(media: MediaItem[]): void {
    localStorage.setItem(STORAGE_KEYS.LIBRARY_MEDIA, JSON.stringify(media));
}

// --- CLIPBOARD (for copy/paste hours and days) ---

let clipboardSlot: HourSlot | null = null;
let clipboardDay: { dayOfWeek: number; slots: HourSlot[] } | null = null;

export function copyHourToClipboard(slot: HourSlot): void {
    clipboardSlot = { ...slot, items: [...slot.items] };
    clipboardDay = null;
}

export function copyDayToClipboard(dayOfWeek: number, schedule: WeeklySchedule, date?: string): void {
    const slots: HourSlot[] = [];
    for (let h = 0; h < 24; h++) {
        const slotId = date ? `${date}-${h}` : `${dayOfWeek}-${h}`;
        if (schedule.slots[slotId]) {
            slots.push(schedule.slots[slotId]);
        }
    }
    clipboardDay = { dayOfWeek, slots };
    clipboardSlot = null;
}

export function getClipboardSlot(): HourSlot | null {
    return clipboardSlot;
}

export function getClipboardDay(): { dayOfWeek: number; slots: HourSlot[] } | null {
    return clipboardDay;
}

export function pasteHourFromClipboard(targetSlotId: string, schedule: WeeklySchedule, options?: { shiftAds?: boolean }): WeeklySchedule {
    if (!clipboardSlot) return schedule;

    const parts = targetSlotId.split('-');
    const hour = parseInt(parts[parts.length - 1], 10);
    
    let items = clipboardSlot.items.map(item => ({
        ...item,
        id: generateId(),
    }));

    if (options?.shiftAds) {
        // Shifting ads logic could involve re-resolving ad slots or just keeping them but marking for refresh
        // For now, we'll just keep them but mark them as "unresolved" if they are dynamic
    }

    const newSlot: HourSlot = {
        ...clipboardSlot,
        id: targetSlotId,
        hour,
        items,
        updatedAt: Date.now(),
    };

    return {
        ...schedule,
        slots: {
            ...schedule.slots,
            [targetSlotId]: newSlot,
        },
    };
}

export function pasteDayFromClipboard(targetDate: string, schedule: WeeklySchedule, options?: { shiftAds?: boolean }): WeeklySchedule {
    if (!clipboardDay) return schedule;

    const nextSlots = { ...schedule.slots };
    
    for (const sourceSlot of clipboardDay.slots) {
        const targetSlotId = `${targetDate}-${sourceSlot.hour}`;
        
        let items = sourceSlot.items.map(item => ({
            ...item,
            id: generateId(),
        }));

        // TODO: Implement shiftAds logic if needed
        
        nextSlots[targetSlotId] = {
            ...sourceSlot,
            id: targetSlotId,
            items,
            updatedAt: Date.now(),
        };
    }

    return {
        ...schedule,
        slots: nextSlots,
    };
}

// --- EXPORT UTILITIES ---

export function exportToJson(data: unknown, filename: string): void {
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
}

export function importFromJson<T>(file: File): Promise<T> {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
            try {
                const data = JSON.parse(e.target?.result as string);
                resolve(data as T);
            } catch (err) {
                reject(err);
            }
        };
        reader.onerror = reject;
        reader.readAsText(file);
    });
}

// --- CAMPAIGNS ---

export function loadCampaigns(): Campaign[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CAMPAIGNS);
    return safeJsonParse(stored, []);
}

export function saveCampaigns(campaigns: Campaign[]): void {
    localStorage.setItem(STORAGE_KEYS.CAMPAIGNS, JSON.stringify(campaigns));
}

export function createCampaign(name: string, client: string): Campaign {
    const now = new Date();
    const endDate = new Date(now);
    endDate.setMonth(endDate.getMonth() + 1); // Default 1 month campaign

    return {
        id: generateId(),
        name,
        client,
        startDate: now.toISOString(),
        endDate: endDate.toISOString(),
        targetPlays: 0,
        currentPlays: 0,
    };
}

// --- CG TEMPLATE PRESETS ---

export function loadCGTemplatePresets(): CGTemplatePreset[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CG_TEMPLATE_PRESETS);
    return safeJsonParse(stored, []);
}

export function saveCGTemplatePresets(presets: CGTemplatePreset[]): void {
    localStorage.setItem(STORAGE_KEYS.CG_TEMPLATE_PRESETS, JSON.stringify(presets));
}

export function createCGTemplatePreset(name: string, templateId: string, values: Record<string, string>): CGTemplatePreset {
    return {
        id: generateId(),
        name,
        templateId,
        values,
        createdAt: new Date().toISOString(),
    };
}

// --- CONNECTION SETTINGS ---

// Connection settings - now handled by settingsManager
// Keeping for backward compatibility
export function loadConnectionSettings(): any {
    try {
        const { settingsManager } = require('./settingsManager');
        return settingsManager.getSettings().connections;
    } catch {
        const stored = localStorage.getItem(STORAGE_KEYS.CONNECTION_SETTINGS);
        return safeJsonParse(stored, null);
    }
}

export function saveConnectionSettings(settings: any): void {
    try {
        const { settingsManager } = require('./settingsManager');
        settingsManager.updateConnections(settings);
    } catch {
        localStorage.setItem(STORAGE_KEYS.CONNECTION_SETTINGS, JSON.stringify(settings));
    }
}

// --- ENGINE PATHS ---

export function loadEnginePaths(): Record<string, string> {
    const stored = localStorage.getItem(STORAGE_KEYS.ENGINE_PATHS);
    return safeJsonParse(stored, {});
}

export function saveEnginePaths(paths: Record<string, string>): void {
    localStorage.setItem(STORAGE_KEYS.ENGINE_PATHS, JSON.stringify(paths));
}

// --- CG HISTORY ---

const MAX_HISTORY_ITEMS = 20;

export function loadCGHistory(): CGHistoryItem[] {
    const stored = localStorage.getItem(STORAGE_KEYS.CG_HISTORY);
    const history = safeJsonParse<CGHistoryItem[]>(stored, []);
    // Sort by most recent first
    return history.sort((a, b) => new Date(b.usedAt).getTime() - new Date(a.usedAt).getTime());
}

export function saveCGHistory(history: CGHistoryItem[]): void {
    // Keep only the most recent MAX_HISTORY_ITEMS
    const trimmed = history.slice(0, MAX_HISTORY_ITEMS);
    localStorage.setItem(STORAGE_KEYS.CG_HISTORY, JSON.stringify(trimmed));
}

export function addCGHistoryItem(templateId: string, values: Record<string, string>): void {
    const history = loadCGHistory();
    const newItem: CGHistoryItem = {
        id: generateId(),
        templateId,
        values,
        usedAt: new Date().toISOString(),
    };
    // Remove any existing item with same templateId and values to avoid duplicates
    const filtered = history.filter(item =>
        !(item.templateId === templateId && JSON.stringify(item.values) === JSON.stringify(values))
    );
    // Add new item at the beginning
    filtered.unshift(newItem);
    saveCGHistory(filtered);
}

export function clearCGHistory(): void {
    localStorage.removeItem(STORAGE_KEYS.CG_HISTORY);
}

// --- SITE STATES (Multi-Site Ticker) ---

export async function loadSiteStates(): Promise<Record<string, SiteState>> {
    const stored = localStorage.getItem(STORAGE_KEYS.SITE_STATES);
    return safeJsonParse<Record<string, SiteState>>(stored, {});
}

export async function saveSiteStates(states: Record<string, SiteState>): Promise<void> {
    localStorage.setItem(STORAGE_KEYS.SITE_STATES, JSON.stringify(states));
}

export async function loadSiteVisibility(): Promise<string[]> {
    const stored = localStorage.getItem(STORAGE_KEYS.SITE_VISIBILITY);
    return safeJsonParse<string[]>(stored, []);
}

export async function saveSiteVisibility(siteIds: string[]): Promise<void> {
    localStorage.setItem(STORAGE_KEYS.SITE_VISIBILITY, JSON.stringify(siteIds));
}

// --- ERROR ENTRIES ---

import { ErrorEntry } from './types';

export async function loadErrorEntries(): Promise<ErrorEntry[]> {
    const stored = localStorage.getItem('error_entries');
    return safeJsonParse<ErrorEntry[]>(stored, []);
}

export async function saveErrorEntries(errors: ErrorEntry[]): Promise<void> {
    // Keep only last 1000 errors
    const trimmed = errors.slice(-1000);
    localStorage.setItem('error_entries', JSON.stringify(trimmed));
}

// --- FILE INDEX ---

import { FileIndexEntry } from './types';

export async function loadFileIndex(): Promise<Record<string, FileIndexEntry>> {
    const stored = localStorage.getItem('file_index');
    return safeJsonParse<Record<string, FileIndexEntry>>(stored, {});
}

export async function saveFileIndex(index: Record<string, FileIndexEntry>): Promise<void> {
    localStorage.setItem('file_index', JSON.stringify(index));
}

// --- COMMAND CONTAINERS ---

import { CommandContainer, EquipmentConnection } from './types';

export async function loadCommandContainers(): Promise<CommandContainer[]> {
    const stored = localStorage.getItem('command_containers');
    return safeJsonParse<CommandContainer[]>(stored, []);
}

export async function saveCommandContainers(commands: CommandContainer[]): Promise<void> {
    localStorage.setItem('command_containers', JSON.stringify(commands));
}

export async function loadEquipmentConnections(): Promise<EquipmentConnection[]> {
    const stored = localStorage.getItem('equipment_connections');
    return safeJsonParse<EquipmentConnection[]>(stored, []);
}

export async function saveEquipmentConnections(connections: EquipmentConnection[]): Promise<void> {
    localStorage.setItem('equipment_connections', JSON.stringify(connections));
}
