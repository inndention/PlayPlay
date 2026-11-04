/**
 * Wall-clock ("boss clock") join-in-progress for Caspar playlist pushes.
 * Mirrors PlayoutController.calculateTimeAccuratePosition + runlist slice.
 */
import type { PlaylistItem } from './types';

export interface BossClockPosition {
    itemIndex: number;
    itemId: string | null;
    /** Absolute offset in seconds from the start of the media file for the resolved row */
    seekTime: number;
    isWithinHour: boolean;
}

/**
 * Which playlist row should be on-air now and at what media seek, given an hour anchor.
 */
export function computeBossClockPlayoutPosition(
    items: PlaylistItem[],
    hourStartTimeMs: number,
    nowMs: number = Date.now(),
): BossClockPosition {
    if (items.length === 0) {
        return { itemIndex: -1, itemId: null, seekTime: 0, isWithinHour: false };
    }

    const hasWallTimes = items.every((it) => it.startTime != null && it.endTime != null);

    if (hasWallTimes) {
        const first = items[0];
        if (first.startTime != null && nowMs < first.startTime) {
            const timeIn = first.timeIn || 0;
            return {
                itemIndex: 0,
                itemId: first.id,
                seekTime: Math.max(0, timeIn),
                isWithinHour: true,
            };
        }

        for (let i = 0; i < items.length; i++) {
            const item = items[i];
            const st = item.startTime!;
            const et = item.endTime!;

            if (nowMs >= st && nowMs < et) {
                const elapsedInItem = (nowMs - st) / 1000;
                const timeIn = item.timeIn || 0;
                const seekPosition = timeIn + elapsedInItem;
                return {
                    itemIndex: i,
                    itemId: item.id,
                    seekTime: Math.max(0, seekPosition),
                    isWithinHour: true,
                };
            }

            const next = items[i + 1];
            if (next?.startTime != null && nowMs >= et && nowMs < next.startTime) {
                const timeInNext = next.timeIn || 0;
                return {
                    itemIndex: i + 1,
                    itemId: next.id,
                    seekTime: Math.max(0, timeInNext),
                    isWithinHour: true,
                };
            }
        }

        const lastItem = items[items.length - 1];
        const lastEnd = lastItem.endTime!;
        if (nowMs >= lastEnd) {
            const timeIn = lastItem.timeIn || 0;
            return {
                itemIndex: items.length - 1,
                itemId: lastItem.id,
                seekTime: timeIn,
                isWithinHour: false,
            };
        }
    }

    const anchorMs = items[0]?.startTime ?? hourStartTimeMs;
    const elapsedSinceAnchor = (nowMs - anchorMs) / 1000;

    let cumulativeTime = 0;

    for (let i = 0; i < items.length; i++) {
        const item = items[i];
        const timeIn = item.timeIn || 0;
        const timeOut = item.timeOut ?? item.media.duration;
        const itemDuration = timeOut - timeIn;
        const segmentEnd = cumulativeTime + itemDuration;

        if (elapsedSinceAnchor >= cumulativeTime && elapsedSinceAnchor < segmentEnd) {
            const seekPosition = timeIn + (elapsedSinceAnchor - cumulativeTime);
            return {
                itemIndex: i,
                itemId: item.id,
                seekTime: Math.max(0, seekPosition),
                isWithinHour: true,
            };
        }

        cumulativeTime = segmentEnd;
    }

    const lastItem = items[items.length - 1];
    const timeIn = lastItem.timeIn || 0;
    return {
        itemIndex: items.length - 1,
        itemId: lastItem.id,
        seekTime: timeIn,
        isWithinHour: false,
    };
}

/**
 * Slice from the on-air row and set first row `timeIn` to absolute media seek (Caspar SEEK).
 * `seekIntoMediaSeconds` must match computeBossClockPlayoutPosition().seekTime (already absolute).
 */
export function buildCasparRunlistFromBossSeek(
    items: PlaylistItem[],
    startIndex: number,
    seekIntoMediaSeconds: number,
): PlaylistItem[] {
    return items.slice(startIndex).map((item, idx) => {
        if (idx !== 0) return item;
        return {
            ...item,
            timeIn: Math.max(0, seekIntoMediaSeconds),
            isFixedTime: false,
            fixedTime: undefined,
        };
    });
}

/** Join-in-progress runlist for a wall-clock hour (boss clock). */
export function buildCasparRunlistForBossClock(
    items: PlaylistItem[],
    hourStartTimeMs: number,
    nowMs: number = Date.now(),
): PlaylistItem[] {
    if (!items.length) return [];
    const pos = computeBossClockPlayoutPosition(items, hourStartTimeMs, nowMs);
    if (pos.itemIndex < 0) return items.slice();
    return buildCasparRunlistFromBossSeek(items, pos.itemIndex, pos.seekTime);
}
