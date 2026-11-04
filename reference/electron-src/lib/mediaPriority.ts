/**
 * Media library "heat" priority: stored score, time-based decay, optional freeze.
 * Scheduling `MediaItem.priority` (-10..10) stays separate — see docs/MEDIA_PRIORITY.md.
 */
import type { MediaItem } from './types';

export const MEDIA_PRIORITY_SCORE_MAX = 100;
export const MEDIA_PRIORITY_SCORE_MIN = 0;
/** Base decay units per minute at "normal" global burn (before preset + item multipliers). */
export const MEDIA_PRIORITY_BASE_BURN_PER_MINUTE = 0.12;

export const MEDIA_PRIORITY_STORAGE_KEY = 'playplay_media_burn_preset';

export type MediaBurnPreset = 'slow' | 'normal' | 'fast';

const PRESET_GLOBAL_MULT: Record<MediaBurnPreset, number> = {
    slow: 0.45,
    normal: 1,
    fast: 2.4,
};

export function clampPriorityScore(n: number): number {
    return Math.min(MEDIA_PRIORITY_SCORE_MAX, Math.max(MEDIA_PRIORITY_SCORE_MIN, n));
}

export function getMediaBurnPreset(): MediaBurnPreset {
    try {
        const v = localStorage.getItem(MEDIA_PRIORITY_STORAGE_KEY);
        if (v === 'slow' || v === 'normal' || v === 'fast') return v;
    } catch {
        /* ignore */
    }
    return 'normal';
}

export function setMediaBurnPreset(preset: MediaBurnPreset): void {
    try {
        localStorage.setItem(MEDIA_PRIORITY_STORAGE_KEY, preset);
    } catch {
        /* ignore */
    }
}

export function globalBurnMultiplier(preset: MediaBurnPreset = getMediaBurnPreset()): number {
    return PRESET_GLOBAL_MULT[preset] ?? 1;
}

/** Minute-aligned anchor so sort/display stay stable within the same clock minute (no timer per row). */
export function priorityDecayAnchorMs(nowMs: number = Date.now()): number {
    return Math.floor(nowMs / 60_000) * 60_000;
}

function parseIsoMs(iso: string | undefined): number {
    if (!iso) return NaN;
    const t = Date.parse(iso);
    return Number.isFinite(t) ? t : NaN;
}

/** Default stored score from legacy scheduling priority when priorityScore is absent. */
export function defaultPriorityScoreFromSchedulePriority(priority: number | undefined): number {
    const p = priority ?? 0;
    return clampPriorityScore(50 + p * 5);
}

/**
 * Normalize optional fields after load (SQLite / JSON) so decay math always has sane inputs.
 */
export function normalizeMediaPriorityFields(m: MediaItem): MediaItem {
    const baseTime =
        (m.priorityUpdatedAt && Number.isFinite(Date.parse(m.priorityUpdatedAt)) && m.priorityUpdatedAt) ||
        (m.addedAt && Number.isFinite(Date.parse(m.addedAt)) && m.addedAt) ||
        new Date().toISOString();

    let priorityScore = m.priorityScore;
    if (priorityScore == null || Number.isNaN(Number(priorityScore))) {
        priorityScore = defaultPriorityScoreFromSchedulePriority(m.priority);
    } else {
        priorityScore = clampPriorityScore(Number(priorityScore));
    }

    const priorityFrozen = Boolean(m.priorityFrozen);
    let mult = m.priorityBurnRateMultiplier;
    if (mult == null || Number.isNaN(Number(mult)) || Number(mult) <= 0) {
        mult = 1;
    }

    return {
        ...m,
        priorityScore,
        priorityUpdatedAt: m.priorityUpdatedAt || baseTime,
        priorityFrozen,
        priorityBurnRateMultiplier: mult,
    };
}

/**
 * Effective library score at `anchorMs` (use priorityDecayAnchorMs() from UI for stable sorts).
 */
export function getEffectiveLibraryPriorityScore(
    m: MediaItem,
    anchorMs: number = priorityDecayAnchorMs(),
    preset: MediaBurnPreset = getMediaBurnPreset(),
): number {
    const item = normalizeMediaPriorityFields(m);
    if (item.priorityFrozen) {
        return clampPriorityScore(Number(item.priorityScore));
    }
    const stored = clampPriorityScore(Number(item.priorityScore));
    const updatedMs = parseIsoMs(item.priorityUpdatedAt);
    const t0 = Number.isFinite(updatedMs) ? updatedMs : anchorMs;
    const dtMin = Math.max(0, (anchorMs - t0) / 60_000);
    const itemMult = Number(item.priorityBurnRateMultiplier) > 0 ? Number(item.priorityBurnRateMultiplier) : 1;
    const burn =
        MEDIA_PRIORITY_BASE_BURN_PER_MINUTE * globalBurnMultiplier(preset) * itemMult * dtMin;
    return clampPriorityScore(stored - burn);
}

const STEP = 6;

function commitStoredAtNow(m: MediaItem, newStored: number): MediaItem {
    return normalizeMediaPriorityFields({
        ...m,
        priorityScore: clampPriorityScore(newStored),
        priorityUpdatedAt: new Date().toISOString(),
    });
}

export function bumpLibraryPriorityUp(m: MediaItem): MediaItem {
    const anchor = priorityDecayAnchorMs();
    const cur = getEffectiveLibraryPriorityScore(m, anchor);
    return commitStoredAtNow(m, cur + STEP);
}

export function bumpLibraryPriorityDown(m: MediaItem): MediaItem {
    const anchor = priorityDecayAnchorMs();
    const cur = getEffectiveLibraryPriorityScore(m, anchor);
    return commitStoredAtNow(m, cur - STEP);
}

/** Toggle freeze: when turning on, snapshot current effective score; when off, anchor at current effective. */
export function toggleLibraryPriorityFreeze(m: MediaItem): MediaItem {
    const anchor = priorityDecayAnchorMs();
    const item = normalizeMediaPriorityFields(m);
    if (item.priorityFrozen) {
        const effective = getEffectiveLibraryPriorityScore(
            { ...item, priorityFrozen: false },
            anchor,
        );
        return normalizeMediaPriorityFields({
            ...item,
            priorityFrozen: false,
            priorityScore: effective,
            priorityUpdatedAt: new Date().toISOString(),
        });
    }
    const effective = getEffectiveLibraryPriorityScore(item, anchor);
    return normalizeMediaPriorityFields({
        ...item,
        priorityFrozen: true,
        priorityScore: effective,
        priorityUpdatedAt: new Date().toISOString(),
    });
}
