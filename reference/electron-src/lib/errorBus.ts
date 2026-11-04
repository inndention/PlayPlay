export type ErrorLevel = 'error' | 'warning' | 'info';

export interface ErrorEntry {
    id: string;
    message: string;
    level: ErrorLevel;
    detail?: any;
    ts: number;
}

const STORAGE_KEY = 'playplay_error_log';
const MAX_ENTRIES = 200;
const EVENT_NAME = 'app-error';

function loadEntries(): ErrorEntry[] {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return [];
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) return parsed as ErrorEntry[];
    } catch {
        /* ignore */
    }
    return [];
}

function saveEntries(entries: ErrorEntry[]) {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(entries.slice(-MAX_ENTRIES)));
    } catch {
        /* ignore */
    }
}

function broadcast(entry: ErrorEntry) {
    if (typeof window !== 'undefined') {
        window.dispatchEvent(new CustomEvent(EVENT_NAME, { detail: entry }));
    }
}

function push(message: string, level: ErrorLevel, detail?: any): ErrorEntry {
    const entry: ErrorEntry = {
        id: `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`,
        message,
        level,
        detail,
        ts: Date.now(),
    };
    const entries = loadEntries();
    entries.push(entry);
    saveEntries(entries);
    broadcast(entry);
    return entry;
}

export function logError(message: string, detail?: any) {
    return push(message, 'error', detail);
}

export function logWarning(message: string, detail?: any) {
    return push(message, 'warning', detail);
}

export function logInfo(message: string, detail?: any) {
    return push(message, 'info', detail);
}

export function loadErrorLog(): ErrorEntry[] {
    return loadEntries();
}

export function clearErrorLog(id?: string) {
    if (!id) {
        saveEntries([]);
        broadcast({ id: 'cleared', message: 'Error log cleared', level: 'info', ts: Date.now() });
        return;
    }
    const entries = loadEntries().filter(e => e.id !== id);
    saveEntries(entries);
    broadcast({ id, message: '__removed__', level: 'info', ts: Date.now() });
}

export const ERROR_EVENT_NAME = EVENT_NAME;
