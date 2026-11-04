import React, { useState, useEffect, useCallback } from 'react';
import { X, CheckCircle, AlertTriangle, AlertCircle, RotateCcw } from 'lucide-react';
import { errorHandler } from '../lib/errorHandler';
import { ErrorEntry } from '../lib/types';
import styles from './ErrorTallyBox.module.css';

export function ErrorTallyBox() {
    const [errors, setErrors] = useState<ErrorEntry[]>([]);
    const [activeInstanceId, setActiveInstanceId] = useState<string | null>(null);
    const [isMinimized, setIsMinimized] = useState(false);

    useEffect(() => {
        const unsubscribe = errorHandler.onUpdate((allErrors) => {
            setErrors(allErrors);
        });

        // Load initial errors
        setErrors(errorHandler.getErrors());

        return unsubscribe;
    }, []);

    const instanceIds = errorHandler.getInstanceIds();
    const totalUnacknowledged = errorHandler.getUnacknowledgedCount();

    const currentErrors = activeInstanceId 
        ? errorHandler.getErrors(activeInstanceId)
        : errors;

    const handleAcknowledge = useCallback((errorId: string) => {
        errorHandler.acknowledgeError(errorId);
    }, []);

    const handleAcknowledgeAll = useCallback(() => {
        if (activeInstanceId) {
            errorHandler.acknowledgeAll(activeInstanceId);
        }
    }, [activeInstanceId]);

    const handleReset = useCallback(() => {
        if (activeInstanceId) {
            errorHandler.resetInstance(activeInstanceId);
        }
    }, [activeInstanceId]);

    if (totalUnacknowledged === 0 && errors.length === 0) {
        return null; // Don't show if no errors
    }

    const getSeverityIcon = (severity: string) => {
        switch (severity) {
            case 'critical':
                return <AlertCircle size={14} style={{ color: '#ef4444' }} />;
            case 'error':
                return <AlertTriangle size={14} style={{ color: '#f59e0b' }} />;
            case 'warning':
                return <AlertCircle size={14} style={{ color: '#3b82f6' }} />;
            default:
                return null;
        }
    };

    const getSeverityColor = (severity: string) => {
        switch (severity) {
            case 'critical':
                return '#ef4444';
            case 'error':
                return '#f59e0b';
            case 'warning':
                return '#3b82f6';
            default:
                return 'var(--text-secondary)';
        }
    };

    return (
        <div className={`${styles.errorBox} ${isMinimized ? styles.minimized : ''}`}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <AlertCircle size={16} />
                    <span className={styles.title}>Errors</span>
                    {totalUnacknowledged > 0 && (
                        <span className={styles.badge}>{totalUnacknowledged}</span>
                    )}
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.iconBtn}
                        onClick={() => setIsMinimized(!isMinimized)}
                        title={isMinimized ? 'Expand' : 'Minimize'}
                    >
                        <X size={14} />
                    </button>
                </div>
            </div>

            {!isMinimized && (
                <>
                    <div className={styles.tabs}>
                        <button
                            className={`${styles.tab} ${activeInstanceId === null ? styles.activeTab : ''}`}
                            onClick={() => setActiveInstanceId(null)}
                        >
                            All
                            {errorHandler.getUnacknowledgedCount() > 0 && (
                                <span className={styles.tabBadge}>{errorHandler.getUnacknowledgedCount()}</span>
                            )}
                        </button>
                        {instanceIds.map(instanceId => {
                            const count = errorHandler.getUnacknowledgedCount(instanceId);
                            return (
                                <button
                                    key={instanceId}
                                    className={`${styles.tab} ${activeInstanceId === instanceId ? styles.activeTab : ''}`}
                                    onClick={() => setActiveInstanceId(instanceId)}
                                >
                                    {instanceId}
                                    {count > 0 && (
                                        <span className={styles.tabBadge}>{count}</span>
                                    )}
                                </button>
                            );
                        })}
                    </div>

                    <div className={styles.content}>
                        {currentErrors.length === 0 ? (
                            <div className={styles.empty}>No errors</div>
                        ) : (
                            <>
                                {activeInstanceId && (
                                    <div className={styles.actions}>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={handleAcknowledgeAll}
                                        >
                                            <CheckCircle size={14} />
                                            Acknowledge All
                                        </button>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={handleReset}
                                        >
                                            <RotateCcw size={14} />
                                            Reset
                                        </button>
                                    </div>
                                )}
                                <div className={styles.errorList}>
                                    {currentErrors.map(error => (
                                        <div
                                            key={error.id}
                                            className={`${styles.errorItem} ${error.acknowledged ? styles.acknowledged : ''}`}
                                            style={{ borderLeftColor: getSeverityColor(error.severity) }}
                                        >
                                            <div className={styles.errorHeader}>
                                                <div className={styles.errorIcon}>
                                                    {getSeverityIcon(error.severity)}
                                                </div>
                                                <div className={styles.errorInfo}>
                                                    <div className={styles.errorMessage}>{error.message}</div>
                                                    <div className={styles.errorMeta}>
                                                        <span>{new Date(error.timestamp).toLocaleString()}</span>
                                                        <span>•</span>
                                                        <span>{error.source}</span>
                                                    </div>
                                                </div>
                                                {!error.acknowledged && (
                                                    <button
                                                        className={styles.ackBtn}
                                                        onClick={() => handleAcknowledge(error.id)}
                                                        title="Acknowledge"
                                                    >
                                                        <CheckCircle size={14} />
                                                    </button>
                                                )}
                                            </div>
                                            {error.details && (
                                                <div className={styles.errorDetails}>
                                                    {JSON.stringify(error.details, null, 2)}
                                                </div>
                                            )}
                                        </div>
                                    ))}
                                </div>
                            </>
                        )}
                    </div>
                </>
            )}
        </div>
    );
}




