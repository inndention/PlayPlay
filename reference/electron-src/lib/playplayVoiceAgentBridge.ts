/**
 * Read-only bridge for the semi-live voice / SIP agent: polls PLAY PLAY’s output HTTP
 * server for a sanitized snapshot (no media paths, no Caspar install paths).
 *
 * Server route: GET /api/voice-agent/snapshot (Electron main, gated by PLAYPLAY_VOICE_AGENT=1).
 * @see docs/VOICE_AGENT_SIP.md
 */

export const VOICE_AGENT_SNAPSHOT_PATH = '/api/voice-agent/snapshot' as const;

export type VoiceAgentCasparSnapshot = {
    running: boolean;
    configuredChannels: number | null;
    videoMode: string | null;
    amcpConnected: boolean;
};

export type VoiceAgentChannelEntry = {
    channelId: string;
    title: string | null;
    isPlaying: boolean;
    updatedAt: number;
};

export type VoiceAgentSanitizedSnapshot = {
    schemaVersion: number;
    capturedAt: number;
    playout: {
        isPlaying: boolean;
        armed: boolean;
        hasCurrentItem: boolean;
        currentTitle: string | null;
        hasNextItem: boolean;
        currentTimeSec: number | null;
    };
    channels: {
        count: number;
        activeChannelId: string | null;
        entries: VoiceAgentChannelEntry[];
    };
    caspar: VoiceAgentCasparSnapshot;
    notes: { obsWebSocket: string };
};

function readProcessEnv(key: string): string | undefined {
    if (typeof process === 'undefined' || !process.env) return undefined;
    const v = process.env[key];
    return v == null || v === '' ? undefined : String(v);
}

function clamp(n: number, lo: number, hi: number): number {
    return Math.min(hi, Math.max(lo, n));
}

/** Bounded non-deterministic delay before status polls (no busy loops). */
export async function sleepVoiceAgentPollJitter(): Promise<void> {
    const rawMin = Number(readProcessEnv('PLAYPLAY_VOICE_POLL_JITTER_MS_MIN') ?? '60');
    const rawMax = Number(readProcessEnv('PLAYPLAY_VOICE_POLL_JITTER_MS_MAX') ?? '420');
    const lo = clamp(Number.isFinite(rawMin) ? rawMin : 60, 0, 60_000);
    const hi = clamp(Number.isFinite(rawMax) ? rawMax : 420, lo, 60_000);
    const ms = lo + Math.floor(Math.random() * (hi - lo + 1));
    if (ms <= 0) return;
    await new Promise((r) => setTimeout(r, ms));
}

/**
 * Base URL for the output server (GET /api/state, etc.).
 * Override with PLAYPLAY_OUTPUT_BASE (no trailing slash), e.g. http://127.0.0.1:8080
 */
export function resolvePlayplayOutputBaseUrl(override?: string): string {
    const o = (override ?? readProcessEnv('PLAYPLAY_OUTPUT_BASE') ?? '').replace(/\/$/, '').trim();
    if (o) return o;
    const p = parseInt(readProcessEnv('PLAYPLAY_OUTPUT_PORT') ?? '8080', 10);
    const port = Number.isFinite(p) && p >= 1 && p <= 65535 ? p : 8080;
    return `http://127.0.0.1:${port}`;
}

export function isVoiceAgentModeEnabled(): boolean {
    return String(readProcessEnv('PLAYPLAY_VOICE_AGENT') ?? '').trim() === '1';
}

/**
 * Fetches the sanitized snapshot. The HTTP route returns 404 unless PLAYPLAY_VOICE_AGENT=1
 * on the Electron process hosting the output server.
 */
export async function fetchVoiceAgentSanitizedSnapshot(
    baseUrl?: string,
    init?: RequestInit,
): Promise<{ ok: true; data: VoiceAgentSanitizedSnapshot } | { ok: false; status: number; error: string }> {
    const base = resolvePlayplayOutputBaseUrl(baseUrl);
    const url = `${base}${VOICE_AGENT_SNAPSHOT_PATH}`;
    await sleepVoiceAgentPollJitter();
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), 8000);
    try {
        const res = await fetch(url, {
            ...init,
            cache: 'no-store',
            signal: ac.signal,
        });
        const text = await res.text();
        if (!res.ok) {
            let err = text.slice(0, 400);
            try {
                const j = JSON.parse(text) as { error?: string; hint?: string };
                err = [j.error, j.hint].filter(Boolean).join(' — ') || err;
            } catch {
                /* plain text */
            }
            return { ok: false, status: res.status, error: err || `HTTP ${res.status}` };
        }
        try {
            const data = JSON.parse(text) as VoiceAgentSanitizedSnapshot;
            return { ok: true, data };
        } catch {
            return { ok: false, status: res.status, error: 'invalid_json' };
        }
    } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        return { ok: false, status: 0, error: msg };
    } finally {
        clearTimeout(t);
    }
}

/**
 * Slice 2 hook: operator-only transport (mute channel, take, etc.) must stay behind IPC
 * and explicit allowlists — do not extend this read-only bridge without updating
 * docs/VOICE_AGENT_SIP.md and main-process guards.
 */
export const VOICE_AGENT_WRITE_HOOKS_PLACEHOLDER = [
    'ipc:casparPauseChannel / casparResumeChannel (preload)',
    'ipc:channelEngineTransport',
    'POST /api/playout/command (coarse; forwards to operator UI)',
] as const;
