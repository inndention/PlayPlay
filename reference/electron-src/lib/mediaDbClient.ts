/**
 * Saved media library (Electron: SQLite; browser: not used).
 * All user-facing wording: "Saved media" / "Library" — avoid "DB" or "database" in UI.
 */
import type { AnalyticsEventRecord, AnalyticsSummary, MediaItem } from './types';
import { resolveFileUrlSync, getEnvironment, decodeFileUrl } from './fileResolver';
import { normalizeMediaPriorityFields } from './mediaPriority';

// Alias for backward compatibility
export async function dbGetAllMedia(): Promise<MediaItem[]> {
    return dbFetchMedia();
}

export async function dbFetchMedia(): Promise<MediaItem[]> {
    const env = getEnvironment();
    if (!env.isElectron) return [];
    
    try {
        const raw = await (window as any).electronAPI!.mediaGetAll();
        const items = raw as MediaItem[];

        // Rebuild stable src from filePath so media stays valid across sessions.
        // Uses the universal file resolver for consistent cross-platform handling.
        return items.map(item => {
            if (!item) return item;

            const row = item as unknown as {
                liveSource?: string;
                segmentCollections?: string;
                segments?: string;
                priorityFrozen?: number | boolean;
                priorityScore?: number | null;
                priorityBurnRateMultiplier?: number | null;
                priorityUpdatedAt?: string | null;
            };
            // Parse liveSource if stored as JSON string (SQLite column)
            if (row.liveSource && typeof row.liveSource === 'string') {
                try {
                    (item as any).liveSource = JSON.parse(row.liveSource);
                } catch {
                    // leave as-is
                }
            }

            // Parse segmentCollections and segments from JSON columns
            let segmentCollections = item.segmentCollections;
            let segments = item.segments;
            if (row.segmentCollections && typeof row.segmentCollections === 'string') {
                try {
                    segmentCollections = JSON.parse(row.segmentCollections);
                } catch {
                    segmentCollections = undefined;
                }
            }
            if (row.segments && typeof row.segments === 'string') {
                try {
                    segments = JSON.parse(row.segments);
                } catch {
                    segments = undefined;
                }
            }

            const priorityFrozen =
                row.priorityFrozen === true ||
                row.priorityFrozen === 1 ||
                String(row.priorityFrozen) === '1';

            const base = normalizeMediaPriorityFields({
                ...item,
                segmentCollections,
                segments,
                priorityFrozen,
                priorityScore: row.priorityScore != null ? Number(row.priorityScore) : item.priorityScore,
                priorityBurnRateMultiplier:
                    row.priorityBurnRateMultiplier != null
                        ? Number(row.priorityBurnRateMultiplier)
                        : item.priorityBurnRateMultiplier,
                priorityUpdatedAt: row.priorityUpdatedAt ?? item.priorityUpdatedAt,
            });

            // Ensure filePath is a string (DB might return null)
            const filePath = (item.filePath || '').trim();

            if (filePath) {
                const src = resolveFileUrlSync(filePath);
                return {
                    ...base,
                    filePath,
                    src: src || 'signal:bars',
                    filename: item.filename || filePath.split(/[\\/]/).pop() || item.title || '',
                };
            }

            if (item.src && item.src.startsWith('file://')) {
                const extractedPath = decodeFileUrl(item.src);
                const src = resolveFileUrlSync(extractedPath);
                return {
                    ...base,
                    filePath: extractedPath,
                    filename: item.filename || extractedPath.split(/[\\/]/).pop() || item.title || '',
                    src: src || item.src,
                };
            }

            if (item.src && !item.src.startsWith('blob:')) {
                return { ...base, filename: item.filename || item.title || '' };
            }

            return {
                ...base,
                src: 'signal:bars',
                filename: item.filename || item.title || '',
            };
        });
    } catch (e) {
        console.error('[MediaDB] fetchMedia failed:', e);
        return [];
    }
}

export async function dbSaveMedia(item: MediaItem): Promise<void> {
    if (!getEnvironment().isElectron) return;
    try {
        await (window as any).electronAPI!.mediaSave(item);
    } catch (e) {
        console.error('[MediaDB] saveMedia failed:', e);
    }
}

export async function dbDeleteMedia(id: string): Promise<void> {
    if (!getEnvironment().isElectron) return;
    try {
        await (window as any).electronAPI!.mediaDelete(id);
    } catch (e) {
        console.error('[MediaDB] deleteMedia failed:', e);
    }
}

export async function dbIncrementPlay(mediaId: string, durationSeconds: number): Promise<void> {
    if (!getEnvironment().isElectron) return;
    try {
        await (window as any).electronAPI!.mediaIncrementPlay(mediaId, durationSeconds);
    } catch (e) {
        console.error('[MediaDB] incrementPlay failed:', e);
    }
}

export async function dbFindByFilename(query: string): Promise<MediaItem[]> {
    if (!getEnvironment().isElectron) return [];
    try {
        const res = await (window as any).electronAPI!.mediaFindByFilename(query);
        return res as MediaItem[];
    } catch (e) {
        console.error('[MediaDB] findByFilename failed:', e);
        return [];
    }
}

export async function dbLogAnalyticsEvent(payload: Record<string, unknown>): Promise<void> {
    if (!getEnvironment().isElectron) return;
    try {
        await (window as any).electronAPI!.analyticsLogEvent(payload);
    } catch (e) {
        console.error('[MediaDB] analyticsLogEvent failed:', e);
    }
}

export async function dbGetAnalyticsEvents(filters?: Record<string, unknown>): Promise<AnalyticsEventRecord[]> {
    if (!getEnvironment().isElectron) return [];
    try {
        return await (window as any).electronAPI!.analyticsGetEvents(filters || {});
    } catch (e) {
        console.error('[MediaDB] analyticsGetEvents failed:', e);
        return [];
    }
}

export async function dbGetAnalyticsSummary(): Promise<AnalyticsSummary | null> {
    if (!getEnvironment().isElectron) return null;
    try {
        return await (window as any).electronAPI!.analyticsGetSummary();
    } catch (e) {
        console.error('[MediaDB] analyticsGetSummary failed:', e);
        return null;
    }
}

