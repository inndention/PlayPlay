/**
 * Typed in-app agent surface: proposals, warnings, completions.
 * Primary UX maps to toasts (see `usePlayplayAgentToastBridge`) — not a full chat pane.
 * VoIP / SIP sidecars may push the same shapes over IPC (`playplay-agent:event`).
 *
 * @see docs/PLAYPLAY_AI_STARTUP.md
 * @see docs/PLAYPLAY_AGENT_OPENCLAW_LIKE.md
 */

export type PlayplayAgentEventKind = 'propose' | 'warn' | 'done';

/** Subset of `VoiceAgentSanitizedSnapshot.playout` plus capture time (read-only context). */
export type PlayplayAgentSnapshotContext = {
    isPlaying: boolean;
    currentTitle: string | null;
    capturedAt?: number;
};

export interface PlayplayAgentEvent {
    kind: PlayplayAgentEventKind;
    /** Short headline for the toast */
    title: string;
    /** Optional body (kept short for toast width) */
    detail?: string;
    /** Stable idempotent key — same key replaces the prior toast */
    toastKey: string;
    snapshotContext?: PlayplayAgentSnapshotContext;
}

type AgentListener = (event: PlayplayAgentEvent) => void;

const listeners = new Set<AgentListener>();

export function subscribePlayplayAgentBus(fn: AgentListener): () => void {
    listeners.add(fn);
    return () => listeners.delete(fn);
}

export function emitPlayplayAgentEvent(event: PlayplayAgentEvent): void {
    for (const fn of listeners) {
        try {
            fn(event);
        } catch (e) {
            console.error('[playplayAgentBus] listener error', e);
        }
    }
}

export function agentEventToastType(kind: PlayplayAgentEventKind): 'info' | 'warning' | 'success' {
    switch (kind) {
        case 'propose':
            return 'info';
        case 'warn':
            return 'warning';
        case 'done':
            return 'success';
    }
}

export function formatAgentToastMessage(ev: PlayplayAgentEvent): string {
    const bits = [ev.title.trim()];
    if (ev.detail?.trim()) bits.push(ev.detail.trim());
    if (ev.snapshotContext) {
        const t = ev.snapshotContext.currentTitle;
        const air = t ? `On-air: ${t}` : 'On-air: (none)';
        bits.push(`${air}${ev.snapshotContext.isPlaying ? ' · playing' : ' · idle'}`);
    }
    return bits.join(' — ');
}

/** Narrow unknown IPC payloads without throwing. */
export function parsePlayplayAgentIpcPayload(raw: unknown): PlayplayAgentEvent | null {
    if (!raw || typeof raw !== 'object') return null;
    const o = raw as Record<string, unknown>;
    const kind = o.kind;
    if (kind !== 'propose' && kind !== 'warn' && kind !== 'done') return null;
    const title = typeof o.title === 'string' ? o.title : '';
    const toastKey = typeof o.toastKey === 'string' && o.toastKey.trim() ? o.toastKey.trim() : '';
    if (!title.trim() || !toastKey) return null;
    const detail = typeof o.detail === 'string' ? o.detail : undefined;
    let snapshotContext: PlayplayAgentSnapshotContext | undefined;
    const sc = o.snapshotContext;
    if (sc && typeof sc === 'object') {
        const p = sc as Record<string, unknown>;
        const ct = p.currentTitle;
        snapshotContext = {
            isPlaying: Boolean(p.isPlaying),
            currentTitle: typeof ct === 'string' ? ct : ct === null ? null : null,
            capturedAt: typeof p.capturedAt === 'number' ? p.capturedAt : undefined,
        };
    }
    return { kind, title, detail, toastKey, snapshotContext };
}

export function ingestPlayplayAgentIpcPayload(raw: unknown): boolean {
    const ev = parsePlayplayAgentIpcPayload(raw);
    if (!ev) return false;
    emitPlayplayAgentEvent(ev);
    return true;
}
