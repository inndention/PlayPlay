/**
 * GuestQueueManager - Manage guest queue with QR codes
 * 
 * Features:
 * - Add guests to queue
 * - Generate QR codes for each guest
 * - Show guest queue status
 * - Move guests up/down in queue
 */

import React, { useState, useEffect, useCallback } from 'react';
import styles from './GuestQueueManager.module.css';
import {
    Users, Plus, Trash2, ChevronUp, ChevronDown, QrCode, Eye, EyeOff,
    Radio, Clock, CheckCircle, AlertCircle, Copy, X
} from 'lucide-react';
import {
    getGuestQueueService,
    QueuedGuest,
} from '../lib/guestQueueService';
import { QRCodeOverlay } from './QRCodeOverlay';

interface GuestQueueManagerProps {
    onGuestSelect?: (guest: QueuedGuest) => void;
}

export const GuestQueueManager: React.FC<GuestQueueManagerProps> = ({
    onGuestSelect,
}) => {
    const [queue, setQueue] = useState<QueuedGuest[]>([]);
    const [showQR, setShowQR] = useState<string | null>(null);
    const [newGuestName, setNewGuestName] = useState('');
    const queueService = getGuestQueueService();

    // Load queue
    useEffect(() => {
        setQueue(queueService.getQueue());
    }, []);

    const handleAddGuest = useCallback(() => {
        if (!newGuestName.trim()) return;

        const guest = queueService.addGuest(newGuestName.trim());
        setQueue(queueService.getQueue());
        setNewGuestName('');
    }, [newGuestName]);

    const handleRemoveGuest = useCallback((guestId: string) => {
        queueService.removeGuest(guestId);
        setQueue(queueService.getQueue());
    }, []);

    const handleMoveUp = useCallback((guestId: string) => {
        queueService.moveUp(guestId);
        setQueue(queueService.getQueue());
    }, []);

    const handleMoveDown = useCallback((guestId: string) => {
        queueService.moveDown(guestId);
        setQueue(queueService.getQueue());
    }, []);

    const handleUpdateStatus = useCallback((guestId: string, status: QueuedGuest['status']) => {
        queueService.updateGuestStatus(guestId, status);
        setQueue(queueService.getQueue());
    }, []);

    const getStatusColor = (status: QueuedGuest['status']) => {
        switch (status) {
            case 'waiting':
                return 'var(--text-secondary)';
            case 'joined':
                return 'var(--accent)';
            case 'on-air':
                return 'var(--danger)';
            case 'completed':
                return 'var(--success)';
            default:
                return 'var(--text-muted)';
        }
    };

    const getStatusIcon = (status: QueuedGuest['status']) => {
        switch (status) {
            case 'waiting':
                return <Clock size={14} />;
            case 'joined':
                return <CheckCircle size={14} />;
            case 'on-air':
                return <Radio size={14} />;
            case 'completed':
                return <CheckCircle size={14} />;
            default:
                return null;
        }
    };

    return (
        <div className={styles.manager}>
            <div className={styles.header}>
                <div className={styles.titleSection}>
                    <Users size={20} />
                    <h2>Guest Queue</h2>
                    <span className={styles.queueCount}>{queue.length}</span>
                </div>
                <div className={styles.addGuest}>
                    <input
                        type="text"
                        placeholder="Guest name..."
                        value={newGuestName}
                        onChange={(e) => setNewGuestName(e.target.value)}
                        onKeyPress={(e) => {
                            if (e.key === 'Enter') {
                                handleAddGuest();
                            }
                        }}
                    />
                    <button onClick={handleAddGuest}>
                        <Plus size={16} />
                        Add
                    </button>
                </div>
            </div>

            <div className={styles.queueList}>
                {queue.length === 0 ? (
                    <div className={styles.emptyState}>
                        <Users size={48} />
                        <p>No guests in queue</p>
                    </div>
                ) : (
                    queue.map(guest => (
                        <div
                            key={guest.id}
                            className={`${styles.guestCard} ${guest.status === 'on-air' ? styles.onAir : ''}`}
                        >
                            <div className={styles.guestHeader}>
                                <div className={styles.guestInfo}>
                                    <span className={styles.position}>#{guest.position}</span>
                                    <span className={styles.guestName}>{guest.name}</span>
                                    <div
                                        className={styles.statusBadge}
                                        style={{ color: getStatusColor(guest.status) }}
                                    >
                                        {getStatusIcon(guest.status)}
                                        <span>{guest.status}</span>
                                    </div>
                                </div>
                                <div className={styles.guestActions}>
                                    <button
                                        onClick={() => setShowQR(showQR === guest.id ? null : guest.id)}
                                        title="Show QR Code"
                                    >
                                        <QrCode size={14} />
                                    </button>
                                    <button
                                        onClick={() => handleMoveUp(guest.id)}
                                        disabled={guest.position === 1}
                                        title="Move Up"
                                    >
                                        <ChevronUp size={14} />
                                    </button>
                                    <button
                                        onClick={() => handleMoveDown(guest.id)}
                                        disabled={guest.position === queue.length}
                                        title="Move Down"
                                    >
                                        <ChevronDown size={14} />
                                    </button>
                                    <button
                                        onClick={() => handleRemoveGuest(guest.id)}
                                        title="Remove"
                                    >
                                        <Trash2 size={14} />
                                    </button>
                                </div>
                            </div>

                            <div className={styles.guestControls}>
                                <button
                                    className={styles.statusButton}
                                    onClick={() => {
                                        const nextStatus: QueuedGuest['status'] =
                                            guest.status === 'waiting' ? 'joined' :
                                            guest.status === 'joined' ? 'on-air' :
                                            guest.status === 'on-air' ? 'completed' : 'waiting';
                                        handleUpdateStatus(guest.id, nextStatus);
                                    }}
                                >
                                    {guest.status === 'waiting' && 'Mark Joined'}
                                    {guest.status === 'joined' && 'Go Live'}
                                    {guest.status === 'on-air' && 'Complete'}
                                    {guest.status === 'completed' && 'Reset'}
                                </button>
                                <button
                                    className={styles.copyButton}
                                    onClick={() => {
                                        navigator.clipboard.writeText(guest.joinUrl);
                                    }}
                                >
                                    <Copy size={14} />
                                    Copy URL
                                </button>
                            </div>

                            {showQR === guest.id && (
                                <div className={styles.qrContainer}>
                                    <QRCodeOverlay
                                        url={guest.joinUrl}
                                        title={`${guest.name} - Join Link`}
                                        visible={true}
                                        position="center"
                                        size={200}
                                        onClose={() => setShowQR(null)}
                                    />
                                </div>
                            )}
                        </div>
                    ))
                )}
            </div>
        </div>
    );
};
