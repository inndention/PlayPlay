/**
 * Comprehensive playout logging — durable, append-only.
 *
 * Logs to:
 *   1. Electron main process → daily JSONL file in userData/playout-logs/
 *      (survives app crashes, full audit trail, exportable)
 *   2. Same handler mirrors a **short** line to userData/playout-logs/event-summary.log
 *      (timestamp|channel|event|id, append-only, size-rotated) when channelId is set or inferred
 *   3. localStorage as-played ring (legacy, for in-app reports)
 *
 * Every event of interest is logged: clip start, clip end, fade, gap, error,
 * manual override, schedule reload. Reports/analytics read from the JSONL.
 */

export type PlayoutEventType =
    | 'play_start'
    | 'play_end'
    | 'play_error'
    | 'fade_start'
    | 'fade_complete'
    | 'gap_detected'
    | 'fill_inserted'
    | 'override_applied'
    | 'schedule_loaded'
    | 'engine_start'
    | 'engine_stop';

export interface PlayoutLogEntry {
    /** Event time, ms since epoch */
    t?: number;
    /** Program output channel (e.g. ch-1) for summary log mirroring */
    channelId?: string;
    type: PlayoutEventType;
    mediaId?: string;
    mediaTitle?: string;
    /** Was this clip on the schedule, or fill/makeup? */
    source?: 'scheduled' | 'fill' | 'makeup' | 'manual' | 'override';
    /** ms — only for play_end */
    durationMs?: number;
    /** Did the clip play to its full intended duration? */
    completed?: boolean;
    /** Scheduled start time (ms), if applicable */
    scheduledStart?: number;
    /** Actual wall-clock start (ms) */
    actualStart?: number;
    /** Actual wall-clock end (ms) — for play_end only */
    actualEnd?: number;
    /** Hour slot ID, for slot-aware reports */
    hourSlot?: string;
    /** Playlist item ID */
    itemId?: string;
    /** Position in playlist at time of play */
    playlistIndex?: number;
    /** Free-form details (error message, fade duration, etc.) */
    detail?: Record<string, unknown>;
}

const isElectron = typeof window !== 'undefined' && !!(window as { electronAPI?: unknown }).electronAPI;

export async function logPlayoutEvent(entry: PlayoutLogEntry): Promise<void> {
    const enriched: PlayoutLogEntry = { t: Date.now(), ...entry };

    // 1. Durable Electron file log
    if (isElectron) {
        const api = (window as { electronAPI?: { appendPlayoutLog?: (e: Record<string, unknown>) => Promise<unknown> } }).electronAPI;
        if (api?.appendPlayoutLog) {
            try {
                await api.appendPlayoutLog(enriched as unknown as Record<string, unknown>);
            } catch (e) {
                console.error('[PlayoutLog] file append failed:', e);
            }
        }
    }

    // 2. In-memory ring (used by error/log viewers in-app)
    try {
        const KEY = 'playplay_playout_events';
        const raw = localStorage.getItem(KEY);
        const arr = raw ? JSON.parse(raw) : [];
        arr.push(enriched);
        // Cap at 5000 to bound localStorage usage
        const trimmed = arr.length > 5000 ? arr.slice(-5000) : arr;
        localStorage.setItem(KEY, JSON.stringify(trimmed));
    } catch {
        /* localStorage may be full; the durable file write is what matters */
    }
}

export function readRecentPlayoutEvents(limit = 200): PlayoutLogEntry[] {
    try {
        const raw = localStorage.getItem('playplay_playout_events');
        if (!raw) return [];
        const arr = JSON.parse(raw) as PlayoutLogEntry[];
        return arr.slice(-limit);
    } catch {
        return [];
    }
}

export async function readPlayoutLogForDate(date?: Date): Promise<PlayoutLogEntry[]> {
    if (!isElectron) return readRecentPlayoutEvents();
    const api = (window as { electronAPI?: { readPlayoutLog?: (d?: string) => Promise<{ entries: PlayoutLogEntry[] }> } }).electronAPI;
    if (!api?.readPlayoutLog) return [];
    try {
        const r = await api.readPlayoutLog(date?.toISOString());
        return r?.entries || [];
    } catch {
        return [];
    }
}
