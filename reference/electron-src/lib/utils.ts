import { type ClassValue, clsx } from "clsx";
import { twMerge } from "tailwind-merge";

export function cn(...inputs: ClassValue[]) {
    return twMerge(clsx(inputs));
}

/**
 * Convert a file path to a proper file:// URL
 * Handles Windows paths correctly (C:\Users\... -> file:///C:/Users/...)
 */
export function pathToFileUrl(filePath: string): string {
    if (!filePath) return '';
    
    // If already a file:// URL, return as-is
    if (filePath.startsWith('file://')) {
        return filePath;
    }
    
    // Convert backslashes to forward slashes
    let normalized = filePath.replace(/\\/g, '/');
    
    // Windows drive letter handling: C:/ -> /C:/
    // Match patterns like "C:/" or "C:\" (already converted to "C:/")
    const isWindowsPath = /^[A-Za-z]:/.test(normalized);
    if (isWindowsPath) {
        normalized = '/' + normalized;
    }
    
    // URL encode the path (handles spaces and special chars)
    // But preserve the drive letter format (C:)
    const parts = normalized.split('/');
    const encodedParts = parts.map((part, index) => {
        // Don't encode the first empty part
        if (index === 0 && part === '') return '';
        // Don't encode drive letter (e.g., "C:")
        if (index === 1 && isWindowsPath && /^[A-Za-z]:$/.test(part)) {
            return part;
        }
        // Encode everything else
        return encodeURIComponent(part);
    });
    
    // Reconstruct with proper separators
    const encodedPath = encodedParts.join('/');
    
    // Windows paths already have leading slash, use file://
    // Unix paths need file:///
    if (encodedPath.startsWith('/')) {
        return `file://${encodedPath}`;
    } else {
        return `file:///${encodedPath}`;
    }
}

/**
 * Calendar date as YYYY-MM-DD in the environment's local timezone.
 * Use for schedule slot ids (`${date}-${hour}`) together with getHours/getDay — not `toISOString().split('T')[0]` (UTC).
 */
export function localCalendarDateKey(date: Date = new Date()): string {
    const y = date.getFullYear();
    const m = String(date.getMonth() + 1).padStart(2, '0');
    const d = String(date.getDate()).padStart(2, '0');
    return `${y}-${m}-${d}`;
}

export function formatDuration(seconds: number): string {
    const totalSeconds = Math.round(seconds);
    const m = Math.floor(totalSeconds / 60);
    const s = totalSeconds % 60;
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

export function generateId(): string {
    return Math.random().toString(36).substr(2, 9);
}

/**
 * Format file size in bytes to human-readable format
 */
export function formatFileSize(bytes: number): string {
    if (bytes === 0) return '0 B';
    if (!bytes || isNaN(bytes)) return '-';
    
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    const k = 1024;
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    
    return `${parseFloat((bytes / Math.pow(k, i)).toFixed(2))} ${units[i]}`;
}