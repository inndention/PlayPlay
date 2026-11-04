/**
 * Re-pick ad rows in an hour slot using the same eligible pool as playout quick-break:
 * library + local + DB, type === 'ad', not archived, positive duration.
 *
 * Sampling: P(ad_i) ∝ priorityWeight_i, where
 *   priorityWeight_i = max(ε, (priority + 10)) × (adMetadata.weight if finite > 0 else 1)
 * (normalize over the pool per draw; independent draws with replacement per ad slot.)
 */

import type { HourSlot, MediaItem, PlaylistItem } from './types';
import { loadLibraryMedia, loadLocalMedia } from './storage';
import { dbFetchMedia } from './mediaDbClient';

const EPS = 0.0001;

function newPlaylistItemId(): string {
    return `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 11)}`;
}

/** Non-negative weight used as unnormalized probability mass. */
export function adPriorityWeight(media: MediaItem): number {
    const pr = typeof media.priority === 'number' && Number.isFinite(media.priority) ? media.priority : 0;
    const base = Math.max(EPS, pr + 10);
    const w = media.adMetadata?.weight;
    const weightMul = typeof w === 'number' && Number.isFinite(w) && w > 0 ? w : 1;
    return base * weightMul;
}

function pickWeightedMedia(pool: MediaItem[]): MediaItem | null {
    if (pool.length === 0) return null;
    const weights = pool.map(adPriorityWeight);
    const total = weights.reduce((a, b) => a + b, 0);
    if (!(total > 0)) return pool[0] ?? null;
    let r = Math.random() * total;
    for (let i = 0; i < pool.length; i++) {
        r -= weights[i];
        if (r <= 0) return pool[i];
    }
    return pool[pool.length - 1] ?? null;
}

/** Deduped eligible ads from real library paths (no synthetic inventory). */
export async function fetchEligibleAdPool(): Promise<MediaItem[]> {
    const [dbMedia, localMedia, libMedia] = await Promise.all([
        dbFetchMedia().catch(() => [] as MediaItem[]),
        Promise.resolve(loadLocalMedia()),
        Promise.resolve(loadLibraryMedia([])),
    ]);
    const merged = [...dbMedia, ...localMedia, ...libMedia];
    const map = new Map<string, MediaItem>();
    for (const m of merged) {
        if (!m || !m.id) continue;
        if (m.type !== 'ad') continue;
        if (m.archived) continue;
        if (!(m.duration && m.duration > 0)) continue;
        if (!map.has(m.id)) map.set(m.id, m);
    }
    return Array.from(map.values());
}

export interface ShakeHourAdsResult {
    items: PlaylistItem[];
    replacedAdCount: number;
}

/**
 * Replace each ad playlist row with a new weighted-random ad from `pool`.
 * Non-ad rows unchanged (order preserved).
 */
export function buildShakenSlotItems(slot: HourSlot, pool: MediaItem[]): ShakeHourAdsResult | null {
    if (!slot.items?.length || pool.length === 0) return null;
    let replaced = 0;
    const items = slot.items.map((item) => {
        if (item.media?.type !== 'ad') return item;
        const pick = pickWeightedMedia(pool);
        if (!pick) return item;
        replaced++;
        return {
            ...item,
            id: newPlaylistItemId(),
            mediaId: pick.id,
            media: pick,
            isResolved: true,
        };
    });
    if (replaced === 0) return null;
    return { items, replacedAdCount: replaced };
}
