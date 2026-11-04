/**
 * Universal File URL Resolver
 * 
 * Provides consistent file URL resolution across all contexts:
 * - Electron (via file server at localhost:8080)
 * - Browser (blob URLs with IndexedDB persistence)
 * - CasperCG (relative clip names from media path)
 * 
 * This is the single source of truth for file URL handling.
 */

import { MediaItem } from './types';
import { persistentLogger } from './persistentLogger';
import { logError } from './errorBus';

// ============================================
// ENVIRONMENT DETECTION
// ============================================

export interface FileResolverEnv {
    isElectron: boolean;
    isBrowser: boolean;
    electronAPI: any | null;
    fileServerPort: number;
    /** Dedicated Electron media HTTP server (`/m?path=`) when > 0 */
    mediaHttpPort: number;
}

let cachedEnv: FileResolverEnv | null = null;
let fileServerPort = 8080; // Default, will be updated dynamically
let mediaHttpPort = 0;

/**
 * Clear the environment cache so the next getEnvironment() re-detects (e.g. after electronAPI becomes available in a child window).
 */
export function clearEnvironmentCache(): void {
    cachedEnv = null;
}

/**
 * Wait for electronAPI to become available (e.g. in output window where preload may run after first paint).
 * @param timeoutMs - Max time to wait (default 3000)
 * @returns true if electronAPI is available, false if timeout
 */
export function waitForElectronAPI(timeoutMs = 3000): Promise<boolean> {
    if (typeof window === 'undefined') return Promise.resolve(false);
    const api = (window as any).electronAPI;
    if (api) return Promise.resolve(true);
    return new Promise((resolve) => {
        const deadline = Date.now() + timeoutMs;
        const check = () => {
            if ((window as any).electronAPI) {
                resolve(true);
                return;
            }
            if (Date.now() >= deadline) {
                resolve(false);
                return;
            }
            setTimeout(check, 50);
        };
        setTimeout(check, 50);
    });
}

/**
 * Detect the current environment.
 * Uses cache; call clearEnvironmentCache() if electronAPI may have become available after initial load (e.g. output window).
 */
export function getEnvironment(): FileResolverEnv {
    const isElectronDetected = typeof window !== 'undefined' && !!(window as any).electronAPI;
    const electronAPI = isElectronDetected ? (window as any).electronAPI : null;

    if (!cachedEnv || cachedEnv.isElectron !== isElectronDetected) {
        cachedEnv = {
            isElectron: isElectronDetected,
            isBrowser: !isElectronDetected,
            electronAPI,
            fileServerPort,
            mediaHttpPort,
        };
    } else {
        cachedEnv.electronAPI = electronAPI;
        cachedEnv.mediaHttpPort = mediaHttpPort;
        cachedEnv.fileServerPort = fileServerPort;
    }

    return cachedEnv;
}

/**
 * Set the file server port (called when Electron provides it)
 */
export function setFileServerPort(port: number): void {
    fileServerPort = port;
    if (cachedEnv) {
        cachedEnv.fileServerPort = port;
    }
}

export function setMediaHttpPort(port: number): void {
    mediaHttpPort = Math.max(0, Math.floor(Number(port)) || 0);
    if (cachedEnv) {
        cachedEnv.mediaHttpPort = mediaHttpPort;
    }
}

// ============================================
// URL RESOLUTION
// ============================================

/**
 * Resolve a file path to a playable URL
 * 
 * @param filePath - The file path (absolute or relative)
 * @param context - The context requesting the URL
 * @returns A URL that can be used for playback
 */
/** Reject paths that point at DB or non-media files (avoids DEMUXER_ERROR from player loading wrong file) */
function isBlockedPath(filePath: string): boolean {
    const lower = filePath.toLowerCase().trim();
    return lower.endsWith('.db') || lower.includes('asrun.db') || lower.includes('media.db') || /[\\/]\.db$/.test(lower);
}

