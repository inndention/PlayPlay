import React, { useEffect, useMemo, useState } from 'react';
import styles from './ErrorConsole.module.css';
import { ERROR_EVENT_NAME, ErrorEntry, clearErrorLog, loadErrorLog } from '../lib/errorBus';
import { X, Bug, Trash2, Clock } from 'lucide-react';

export function ErrorConsole() {
    const [entries, setEntries] = useState<ErrorEntry[]>(() => loadErrorLog());
    const [open, setOpen] = useState(false);

    useEffect(() => {
        const handler = (evt: Event) => {
            const detail = (evt as CustomEvent<ErrorEntry>).detail;
            if (!detail) return;
            if (detail.message === '__removed__') {
                setEntries(loadErrorLog());
                return;
            }
            setEntries(loadErrorLog());
        };
        window.addEventListener(ERROR_EVENT_NAME, handler as EventListener);
        return () => window.removeEventListener(ERROR_EVENT_NAME, handler as EventListener);
    }, []);

    const unreadCount = useMemo(() => entries.length, [entries]);

    const handleDismiss = (id: string) => {
        clearErrorLog(id);
        setEntries(loadErrorLog());
    };

    const handleClearAll = () => {
        clearErrorLog();
        setEntries([]);
    };

    const levelDot = (level: ErrorEntry['level']) => {
        if (level === 'error') return styles.dotError;
        if (level === 'warning') return styles.dotWarn;
        return styles.dotInfo;
    };

    return (
        <div className={`${styles.console} ${open ? styles.open : ''}`}>
            <button
                className={styles.fab}
                onClick={() => setOpen(v => !v)}
                aria-label="Open error console"
            >
                <Bug size={16} />
                {unreadCount > 0 && <span className={styles.badge}>{unreadCount}</span>}
            </button>

            {open && (
                <div className={styles.panel}>
                    <div className={styles.panelHeader}>
                        <div className={styles.title}>
                            <Bug size={16} /> Error Console
                        </div>
                        <div className={styles.actions}>
                            <button onClick={handleClearAll} title="Clear all" tabIndex={0}>
                                <Trash2 size={14} />
                            </button>
                            <button onClick={() => setOpen(false)} title="Close" tabIndex={0}>
                                <X size={14} />
                            </button>
                        </div>
                    </div>
                    <div className={styles.list}>
                        {entries.length === 0 && (
                            <div className={styles.empty}>No errors logged</div>
                        )}
                        {entries.slice().reverse().map(entry => (
                            <div key={entry.id} className={styles.item}>
                                <div className={styles.itemLeft}>
                                    <span className={`${styles.dot} ${levelDot(entry.level)}`} />
                                    <div className={styles.message}>{entry.message}</div>
                                    <div className={styles.meta}>
                                        <Clock size={12} />
                                        {new Date(entry.ts).toLocaleTimeString()}
                                    </div>
                                </div>
                                <button
                                    className={styles.dismiss}
                                    onClick={() => handleDismiss(entry.id)}
                                    title="Dismiss"
                                    tabIndex={0}
                                >
                                    <X size={12} />
                                </button>
                            </div>
                        ))}
                    </div>
                </div>
            )}
        </div>
    );
}
