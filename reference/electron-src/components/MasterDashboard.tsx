import React, { useEffect, useState, useCallback } from 'react';
import { apiClient } from '../lib/apiClient';
import { useToast } from './Toast';
import styles from './MasterDashboard.module.css';

interface SyncFile {
    name: string;
    progress: number;
    downloadSpeed: number;
    uploadSpeed: number;
    timeRemaining: number;
    timeToPlayable: number;
    isPlayable: boolean;
    state: string;
}

interface Device {
    id: string;
    ip: string;
    port: number;
    type: string;
    lastSeen: number;
}

interface PeerData {
    id: string;
    hostname: string;
    lastSeen: number;
    downloads: Record<string, number>;
    screenshot?: string;
    playing?: string;
    device?: Device;
    type?: string;
    isPlayout?: boolean;
}

interface SwarmFile {
    hash: string;
    status: string;
    updated_at: string;
    magnet?: string;
    name?: string;
}

interface TempBucketItem {
    id: string;
    type: 'file' | 'command';
    label: string;
    data: any;
}

type LayoutMode = 'grid' | 'focus' | 'list' | 'broadcast';

export const MasterDashboard: React.FC = () => {
    const { showToast } = useToast();
    const [status, setStatus] = useState<any>(null);
    const [devices, setDevices] = useState<Device[]>([]);
    const [peers, setPeers] = useState<Record<string, PeerData>>({});
    const [swarm, setSwarm] = useState<Record<string, SwarmFile[]>>({});
    const [loading, setLoading] = useState(true);
    const [draggedFile, setDraggedFile] = useState<string | null>(null);
    const [isDraggingOver, setIsDraggingOver] = useState(false);

    // New state for enhanced features
    const [activeTab, setActiveTab] = useState<string>('overview');
    const [layoutMode, setLayoutMode] = useState<LayoutMode>('grid');
    const [tempBucket, setTempBucket] = useState<TempBucketItem[]>([]);
    const [focusedPeerId, setFocusedPeerId] = useState<string | null>(null);

    // Get active playouts (peers that are currently playing something)
    const activePlayouts = Object.values(peers).filter(p => p.playing || p.isPlayout);

    useEffect(() => {
        let lastDataHash = '';
        
        const fetchData = async () => {
            try {
                const [statusData, devicesData, meshData, swarmData] = await Promise.all([
                    apiClient.getStatus(),
                    apiClient.getDevices(),
                    apiClient.getMeshState(),
                    apiClient.getSwarmState()
                ]);
                
                // Create hash of data to detect actual changes
                const dataHash = JSON.stringify({
                    status: statusData,
                    devicesCount: devicesData.devices?.length || 0,
                    peersCount: Object.keys(meshData.peers || {}).length,
                    swarmCount: Object.keys(swarmData.swarm || {}).length,
                });
                
                // Only update state if data actually changed
                if (dataHash !== lastDataHash) {
                    lastDataHash = dataHash;
                    setStatus(statusData);
                    setDevices(devicesData.devices || []);
                    setPeers(meshData.peers || {});
                    setSwarm(swarmData.swarm || {});
                }
            } catch (err) {
                console.error('Failed to fetch dashboard data:', err);
            } finally {
                setLoading(false);
            }
        };

        fetchData();
        const interval = setInterval(fetchData, 2000);
        return () => clearInterval(interval);
    }, []);

    // Temp Bucket Functions
    const addToTempBucket = (item: TempBucketItem) => {
        setTempBucket(prev => {
            if (prev.find(i => i.id === item.id)) return prev;
            return [...prev, item];
        });
        showToast(`Added "${item.label}" to bucket`, 'success');
    };

    const removeFromTempBucket = (id: string) => {
        setTempBucket(prev => prev.filter(i => i.id !== id));
    };

    const handleBucketDrop = (e: React.DragEvent) => {
        e.preventDefault();
        const fileHash = e.dataTransfer.getData('application/playplay-file');
        if (fileHash) {
            // Find file info
            for (const [nodeId, files] of Object.entries(swarm)) {
                const file = files.find(f => f.hash === fileHash);
                if (file) {
                    addToTempBucket({
                        id: fileHash,
                        type: 'file',
                        label: file.name || fileHash.substring(0, 12) + '...',
                        data: { ...file, sourceNode: nodeId }
                    });
                    return;
                }
            }
        }
    };

    const handleDragStart = (e: React.DragEvent, fileHash: string) => {
        setDraggedFile(fileHash);
        e.dataTransfer.setData('application/playplay-file', fileHash);
        e.dataTransfer.effectAllowed = 'copy';
    };

    const handleDragOver = (e: React.DragEvent) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
    };

    const handleDragEnter = () => setIsDraggingOver(true);
    const handleDragLeave = () => setIsDraggingOver(false);

    const handleDrop = async (e: React.DragEvent, targetNodeId: string) => {
        e.preventDefault();
        setIsDraggingOver(false);
        const fileHash = e.dataTransfer.getData('application/playplay-file');

        if (fileHash) {
            try {
                let magnetUri = '';
                for (const nodeFiles of Object.values(swarm)) {
                    const file = nodeFiles.find(f => f.hash === fileHash);
                    if (file?.magnet) {
                        magnetUri = file.magnet;
                        break;
                    }
                }

                await apiClient.queueTransfer(targetNodeId, fileHash, magnetUri);
                showToast(`Transfer queued to ${targetNodeId.substring(0, 12)}...`, 'success');
            } catch (err) {
                showToast('Failed to queue transfer', 'error');
            }
        }
        setDraggedFile(null);
    };

    const handleGlobalDrop = async (e: React.DragEvent) => {
        e.preventDefault();
        setIsDraggingOver(false);
        const files = Array.from(e.dataTransfer.files);

        if (files.length > 0) {
            for (const file of files) {
                try {
                    // @ts-ignore - Electron provides path property
                    const filePath = file.path;
                    if (filePath) {
                        await apiClient.seedFile(filePath);
                    }
                } catch (err) {
                    console.error('Failed to seed file:', err);
                }
            }
            showToast(`${files.length} file(s) added to library`, 'success');
        }
    };

    const handleYouTubeUpload = async (file: SwarmFile) => {
        const { safePrompt } = await import('../lib/promptPolyfill');
        const title = await safePrompt('Video Title:', file.hash.substring(0, 12));
        if (!title) return;

        const description = await safePrompt('Description:', 'Uploaded via PlayPlay');

        try {
            const findRes = await apiClient.findFile(file.hash);
            if (!findRes?.found) {
                showToast('File not available locally', 'warning');
                return;
            }

            const res = await apiClient.uploadToYouTube(findRes.filePath, title, description || '');
            if (res.error) {
                if (res.error.includes('credentials')) {
                    showToast('YouTube authentication required', 'warning');
                    const auth = await apiClient.getYouTubeAuthUrl();
                    if (auth.url) {
                        window.open(auth.url, '_blank');
                        const code = await safePrompt('Paste the auth code from Google:');
                        if (code) {
                            await apiClient.authenticateYouTube(code);
                            showToast('Authenticated! Try upload again.', 'success');
                        }
                    }
                } else {
                    showToast(`Upload failed: ${res.error}`, 'error');
                }
            } else {
                showToast('Upload started!', 'success');
            }
        } catch {
            showToast('Error initiating upload', 'error');
        }
    };

    const formatSpeed = (bytes: number) => {
        if (!bytes || isNaN(bytes)) return '0 B/s';
        const mb = bytes / 1024 / 1024;
        return mb >= 1 ? `${mb.toFixed(1)} MB/s` : `${(bytes / 1024).toFixed(0)} KB/s`;
    };

    const formatTime = (ms: number) => {
        if (!ms || ms === Infinity) return '--';
        const s = Math.floor(ms / 1000);
        if (s < 60) return `${s}s`;
        return `${Math.floor(s / 60)}m ${Math.round(s % 60)}s`;
    };

    const getDeviceIcon = (type: string) => {
        switch (type?.toLowerCase()) {
            case 'obs': return '🎬';
            case 'atem': return '🎛️';
            case 'ndi': return '📡';
            default: return '📦';
        }
    };

    const getLayoutColumns = () => {
        switch (layoutMode) {
            case 'grid': return 'repeat(auto-fill, minmax(380px, 1fr))';
            case 'focus': return '1fr';
            case 'list': return '1fr';
            case 'broadcast': return 'repeat(2, 1fr)';
            default: return 'repeat(auto-fill, minmax(380px, 1fr))';
        }
    };

    if (loading) {
        return (
            <div className={styles.dashboard}>
                <div className={styles.emptyState}>Loading Master Control...</div>
            </div>
        );
    }

    const focusedPeer = focusedPeerId ? peers[focusedPeerId] : null;

    return (
        <div
            className={`${styles.dashboard} ${styles.dropZone} ${isDraggingOver ? styles.dragging : ''}`}
            onDragOver={handleDragOver}
            onDragEnter={handleDragEnter}
            onDragLeave={handleDragLeave}
            onDrop={handleGlobalDrop}
        >
            {/* Master Tab Bar */}
            <div className={styles.masterTabBar}>
                <div className={styles.tabGroup}>
                    <button
                        className={`${styles.tab} ${activeTab === 'overview' ? styles.tabActive : ''}`}
                        onClick={() => setActiveTab('overview')}
                    >
                        📊 Overview
                    </button>
                    {activePlayouts.map(peer => (
                        <button
                            key={peer.id}
                            className={`${styles.tab} ${activeTab === peer.id ? styles.tabActive : ''} ${peer.playing ? styles.tabPlaying : ''}`}
                            onClick={() => { setActiveTab(peer.id); setFocusedPeerId(peer.id); }}
                        >
                            <span className={peer.playing ? styles.liveIndicator : styles.idleIndicator}></span>
                            {peer.hostname}
                        </button>
                    ))}
                </div>

                {/* Layout Switcher */}
                <div className={styles.layoutSwitcher}>
                    <button
                        className={`${styles.layoutBtn} ${layoutMode === 'grid' ? styles.layoutActive : ''}`}
                        onClick={() => setLayoutMode('grid')}
                        title="Grid View"
                    >⊞</button>
                    <button
                        className={`${styles.layoutBtn} ${layoutMode === 'focus' ? styles.layoutActive : ''}`}
                        onClick={() => setLayoutMode('focus')}
                        title="Focus View"
                    >◻</button>
                    <button
                        className={`${styles.layoutBtn} ${layoutMode === 'list' ? styles.layoutActive : ''}`}
                        onClick={() => setLayoutMode('list')}
                        title="List View"
                    >≡</button>
                    <button
                        className={`${styles.layoutBtn} ${layoutMode === 'broadcast' ? styles.layoutActive : ''}`}
                        onClick={() => setLayoutMode('broadcast')}
                        title="Broadcast View"
                    >⧉</button>
                </div>
            </div>

            {/* Header */}
            <header className={styles.header}>
                <div>
                    <h1 className={styles.headerTitle}>Master Control</h1>
                    <p className={styles.headerSubtitle}>
                        System Status: <span className={status?.status === 'online' ? styles.statusOnline : styles.statusOffline}>
                            {status?.status || 'Offline'}
                        </span>
                        {' • '}
                        <span>{Object.keys(peers).length} nodes</span>
                        {' • '}
                        <span>{activePlayouts.length} active playouts</span>
                    </p>
                </div>
                <div className={styles.headerMeta}>
                    <div>Version: {status?.version || '-'}</div>
                    <div>Mode: {status?.mode || '-'}</div>
                </div>
            </header>

            {/* Main Content Area */}
            <div className={styles.mainContent}>
                {/* Primary Panel */}
                <div className={styles.primaryPanel}>
                    {activeTab === 'overview' ? (
                        <>
                            {/* Cards Grid */}
                            <div className={styles.cardsGrid} style={{ gridTemplateColumns: getLayoutColumns() }}>
                                {/* Sync Health Card */}
                                <div className={styles.card}>
                                    <h2 className={styles.cardTitle}>
                                        <span className={`${styles.cardTitleIcon} ${styles.iconBlue}`}></span>
                                        Sync Health
                                    </h2>

                                    <div className={styles.statsRow}>
                                        <span className={styles.statsLabel}>Download</span>
                                        <span className={styles.statsValue}>{formatSpeed(status?.sync?.downloadSpeed)}</span>
                                    </div>
                                    <div className={styles.statsRow}>
                                        <span className={styles.statsLabel}>Upload</span>
                                        <span className={styles.statsValue}>{formatSpeed(status?.sync?.uploadSpeed)}</span>
                                    </div>

                                    <div className={styles.progressContainer}>
                                        {(!status?.sync?.files || status.sync.files.length === 0) ? (
                                            <div className={styles.emptyState} style={{ padding: '16px' }}>No active transfers</div>
                                        ) : (
                                            status.sync.files.map((f: SyncFile, i: number) => (
                                                <div key={i} className={styles.progressItem}>
                                                    <div className={styles.progressHeader}>
                                                        <span className={styles.progressName} title={f.name}>{f.name}</span>
                                                        <span className={styles.progressPercent}>{(f.progress * 100).toFixed(1)}%</span>
                                                    </div>
                                                    <div className={styles.progressTrack}>
                                                        <div
                                                            className={`${styles.progressFill} ${f.isPlayable ? styles.progressFillGreen : styles.progressFillBlue}`}
                                                            style={{ width: `${f.progress * 100}%` }}
                                                        />
                                                    </div>
                                                    <div className={styles.progressMeta}>
                                                        <span className={f.isPlayable ? styles.playableTag : styles.pendingTag}>
                                                            {f.isPlayable ? '● PLAYABLE' : `Playable in ${formatTime(f.timeToPlayable)}`}
                                                        </span>
                                                        <span>ETA: {formatTime(f.timeRemaining)}</span>
                                                    </div>
                                                </div>
                                            ))
                                        )}
                                    </div>
                                </div>

                                {/* Devices Card */}
                                <div className={styles.card}>
                                    <h2 className={styles.cardTitle}>
                                        <span className={`${styles.cardTitleIcon} ${styles.iconPurple}`}></span>
                                        Discovered Devices
                                    </h2>

                                    {devices.length === 0 ? (
                                        <div className={styles.emptyState}>Scanning for devices...</div>
                                    ) : (
                                        <div className={styles.devicesGrid}>
                                            {devices.map((device) => (
                                                <div key={device.id} className={styles.deviceCard}>
                                                    <div className={`${styles.deviceIcon}`}>
                                                        {getDeviceIcon(device.type)}
                                                    </div>
                                                    <div className={styles.deviceInfo}>
                                                        <div className={styles.deviceName}>{device.type}</div>
                                                        <div className={styles.deviceIp}>{device.ip}:{device.port}</div>
                                                    </div>
                                                </div>
                                            ))}
                                        </div>
                                    )}
                                </div>

                                {/* Remote Nodes */}
                                {Object.values(peers).map((peerData) => (
                                    <div
                                        key={peerData.id}
                                        className={`${styles.nodeCard} ${draggedFile ? styles.dropTarget : ''}`}
                                        onDragOver={handleDragOver}
                                        onDrop={(e) => handleDrop(e, peerData.id)}
                                        onClick={() => { setActiveTab(peerData.id); setFocusedPeerId(peerData.id); }}
                                    >
                                        <h2 className={styles.nodeHostname}>
                                            {peerData.playing && <span className={styles.liveIndicator}></span>}
                                            {peerData.hostname}
                                        </h2>
                                        <div className={styles.nodeId}>{peerData.id}</div>

                                        {/* Confidence Monitor */}
                                        <div className={styles.confidenceMonitor}>
                                            <div className={styles.confidenceLabel}>Confidence Monitor</div>
                                            <div className={styles.confidencePreview}>
                                                {peerData.screenshot ? (
                                                    <img src={peerData.screenshot} alt="Live" style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
                                                ) : (
                                                    <span className={styles.noSignal}>No Signal</span>
                                                )}
                                                <div className={styles.confidenceOverlay}>
                                                    {peerData.playing ? `▶ ${peerData.playing}` : '⏹ Stopped'}
                                                </div>
                                            </div>
                                        </div>

                                        <div className={styles.statsRow} style={{ marginTop: 16 }}>
                                            <span className={styles.statsLabel}>Last seen</span>
                                            <span className={styles.statsValue}>{Math.floor((Date.now() - peerData.lastSeen) / 1000)}s ago</span>
                                        </div>
                                    </div>
                                ))}
                            </div>

                            {/* Swarm Inventory */}
                            <section className={styles.inventorySection}>
                                <h2 className={styles.inventoryTitle}>
                                    <span className={`${styles.cardTitleIcon} ${styles.iconOrange}`}></span>
                                    Swarm Inventory
                                </h2>

                                <div className={styles.card}>
                                    <table className={styles.inventoryTable}>
                                        <thead>
                                            <tr>
                                                <th>Node</th>
                                                <th>File Hash</th>
                                                <th>Status</th>
                                                <th>Updated</th>
                                                <th>Actions</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {Object.entries(swarm).length === 0 ? (
                                                <tr>
                                                    <td colSpan={5} className={styles.emptyState}>
                                                        No inventory data available
                                                    </td>
                                                </tr>
                                            ) : (
                                                Object.entries(swarm).flatMap(([nodeId, files]) =>
                                                    files.map((file, i) => (
                                                        <tr
                                                            key={`${nodeId}-${file.hash}`}
                                                            draggable
                                                            onDragStart={(e) => handleDragStart(e, file.hash)}
                                                        >
                                                            <td style={{ color: 'rgba(255,255,255,0.6)', fontFamily: 'monospace', fontSize: 11 }}>
                                                                {i === 0 ? nodeId.substring(0, 12) + '...' : ''}
                                                            </td>
                                                            <td className={styles.hashCell}>{file.hash.substring(0, 16)}...</td>
                                                            <td>
                                                                <span className={`${styles.statusBadge} ${file.status === 'ready' ? styles.statusReady : styles.statusPending}`}>
                                                                    {file.status}
                                                                </span>
                                                            </td>
                                                            <td style={{ color: 'rgba(255,255,255,0.4)', fontSize: 12 }}>{file.updated_at}</td>
                                                            <td>
                                                                <button
                                                                    className={`${styles.actionButton} ${styles.actionSecondary}`}
                                                                    style={{ padding: '6px 12px', fontSize: 11 }}
                                                                    onClick={() => handleYouTubeUpload(file)}
                                                                >
                                                                    📤 YouTube
                                                                </button>
                                                            </td>
                                                        </tr>
                                                    ))
                                                )
                                            )}
                                        </tbody>
                                    </table>
                                </div>
                            </section>
                        </>
                    ) : focusedPeer ? (
                        /* Focused Peer View */
                        <div className={styles.focusedPeerView}>
                            <div className={styles.focusedHeader}>
                                <h2>{focusedPeer.hostname}</h2>
                                <span className={focusedPeer.playing ? styles.liveTag : styles.offlineTag}>
                                    {focusedPeer.playing ? '● LIVE' : 'OFFLINE'}
                                </span>
                            </div>

                            <div className={styles.focusedMonitor}>
                                {focusedPeer.screenshot ? (
                                    <img src={focusedPeer.screenshot} alt="Live" />
                                ) : (
                                    <div className={styles.noSignalLarge}>No Signal</div>
                                )}
                                <div className={styles.focusedOverlay}>
                                    {focusedPeer.playing ? `▶ Now Playing: ${focusedPeer.playing}` : '⏹ Stopped'}
                                </div>
                            </div>

                            <div className={styles.focusedControls}>
                                <button className={`${styles.actionButton} ${styles.actionPrimary}`}>
                                    ▶ Play
                                </button>
                                <button className={`${styles.actionButton} ${styles.actionSecondary}`}>
                                    ⏸ Pause
                                </button>
                                <button className={`${styles.actionButton} ${styles.actionSecondary}`}>
                                    ⏹ Stop
                                </button>
                                <button className={`${styles.actionButton} ${styles.actionSecondary}`}>
                                    ⏭ Next
                                </button>
                            </div>
                        </div>
                    ) : null}
                </div>

                {/* Temp Bucket Sidebar */}
                <div
                    className={styles.tempBucket}
                    onDragOver={handleDragOver}
                    onDrop={handleBucketDrop}
                >
                    <h3 className={styles.bucketTitle}>
                        🪣 Quick Bucket
                    </h3>
                    <p className={styles.bucketHint}>Drop files or commands here for quick access</p>

                    {tempBucket.length === 0 ? (
                        <div className={styles.bucketEmpty}>
                            <span>Drop items here</span>
                        </div>
                    ) : (
                        <div className={styles.bucketItems}>
                            {tempBucket.map(item => (
                                <div
                                    key={item.id}
                                    className={styles.bucketItem}
                                    draggable
                                    onDragStart={(e) => {
                                        if (item.type === 'file') {
                                            e.dataTransfer.setData('application/playplay-file', item.data.hash);
                                        }
                                    }}
                                >
                                    <span className={styles.bucketItemIcon}>
                                        {item.type === 'file' ? '📄' : '⚡'}
                                    </span>
                                    <span className={styles.bucketItemLabel}>{item.label}</span>
                                    <button
                                        className={styles.bucketItemRemove}
                                        onClick={() => removeFromTempBucket(item.id)}
                                    >
                                        ×
                                    </button>
                                </div>
                            ))}
                        </div>
                    )}
                </div>
            </div>
        </div>
    );
};