export async function resolveFileUrl(
    filePath: string | undefined | null,
    context: 'player' | 'thumbnail' | 'casper' = 'player'
): Promise<string> {
    if (!filePath || isBlockedPath(filePath)) {
        return '';
    }

    const env = getEnvironment();

    // Already a valid URL?
    if (filePath.startsWith('http://') || filePath.startsWith('https://')) {
        return filePath;
    }

    // Blob URL (browser mode)
    if (filePath.startsWith('blob:')) {
        return filePath;
    }

    // Data URL (embedded)
    if (filePath.startsWith('data:')) {
        return filePath;
    }

    // Signal URL (test patterns)
    if (filePath.startsWith('signal:')) {
        return filePath;
    }

    // File URL - convert to HTTP server URL in Electron
    if (filePath.startsWith('file:///') || filePath.startsWith('file://')) {
        filePath = decodeFileUrl(filePath);
    }

    const encodedPath = encodeURIComponent(filePath);

    // In Electron: use custom protocol (avoids CORS when app loads from file://)
    if (env.isElectron) {
        return `playout-media://localhost/?path=${encodedPath}`;
    }

    if (env.mediaHttpPort > 0) {
        return `http://127.0.0.1:${env.mediaHttpPort}/m?path=${encodedPath}`;
    }
    return `http://localhost:${env.fileServerPort}/file?path=${encodedPath}`;
}

/**
 * Synchronously resolve a file URL (for cases where async isn't possible)
 * Uses direct URL construction instead of IPC
 */
export function resolveFileUrlSync(filePath: string | undefined | null): string {
    if (!filePath || isBlockedPath(filePath)) {
        return '';
    }

    const env = getEnvironment();

    // Already a valid URL?
    if (filePath.startsWith('http://') || filePath.startsWith('https://')) {
        return filePath;
    }

    // Blob URL (browser mode)
    if (filePath.startsWith('blob:')) {
        return filePath;
    }

    // Data URL (embedded)
    if (filePath.startsWith('data:')) {
        return filePath;
    }

    // Signal URL (test patterns)
    if (filePath.startsWith('signal:')) {
        return filePath;
    }

    // File URL - convert to HTTP server URL
    if (filePath.startsWith('file:///') || filePath.startsWith('file://')) {
        filePath = decodeFileUrl(filePath);
    }

    const encodedPath = encodeURIComponent(filePath);

    // In Electron: use custom protocol (avoids CORS when app loads from file://)
    if (env.isElectron) {
        return `playout-media://localhost/?path=${encodedPath}`;
    }

    // Browser-only dev mode cannot read arbitrary local disk paths.
    // Returning empty here prevents MEDIA_ELEMENT_ERROR spam from dead localhost:8080 URLs.
    if (!env.isElectron && typeof window !== 'undefined' && /localhost:5173|127\.0\.0\.1:5173/.test(window.location.host)) {
        return '';
    }

    if (env.mediaHttpPort > 0) {
        return `http://127.0.0.1:${env.mediaHttpPort}/m?path=${encodedPath}`;
    }
    return `http://localhost:${env.fileServerPort}/file?path=${encodedPath}`;
}

/**
 * Decode a file:// URL back to a file path
 */
export function decodeFileUrl(fileUrl: string): string {
    if (!fileUrl.startsWith('file://')) {
        return fileUrl;
    }

    // Remove file:// prefix
    let path = fileUrl.replace(/^file:\/\/\/?/, '');

    // Decode URI components
    path = decodeURIComponent(path);

    // Handle Windows paths (file:///C:/...)
    if (/^[A-Za-z]:/.test(path)) {
        return path;
    }

    // Handle paths that start with / but have Windows drive after
    if (/^\/[A-Za-z]:/.test(path)) {
        return path.substring(1);
    }

    return path;
}

// ============================================
// MEDIA ITEM URL RESOLUTION
// ============================================

/**
 * Get a playable URL from a MediaItem
 * Tries multiple sources in order of preference
 */
