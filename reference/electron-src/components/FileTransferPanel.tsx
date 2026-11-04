import React, { useState, useEffect } from 'react';
import { Download, Upload, CheckCircle, XCircle, Clock, AlertCircle, RefreshCw } from 'lucide-react';
import { fileTransferService, FileTransferStatus } from '../lib/fileTransferService';
import styles from './FileTransferPanel.module.css';

export function FileTransferPanel() {
    const [transfers, setTransfers] = useState<FileTransferStatus[]>([]);
    const [isExpanded, setIsExpanded] = useState(false);

    useEffect(() => {
        // Subscribe to transfer updates
        const unsubscribe = fileTransferService.onTransferUpdate((transfer) => {
            setTransfers(prev => {
                const index = prev.findIndex(t => t.fileId === transfer.fileId);
                if (index >= 0) {
                    const updated = [...prev];
                    updated[index] = transfer;
                    return updated;
                }
                return [...prev, transfer];
            });
        });

        // Load initial transfers
        const initialTransfers = fileTransferService.getAllTransfers();
        setTransfers(initialTransfers);

        return unsubscribe;
    }, []);

    const activeTransfers = transfers.filter(t => 
        t.status === 'checking' || t.status === 'downloading' || t.status === 'pending'
    );
    const completedTransfers = transfers.filter(t => t.status === 'complete');
    const errorTransfers = transfers.filter(t => t.status === 'error');

    const getStatusIcon = (status: FileTransferStatus['status']) => {
        switch (status) {
            case 'complete':
                return <CheckCircle size={16} className={styles.iconSuccess} />;
            case 'error':
                return <XCircle size={16} className={styles.iconError} />;
            case 'downloading':
            case 'checking':
                return <RefreshCw size={16} className={styles.iconSpinning} />;
            case 'pending':
                return <Clock size={16} className={styles.iconPending} />;
            default:
                return <AlertCircle size={16} />;
        }
    };

    const getStatusText = (transfer: FileTransferStatus) => {
        switch (transfer.status) {
            case 'complete':
                return 'Complete';
            case 'error':
                return transfer.error || 'Error';
            case 'downloading':
                return `Downloading ${Math.round(transfer.progress)}%`;
            case 'checking':
                return 'Checking...';
            case 'pending':
                return 'Queued';
            default:
                return 'Unknown';
        }
    };

    return (
        <div className={styles.container}>
            <div 
                className={styles.header}
                onClick={() => setIsExpanded(!isExpanded)}
            >
                <div className={styles.headerLeft}>
                    <Download size={16} />
                    <span>File Transfers</span>
                    {activeTransfers.length > 0 && (
                        <span className={styles.badge}>{activeTransfers.length}</span>
                    )}
                </div>
                <div className={styles.headerRight}>
                    {activeTransfers.length > 0 && (
                        <span className={styles.activeCount}>
                            {activeTransfers.length} active
                        </span>
                    )}
                </div>
            </div>

            {isExpanded && (
                <div className={styles.content}>
                    {transfers.length === 0 ? (
                        <div className={styles.emptyState}>
                            <Download size={32} className={styles.emptyIcon} />
                            <div>No file transfers</div>
                            <div className={styles.emptyHint}>
                                Drop files to initiate transfers
                            </div>
                        </div>
                    ) : (
                        <div className={styles.transferList}>
                            {/* Active transfers */}
                            {activeTransfers.length > 0 && (
                                <div className={styles.section}>
                                    <div className={styles.sectionHeader}>Active</div>
                                    {activeTransfers.map(transfer => (
                                        <div key={transfer.fileId} className={styles.transferItem}>
                                            <div className={styles.transferIcon}>
                                                {getStatusIcon(transfer.status)}
                                            </div>
                                            <div className={styles.transferInfo}>
                                                <div className={styles.transferName}>
                                                    {transfer.fileName}
                                                </div>
                                                <div className={styles.transferMeta}>
                                                    <span>{transfer.targetSiteId}</span>
                                                    <span className={styles.transferStatus}>
                                                        {getStatusText(transfer)}
                                                    </span>
                                                </div>
                                                {transfer.status === 'downloading' && (
                                                    <div className={styles.progressBar}>
                                                        <div 
                                                            className={styles.progressFill}
                                                            style={{ width: `${transfer.progress}%` }}
                                                        />
                                                    </div>
                                                )}
                                            </div>
                                        </div>
                                    ))}
                                </div>
                            )}

                            {/* Completed transfers */}
                            {completedTransfers.length > 0 && (
                                <div className={styles.section}>
                                    <div className={styles.sectionHeader}>
                                        Completed ({completedTransfers.length})
                                    </div>
                                    {completedTransfers.slice(-5).map(transfer => (
                                        <div key={transfer.fileId} className={styles.transferItem}>
                                            <div className={styles.transferIcon}>
                                                {getStatusIcon(transfer.status)}
                                            </div>
                                            <div className={styles.transferInfo}>
                                                <div className={styles.transferName}>
                                                    {transfer.fileName}
                                                </div>
                                                <div className={styles.transferMeta}>
                                                    <span>{transfer.targetSiteId}</span>
                                                </div>
                                            </div>
                                        </div>
                                    ))}
                                </div>
                            )}

                            {/* Error transfers */}
                            {errorTransfers.length > 0 && (
                                <div className={styles.section}>
                                    <div className={styles.sectionHeader}>
                                        Errors ({errorTransfers.length})
                                    </div>
                                    {errorTransfers.slice(-3).map(transfer => (
                                        <div key={transfer.fileId} className={styles.transferItem}>
                                            <div className={styles.transferIcon}>
                                                {getStatusIcon(transfer.status)}
                                            </div>
                                            <div className={styles.transferInfo}>
                                                <div className={styles.transferName}>
                                                    {transfer.fileName}
                                                </div>
                                                <div className={styles.transferMeta}>
                                                    <span className={styles.transferError}>
                                                        {transfer.error || 'Transfer failed'}
                                                    </span>
                                                </div>
                                            </div>
                                        </div>
                                    ))}
                                </div>
                            )}
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}




