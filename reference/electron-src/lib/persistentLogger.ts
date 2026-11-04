// ============================================
// Persistent Logger
// Logs that survive state resets and can be read even when console is cleared
// ============================================

import { reportingService } from './reportingService';

export type LogLevel = 'debug' | 'info' | 'warn' | 'error';

export interface LogEntry {
    timestamp: number;
    level: LogLevel;
    message: string;
    data?: any;
    component?: string;
    stack?: string;
}

class PersistentLogger {
    private maxEntries = 1000; // Keep last 1000 entries
    private logs: LogEntry[] = [];
    private listeners: Set<(logs: LogEntry[]) => void> = new Set();

    constructor() {
        this.loadLogs();
    }

    /**
     * Load logs from localStorage
     */
    private loadLogs(): void {
        try {
            const stored = localStorage.getItem('persistent_logs');
            if (stored) {
                this.logs = JSON.parse(stored);
                // Keep only recent entries
                if (this.logs.length > this.maxEntries) {
                    this.logs = this.logs.slice(-this.maxEntries);
                }
            }
        } catch (e) {
            console.error('[PersistentLogger] Failed to load logs:', e);
        }
    }

    private lastSaveTime = 0;
    private saveTimeout: NodeJS.Timeout | null = null;

    /**
     * Save logs to localStorage - debounced and throttled
     */
    private saveLogs(): void {
        const now = Date.now();
        const throttleLimit = 500; // Only save every 500ms max

        if (this.saveTimeout) {
            clearTimeout(this.saveTimeout);
        }

        const doSave = () => {
            try {
                this.lastSaveTime = Date.now();
                this.saveTimeout = null;

                // Keep only recent entries
                if (this.logs.length > this.maxEntries) {
                    this.logs = this.logs.slice(-this.maxEntries);
                }
                localStorage.setItem('persistent_logs', JSON.stringify(this.logs));
                
                // Also try to write to file if in Electron
                if (typeof window !== 'undefined' && (window as any).electronAPI?.writeLog) {
                    // Only send the NEWEST logs to avoid writing massive blocks
                    const newestLogs = this.logs.slice(-10);
                    const logText = newestLogs
                        .map(log => {
                            const time = new Date(log.timestamp).toISOString();
                            const data = log.data ? ` | ${JSON.stringify(log.data)}` : '';
                            const stack = log.stack ? `\n${log.stack}` : '';
                            return `[${time}] [${log.level.toUpperCase()}] ${log.component || 'APP'}: ${log.message}${data}${stack}`;
                        })
                        .join('\n');
                    
                    (window as any).electronAPI.writeLog(logText).catch(() => {});
                }
            } catch (e) {
                console.error('[PersistentLogger] Failed to save logs:', e);
            }
        };

        if (now - this.lastSaveTime > throttleLimit) {
            doSave();
        } else {
            this.saveTimeout = setTimeout(doSave, throttleLimit);
        }
    }

    /**
     * Add a log entry
     */
    private addLog(level: LogLevel, message: string, data?: any, component?: string): void {
        const entry: LogEntry = {
            timestamp: Date.now(),
            level,
            message,
            data: data ? (typeof data === 'object' ? JSON.parse(JSON.stringify(data)) : data) : undefined,
            component,
            stack: level === 'error' ? new Error().stack : undefined,
        };

        this.logs.push(entry);
        this.saveLogs();
        this.notifyListeners();

        // Also log to console for immediate visibility
        const consoleMethod = level === 'error' ? 'error' : level === 'warn' ? 'warn' : 'log';
        console[consoleMethod](`[${component || 'APP'}] ${message}`, data || '');
    }

    debug(message: string, data?: any, component?: string): void {
        this.addLog('debug', message, data, component);
    }

    info(message: string, data?: any, component?: string): void {
        this.addLog('info', message, data, component);
    }

    warn(message: string, data?: any, component?: string): void {
        this.addLog('warn', message, data, component);
    }

    error(message: string, data?: any, component?: string): void {
        this.addLog('error', message, data, component);
    }