export async function resolveMediaUrl(media: MediaItem): Promise<string> {
    // Priority 1: filePath (most reliable for Electron)
    if (media.filePath) {
        const url = await resolveFileUrl(media.filePath, 'player');
        if (url) return url;
    }

    // Priority 2: src (may already be a valid URL)
    if (media.src) {
        // If it's already an HTTP URL from our server, make sure it doesn't point at a blocked path (.db, asrun.db, media.db)
        if (media.src.startsWith('http://localhost:')) {
            if (media.src.includes('/file?path=')) {
                const match = media.src.match(/path=([^&]+)/);
                if (match) {
                    const decodedPath = decodeURIComponent(match[1]);
                    if (isBlockedPath(decodedPath)) {
                        // Treat blocked targets as invalid so callers can fall back cleanly
                        return '';
                    }
                }
            }
            return media.src;
        }

        // If it's a valid non-file URL (including our custom protocol)
        if (media.src.startsWith('blob:') ||
            media.src.startsWith('data:') ||
            media.src.startsWith('signal:') ||
            media.src.startsWith('https://') ||
            media.src.startsWith('playout-media://')) {
            return media.src;
        }

        // Try to resolve it
        const url = await resolveFileUrl(media.src, 'player');
        if (url) return url;
    }

    // Priority 3: storedFileId (IndexedDB in browser mode)
    if (media.storedFileId && getEnvironment().isBrowser) {
        // This needs to be handled by the caller importing browserFileStorage
        // Return a marker that indicates IndexedDB lookup is needed
        return `indexeddb:${media.storedFileId}`;
    }

    // No valid source
    return '';
}

/**
 * Synchronous version for cases where async isn't possible
 */
export function resolveMediaUrlSync(media: MediaItem): string {
    // Priority 1: filePath
    if (media.filePath) {
        const url = resolveFileUrlSync(media.filePath);
        if (url) return url;
    }

    // Priority 2: src
    if (media.src) {
        if (media.src.startsWith('http://localhost:')) {
            if (media.src.includes('/file?path=')) {
                const match = media.src.match(/path=([^&]+)/);
                if (match) {
                    const decodedPath = decodeURIComponent(match[1]);
                    if (isBlockedPath(decodedPath)) {
                        return '';
                    }
                }
            }
            return media.src;
        }

        if (media.src.startsWith('blob:') ||
            media.src.startsWith('data:') ||
            media.src.startsWith('signal:') ||
            media.src.startsWith('https://') ||
            media.src.startsWith('playout-media://')) {
            return media.src;
        }

        const url = resolveFileUrlSync(media.src);
        if (url) return url;
    }

    return '';
}

// ============================================
// CASPERCG CLIP RESOLUTION
// ============================================

export interface CasperClipOptions {
    mediaPath?: string;  // CasparCG media-path from config
    stripExtension?: boolean;
    /** When true with a patched server + config flag, pass full absolute/UNC paths (keep extension). */
    allowAbsoluteMediaPaths?: boolean;
}

/**
 * Resolve a MediaItem to a CasparCG clip name.
 * Default: relative clip under media-path (extension stripped). With **allowAbsoluteMediaPaths**,
 * out-of-root files stay as full paths and keep their extension for patched servers.
 */
