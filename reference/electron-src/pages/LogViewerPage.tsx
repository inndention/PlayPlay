// ============================================
// Standalone Log Viewer Page
// Accessible even when main UI is broken
// ============================================

import React, { useState, useEffect } from 'react';
import { persistentLogger, LogEntry, LogLevel } from '../lib/persistentLogger';
import { X, Download, Trash2, Search, RefreshCw } from 'lucide-react';
import styles from './LogViewerPage.module.css';

export function LogViewerPage() {
    const [logs, setLogs] = useState<LogEntry[]>([]);
    const [filterLevel, setFilterLevel] = useState<LogLevel | 'all'>('all');
    const [filterComponent, setFilterComponent] = useState<string>('');
    const [searchQuery, setSearchQuery] = useState('');
    const [autoRefresh, setAutoRefresh] = useState(true);
    const [lastUpdate, setLastUpdate] = useState(Date.now());

    const loadLogs = () => {
        const newLogs = persistentLogger.getLogs();
        setLogs(newLogs);
        setLastUpdate(Date.now());
    };

    useEffect(() => {
        loadLogs();

        // Subscribe to updates
        const unsubscribe = persistentLogger.onLogUpdate((newLogs) => {
            setLogs(newLogs);
            setLastUpdate(Date.now());
        });

        // Auto-refresh every 500ms if enabled
        let interval: NodeJS.Timeout | null = null;
        if (autoRefresh) {
            interval = setInterval(() => {
                loadLogs();
            }, 500);
        }

        return () => {
            unsubscribe();
            if (interval) clearInterval(interval);
        };
    }, [autoRefresh]);

    // Auto-scroll to bottom
    const logContainerRef = React.useRef<HTMLDivElement>(null);
    useEffect(() => {
        if (logContainerRef.current) {
            logContainerRef.current.scrollTop = logContainerRef.current.scrollHeight;
        }
    }, [logs]);

    const filteredLogs = logs.filter(log => {
        if (filterLevel !== 'all' && log.level !== filterLevel) return false;
        if (filterComponent && log.component !== filterComponent) return false;
        if (searchQuery && !log.message.toLowerCase().includes(searchQuery.toLowerCase())) {
            return false;
        }
        return true;
    });

    const handleExport = () => {
        const text = persistentLogger.exportLogs();
        const blob = new Blob([text], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `playplay-logs-${new Date().toISOString().split('T')[0]}.txt`;
        a.click();
        URL.revokeObjectURL(url);
    };

    const handleClear = () => {
        if (confirm('Clear all logs?')) {
            persistentLogger.clearLogs();
            setLogs([]);
        }
    };

    const getLogLevelColor = (level: LogLevel): string => {
        switch (level) {
            case 'error': return '#ef4444';
            case 'warn': return '#f59e0b';
            case 'info': return '#3b82f6';
            case 'debug': return '#6b7280';
            default: return '#9ca3af';
        }
    };

    const components = Array.from(new Set(logs.map(log => log.component).filter(Boolean)));

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h1>Persistent Logs Viewer</h1>
                <div className={styles.actions}>
                    <button onClick={loadLogs} title="Refresh logs">
                        <RefreshCw size={16} />
                    </button>
                    <button onClick={handleExport} title="Export logs">
                        <Download size={16} />
                    </button>
                    <button onClick={handleClear} title="Clear logs">
                        <Trash2 size={16} />
                    </button>
                </div>
            </div>

            <div className={styles.info}>
                <span>{filteredLogs.length} of {logs.length} logs</span>
                <span>Last update: {new Date(lastUpdate).toLocaleTimeString()}</span>
                {autoRefresh && <span className={styles.autoRefresh}>Auto-refreshing...</span>}
            </div>

            <div className={styles.filters}>
                <div className={styles.filterGroup}>
                    <label>Level:</label>
                    <select value={filterLevel} onChange={(e) => setFilterLevel(e.target.value as any)}>
                        <option value="all">All</option>
                        <option value="error">Error</option>
                        <option value="warn">Warn</option>
                        <option value="info">Info</option>
                        <option value="debug">Debug</option>
                    </select>
                </div>

                <div className={styles.filterGroup}>
                    <label>Component:</label>
                    <select value={filterComponent} onChange={(e) => setFilterComponent(e.target.value)}>
                        <option value="">All</option>
                        {components.map(comp => (
                            <option key={comp} value={comp}>{comp}</option>
                        ))}
                    </select>
                </div>

                <div className={styles.searchGroup}>
                    <Search size={16} />
                    <input
                        type="text"
                        placeholder="Search logs..."
                        value={searchQuery}
                        onChange={(e) => setSearchQuery(e.target.value)}
                    />
                </div>

                <label className={styles.checkboxLabel}>
                    <input
                        type="checkbox"
                        checked={autoRefresh}
                        onChange={(e) => setAutoRefresh(e.target.checked)}
                    />
                    Auto-refresh
                </label>
            </div>

            <div className={styles.logContainer} ref={logContainerRef}>
                {filteredLogs.length === 0 ? (
                    <div className={styles.empty}>No logs found</div>
                ) : (
                    filteredLogs.map((log, index) => (
                        <div key={index} className={styles.logEntry} data-level={log.level}>
                            <span className={styles.timestamp}>
                                {new Date(log.timestamp).toLocaleTimeString()}
                            </span>
                            <span
                                className={styles.level}
                                style={{ color: getLogLevelColor(log.level) }}
                            >
                                [{log.level.toUpperCase()}]
                            </span>
                            {log.component && (
                                <span className={styles.component}>{log.component}:</span>
                            )}
                            <span className={styles.message}>{log.message}</span>
                            {log.data && (
                                <details className={styles.data}>
                                    <summary>Data</summary>
                                    <pre>{JSON.stringify(log.data, null, 2)}</pre>
                                </details>
                            )}
                            {log.stack && (
                                <details className={styles.stack}>
                                    <summary>Stack</summary>
                                    <pre>{log.stack}</pre>
                                </details>
                            )}
                        </div>
                    ))
                )}
            </div>
        </div>
    );
}