    /**
     * Get all logs
     */
    getLogs(level?: LogLevel, component?: string): LogEntry[] {
        let filtered = this.logs;
        if (level) {
            filtered = filtered.filter(log => log.level === level);
        }
        if (component) {
            filtered = filtered.filter(log => log.component === component);
        }
        return filtered;
    }

    /**
     * Get recent logs (last N entries)
     */
    getRecentLogs(count: number = 50): LogEntry[] {
        return this.logs.slice(-count);
    }

    /**
     * Clear all logs
     */
    clearLogs(): void {
        this.logs = [];
        this.saveLogs();
        this.notifyListeners();
    }

    /**
     * Export logs as text
     */
    exportLogs(): string {
        return this.logs
            .map(log => {
                const time = new Date(log.timestamp).toISOString();
                const data = log.data ? ` | ${JSON.stringify(log.data)}` : '';
                const stack = log.stack ? `\n${log.stack}` : '';
                return `[${time}] [${log.level.toUpperCase()}] ${log.component || 'APP'}: ${log.message}${data}${stack}`;
            })
            .join('\n');
    }

    /**
     * Export specific audit logs as CSV (for regulatory compliance)
     */
    exportAsPlayedCSV(startTime: number, endTime: number): string {
        const auditLogs = this.logs.filter(l => 
            l.timestamp >= startTime && 
            l.timestamp <= endTime && 
            (l.component === 'Playout' || l.component === 'Automation' || l.message.includes('Play'))
        );

        const headers = ['Timestamp', 'Level', 'Component', 'Message', 'Data'];
        const rows = auditLogs.map(l => [
            new Date(l.timestamp).toISOString(),
            l.level,
            l.component || 'APP',
            `"${l.message.replace(/"/g, '""')}"`,
            l.data ? `"${JSON.stringify(l.data).replace(/"/g, '""')}"` : ''
        ].join(','));

        return [headers.join(','), ...rows].join('\n');
    }

    /**
     * Subscribe to log updates
     */
    onLogUpdate(callback: (logs: LogEntry[]) => void): () => void {
        this.listeners.add(callback);
        return () => this.listeners.delete(callback);
    }

    private notifyListeners(): void {
        this.listeners.forEach(callback => {
            try {
                callback(this.logs);
            } catch (e) {
                console.error('[PersistentLogger] Listener error:', e);
            }
        });
    }
}

export const persistentLogger = new PersistentLogger();

// Auto-log unhandled errors
if (typeof window !== 'undefined') {
    window.addEventListener('error', (event) => {
        persistentLogger.error('Unhandled error', {
            message: event.message,
            filename: event.filename,
            lineno: event.lineno,
            colno: event.colno,
        }, 'GLOBAL');
    });

    window.addEventListener('unhandledrejection', (event) => {
        const reason = event.reason;
        // Suppress CopilotKit GraphQL errors from persistent logging
        if (reason && typeof reason === 'object') {
            const payload = reason.payload || reason.message || '';
            if (typeof payload === 'string' && (
                payload.includes('Unexpected parameter "method"') ||
                payload.includes('GraphQL') ||
                payload.includes('BAD_REQUEST')
            )) {
                // Don't log CopilotKit GraphQL errors - they're expected and harmless
                return;
            }
        }
        persistentLogger.error('Unhandled promise rejection', {
            reason: event.reason,
        }, 'GLOBAL');
    });

    // Expose logger to window for console access
    (window as any).getLogs = () => persistentLogger.getLogs();
    (window as any).getRecentLogs = (count?: number) => persistentLogger.getRecentLogs(count);
    (window as any).exportLogs = () => persistentLogger.exportLogs();
    (window as any).clearLogs = () => persistentLogger.clearLogs();
    (window as any).viewLogs = () => {
        window.location.href = '/#/logs';
    };
    (window as any).reportingService = reportingService;
    
    console.log('%c[PersistentLogger]', 'color: #3b82f6; font-weight: bold', 'Logger initialized. Use window.getLogs(), window.exportLogs(), or window.viewLogs() to access logs.');
}

