/**
 * Build absolute Caspar <media-path> when ramdisk routing is enabled in engine settings.
 * Caspar on Windows accepts forward slashes in XML (e.g. B:/PlayPlay-caspar-media/).
 */
import { mergeEngineApproaches, type EngineApproachesMergeInput } from './engineApproaches';
import { loadSettings } from './storage';

/** Resolved Caspar `<media-path>` for the current automation settings (or undefined if disabled). */
export function getCasparMediaPathForCurrentSettings(): string | undefined {
    return buildCasparMediaPathFromEngineConfig(loadSettings().engineApproaches);
}

export function buildCasparMediaPathFromEngineConfig(
    partial?: EngineApproachesMergeInput | null
): string | undefined {
    const ea = mergeEngineApproaches(partial ?? undefined);
    const r = ea.ramdisk;
    if (!r?.enableCasparMediaOnRamdisk) return undefined;
    const root = (r.root || '').trim();
    if (!root) return undefined;
    let norm = root.replace(/\\/g, '/').replace(/\/+$/, '');
    if (/^[a-zA-Z]:$/.test(norm)) norm += '/';
    const folder = (r.casparMediaFolder || 'PlayPlay-caspar-media')
        .replace(/^[\\/]+/, '')
        .replace(/\\/g, '/')
        .replace(/\/+$/, '');
    const joined = `${norm}/${folder}/`.replace(/([^:]\/)\/+/g, '$1');
    return joined;
}
