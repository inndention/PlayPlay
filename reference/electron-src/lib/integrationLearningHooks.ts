/**
 * Extension point for future cue-quality or schedule hints (no model shipped).
 * Meta RN Hermes is not this stack; Nous Hermes Agent is an optional external CLI.
 * See `docs/NEXT_HERMES_AND_LEARNING.md`.
 */
export type LearningSignal = { kind: string; channelId?: string; payload?: Record<string, unknown> };

/** Safe no-op today; wire to IPC or a local service when you add a backend. */
export function emitLearningSignal(_signal: LearningSignal): void {
    /* intentionally empty */
}