export function resolveCasperClip(media: MediaItem, options: CasperClipOptions = {}): string | null {
    const { mediaPath, stripExtension = true, allowAbsoluteMediaPaths = false } = options;

    // Get the file path
    let filePath = media.filePath || media.src;
    let keepExtensionForAbsoluteOutsideRoot = false;
    if (!filePath) return null;

    // Decode if it's a file:// URL
    if (filePath.startsWith('file://')) {
        filePath = decodeFileUrl(filePath);
    }

    // Decode if it's an http URL from our server
    if (filePath.startsWith('http://localhost:') && filePath.includes('/file?path=')) {
        const match = filePath.match(/path=([^&]+)/);
        if (match) {
            filePath = decodeURIComponent(match[1]);
        }
    }

    // Normalize path separators to forward slashes for consistent comparison
    filePath = filePath.replace(/\\/g, '/');

    // If we have a mediaPath, try to make it relative
    if (mediaPath) {
        const normalizedMediaPath = mediaPath.replace(/\\/g, '/').replace(/\/$/, '');

        // Check if filePath starts with mediaPath (case-insensitive for Windows)
        if (filePath.toLowerCase().startsWith(normalizedMediaPath.toLowerCase())) {
            // Extract relative path
            let relativePath = filePath.substring(normalizedMediaPath.length);
            // Remove leading slash if present
            if (relativePath.startsWith('/')) {
                relativePath = relativePath.substring(1);
            }
            filePath = relativePath;
        } else if (allowAbsoluteMediaPaths) {
            keepExtensionForAbsoluteOutsideRoot = true;
        } else {
            persistentLogger.warn(
                `[FileResolver] File path "${filePath}" is not within CasparCG media path "${normalizedMediaPath}". Using filename only.`,
                undefined,
                'FileResolver',
            );
            filePath = filePath.split('/').pop() || filePath;
        }
    }

    // Get the filename/clip name (if we didn't already reduce it to relative path)
    // Actually, if we successfully made it relative, we want to keep the folder structure (e.g. "subfolder/clip")
    // So we don't want to just pop the filename unless we failed to match the media path.

    // Strip extension if requested (never strip for absolute out-of-root with allow-absolute mode)
    if (stripExtension && !keepExtensionForAbsoluteOutsideRoot) {
        filePath = filePath.replace(/\.[^.]+$/, '');
    }

    return filePath || null;
}

// ============================================
// THUMBNAIL GENERATION
// ============================================

/**
 * Generate a thumbnail from a media file
 * Works in both Electron and browser contexts
 */
export async function generateThumbnail(
    source: string | File,
    seekPercent: number = 0.1
): Promise<string | null> {
    return new Promise((resolve) => {
        const video = document.createElement('video');
        video.preload = 'metadata';
        video.crossOrigin = 'anonymous';

        video.onloadedmetadata = () => {
            const canvas = document.createElement('canvas');
            canvas.width = 320;
            canvas.height = 180;
            const ctx = canvas.getContext('2d');

            if (!ctx) {
                resolve(null);
                return;
            }

            // Seek to a position
            video.currentTime = Math.min(video.duration * seekPercent, 5);

            video.onseeked = () => {
                ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
                const thumbnail = canvas.toDataURL('image/jpeg', 0.7);
                resolve(thumbnail);
            };
        };

        video.onerror = () => {
            resolve(null);
        };

        // Set the source
        if (source instanceof File) {
            video.src = URL.createObjectURL(source);
        } else {
            // Resolve the URL first
            resolveFileUrl(source, 'thumbnail').then(url => {
                if (url) {
                    video.src = url;
                } else {
                    resolve(null);
                }
            });
        }
    });
}

// ============================================
// INITIALIZATION
// ============================================

/**
 * Initialize the file resolver
 * Call this early in app startup to cache environment and get server port
 */
export async function initFileResolver(): Promise<void> {
    const env = getEnvironment();

    if (env.isElectron && env.electronAPI?.getOutputUrl) {
        try {
            for (let attempt = 0; attempt < 8; attempt++) {
                const result = await env.electronAPI.getOutputUrl();
                if (typeof result !== 'object' || !result?.port) {
                    if (typeof result === 'string') {
                        const match = result.match(/:(\d+)\//);
                        if (match) {
                            setFileServerPort(parseInt(match[1], 10));
                            persistentLogger.debug('[FileResolver] Server port set to: ' + match[1], undefined, 'FileResolver');
                        }
                    }
                    break;
                }
                setFileServerPort(result.port);
                console.log('[FileResolver] Server port set to:', result.port);
                if (typeof result.mediaHttpPort === 'number' && result.mediaHttpPort > 0) {
                    setMediaHttpPort(result.mediaHttpPort);
                    console.log('[FileResolver] Media HTTP port set to:', result.mediaHttpPort);
                    break;
                }
                if (attempt < 7) await new Promise((r) => setTimeout(r, 200));
            }
        } catch (err) {
            console.error('[FileResolver] Failed to get server port:', err);
        }
    }
}

// Auto-initialize when module loads
if (typeof window !== 'undefined') {
    // Delay initialization to ensure electronAPI is available
    setTimeout(() => {
        initFileResolver().catch((e) => logError('[FileResolver] init failed', e));
    }, 100);
}



