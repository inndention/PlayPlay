// ============================================
// Show hour — schedule builder merge rules
// New rows at normal MediaItem.priority; prior program rows demoted to low.
// ============================================

import type { HourSlot, MediaItem, PlaylistItem } from './types';
import { autoDetectMediaMetadata } from './mediaDetection';
import { getEnvironment } from './fileResolver';
import { dbFindByFilename, dbSaveMedia } from './mediaDbClient';

/** Low scheduling weight (see MediaItem.priority -10..10). */
export const SHOW_HOUR_LOW_PRIORITY = -8;

export const SHOW_HOUR_NORMAL_PRIORITY = 0;

export const SHOW_HOUR_MAX_WATCH_FILES = 20;

export function getActiveChannelId(): string {
    if (typeof window === 'undefined') return 'ch-1';
    const id = (window as unknown as { __activeChannelId?: string }).__activeChannelId;
    if (!id || id === 'default') return 'ch-1';
    return String(id);
}

export function normalizeFsPath(p: string): string {
    return p.replace(/\\/g, '/').trim().toLowerCase();
}

/** Program-like rows whose MediaItem.priority we demote when new show-hour content arrives. */
export function isDemotableProgramLikeRow(item: PlaylistItem): boolean {
    const t = item.media?.type;
    if (!item.media || !t) return false;
    if (t === 'ad' || t === 'gap') return false;
    return true;
}

export function demoteProgramLikeRows(items: PlaylistItem[]): PlaylistItem[] {
    return items.map((row) => {
        if (!isDemotableProgramLikeRow(row)) return row;
        return {
            ...row,
            media: {
                ...row.media,
                priority: SHOW_HOUR_LOW_PRIORITY,
            },
        };
    });
}

export function mergeShowHourAppendItems(existing: PlaylistItem[], newRows: PlaylistItem[]): PlaylistItem[] {
    const demoted = demoteProgramLikeRows(existing);
    const normalizedNew = newRows.map((row) => ({
        ...row,
        media: {
            ...row.media,
            priority:
                typeof row.media.priority === 'number' && Number.isFinite(row.media.priority)
                    ? row.media.priority
                    : SHOW_HOUR_NORMAL_PRIORITY,
        },
    }));
    return [...demoted, ...normalizedNew];
}

export function hashPathForShowHourId(absPath: string): string {
    let h = 2166136261 >>> 0;
    const s = absPath;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return `showhour_${h.toString(36)}`;
}

export function createPlaylistItemFromMedia(media: MediaItem, newRowId: string): PlaylistItem {
    return {
        id: newRowId,
        mediaId: media.id,
        media: { ...media },
        isResolved: true,
    };
}

/**
 * Resolve or create a MediaItem for a disk path (Electron), save new rows to the library DB.
 */
export async function resolveOrCreateMediaFromDiskPath(absPath: string): Promise<MediaItem> {
    const api = (typeof window !== 'undefined' ? (window as unknown as { electronAPI?: any }).electronAPI : null) as
        | {
              getFileInfo?: (p: string) => Promise<{ duration?: number }>;
              getFileUrl?: (p: string) => Promise<string>;
          }
        | undefined;

    const basename = absPath.replace(/^.*[/\\]/, '') || absPath;
    const norm = normalizeFsPath(absPath);

    const hits = await dbFindByFilename(basename);
    const existing = hits.find((m) => m.filePath && normalizeFsPath(m.filePath) === norm);
    if (existing) {
        return {
            ...existing,
            priority: SHOW_HOUR_NORMAL_PRIORITY,
        };
    }

    let duration = 0;
    if (api?.getFileInfo) {
        try {
            const info = await api.getFileInfo(absPath);
            duration = typeof info?.duration === 'number' ? info.duration : 0;
        } catch {
            duration = 0;
        }
    }

    const meta = autoDetectMediaMetadata(basename, duration);
    let src = `file://${absPath}`;
    if (api?.getFileUrl) {
        try {
            src = await api.getFileUrl(absPath);
        } catch {
            /* keep file:// */
        }
    }

    const id = hashPathForShowHourId(absPath);
    const media: MediaItem = {
        id,
        title: meta.title || basename,
        duration,
        src,
        type: meta.type,
        filePath: absPath,
        filename: basename,
        priority: SHOW_HOUR_NORMAL_PRIORITY,
        mediaClass: meta.mediaClass,
        detectionNotes: meta.detectionNotes,
        client: meta.client,
        bumperSubtype: meta.bumperSubtype,
        episodeDate: meta.episodeDate,
    };

    if (getEnvironment().isElectron) {
        await dbSaveMedia(media);
    }

    return media;
}

export function slotHasFilePath(slot: HourSlot | null | undefined, absPath: string): boolean {
    if (!slot?.items?.length) return false;
    const n = normalizeFsPath(absPath);
    return slot.items.some((it) => it.media?.filePath && normalizeFsPath(it.media.filePath) === n);
}
