/**
 * Platform detection utility
 * Safely gets the platform in both browser and Electron environments
 */

export function getPlatform(): string {
    // In Electron, use the exposed platform from preload
    if (typeof window !== 'undefined' && (window as any).electronAPI?.platform) {
        return (window as any).electronAPI.platform;
    }
    
    // Fallback to process.platform if available (Node.js environment)
    if (typeof process !== 'undefined' && process.platform) {
        return process.platform;
    }
    
    // Default fallback - detect from user agent or default to win32
    if (typeof navigator !== 'undefined') {
        const userAgent = navigator.userAgent.toLowerCase();
        if (userAgent.includes('win')) return 'win32';
        if (userAgent.includes('mac')) return 'darwin';
        if (userAgent.includes('linux')) return 'linux';
    }
    
    // Default to win32 for Windows users
    return 'win32';
}

export const platform = getPlatform();
export const isWindows = platform === 'win32';
export const isMac = platform === 'darwin';
export const isLinux = platform === 'linux';




