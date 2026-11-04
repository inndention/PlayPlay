/**
 * File Management Aggregator – torrent-based transfer synced via PHP tracker API
 *
 * Aggregates: local torrent status, swarm state from tracker, and transfer controls.
 * Everything syncs via the PHP tracker (register on seed, heartbeat for pending transfers).
 */

import React, { useState, useEffect, useCallback } from 'react';
import {
    Share2, Download, RefreshCw, Server, HardDrive, FileVideo, Wifi,
    AlertCircle, CheckCircle, Clock, Hash, Users, Database
} from 'lucide-react';
import styles from './SyncAggregatorPage.module.css';

const API_BASE = 'http://localhost:3000';

interface TorrentFile {
    name: string;
    progress: number;
    downloaded: number;
    length: number;
    downloadSpeed: number;
    uploadSpeed: number;
    timeRemaining: number;
    isPlayable: boolean;
    numPeers: number;
    numSeeders: number;
    numLeechers: number;
    state: string;
}

interface SyncStatus {
    downloadSpeed: number;
    uploadSpeed: number;
    progress: number;
    numPeers: number;
    files: TorrentFile[];
}

interface SwarmNode {
    id: string;
    hostname: string;
    status: string;
    media_count?: number;
    last_seen?: string;
}

interface SwarmFile {
    hash: string;
    filename: string;
    size_bytes: number;
    node_count?: number;
    available_on?: string;
}

interface SwarmState {
    swarm?: {
        nodes?: SwarmNode[];
        files?: SwarmFile[];
        node_count?: number;
        file_count?: number;
    };
    error?: string;
}

export function SyncAggregatorPage() {
    const [syncStatus, setSyncStatus] = useState<SyncStatus | null>(null);
    const [swarmState, setSwarmState] = useState<SwarmState | null>(null);
    const [loading, setLoading] = useState(true);
    const [error, setError] = useState<string | null>(null);
    const [magnetInput, setMagnetInput] = useState('');
    const [seeding, setSeeding] = useState(false);

    const fetchStatus = useCallback(async () => {
        try {
            const res = await fetch(`${API_BASE}/api/v1/status`);
            if (res.ok) {
                const data = await res.json();
                setSyncStatus(data.sync || null);
            }
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Sync API unreachable');
        }
    }, []);

    const fetchSwarm = useCallback(async () => {
        try {
            const res = await fetch(`${API_BASE}/api/v1/swarm/state`);
            if (res.ok) {
                const data = await res.json();
                setSwarmState(data);
            } else {
                setSwarmState({ error: 'Tracker not configured or unreachable' });
            }
        } catch (e) {
            setSwarmState({ error: e instanceof Error ? e.message : 'Swarm API error' });
        }
    }, []);

    useEffect(() => {
        setLoading(true);
        setError(null);
        Promise.all([fetchStatus(), fetchSwarm()]).finally(() => setLoading(false));
    }, [fetchStatus, fetchSwarm]);

    useEffect(() => {
        const t = setInterval(() => {
            fetchStatus();
        }, 2000);
        return () => clearInterval(t);
    }, [fetchStatus]);

    const handleAddMagnet = async () => {
        const magnet = magnetInput.trim();
        if (!magnet || !magnet.startsWith('magnet:')) {
            alert('Enter a valid magnet link');
            return;
        }
        try {
            const res = await fetch(`${API_BASE}/api/v1/sync/download`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ magnet }),
            });
            if (res.ok) {
                setMagnetInput('');
                fetchStatus();
            } else {
                const data = await res.json().catch(() => ({}));
                alert(data.error || 'Download failed');
            }
        } catch (e) {
            alert(e instanceof Error ? e.message : 'Request failed');
        }
    };

    const handleSeedFile = async () => {
        const api = (window as any).electronAPI;
        if (!api?.openFileDialog) {
            alert('File picker not available');
            return;
        }
        setSeeding(true);
        try {
            const result = await api.openFileDialog({ properties: ['openFile'] });
            const filePath = result?.filePaths?.[0];
            if (!filePath) {
                setSeeding(false);
                return;
            }
            const res = await fetch(`${API_BASE}/api/v1/sync/seed`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ filePath }),
            });
            if (res.ok) {
                const data = await res.json();
                alert(`Seeding started. Magnet: ${(data.magnet || '').slice(0, 60)}...`);
                fetchStatus();
                fetchSwarm();
            } else {
                const data = await res.json().catch(() => ({}));
                alert(data.error || 'Seed failed');
            }
        } catch (e) {
            alert(e instanceof Error ? e.message : 'Seed failed');
        } finally {
            setSeeding(false);
        }
    };

    const formatBytes = (n: number) => {
        if (n >= 1e9) return (n / 1e9).toFixed(1) + ' GB';
        if (n >= 1e6) return (n / 1e6).toFixed(1) + ' MB';
        if (n >= 1e3) return (n / 1e3).toFixed(1) + ' KB';
        return n + ' B';
    };

    const formatSpeed = (n: number) => (n > 0 ? formatBytes(n) + '/s' : '—');

    if (loading && !syncStatus && !swarmState?.swarm) {
        return (
            <div className={styles.container}>
                <div className={styles.header}>
                    <Database size={24} />
                    <h1>File & Sync</h1>
                </div>
                <div className={styles.loading}>Loading...</div>
            </div>
        );
    }

    const swarm = swarmState?.swarm;
    const nodes = swarm?.nodes || [];
    const files = swarm?.files || [];
    const torrentFiles = syncStatus?.files || [];

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.titleRow}>
                    <Database size={24} />
                    <h1>File Management & Sync</h1>
                </div>
                <p className={styles.subtitle}>
                    Transparent file sync across multiple PlayPlay sessions (torrent + PHP tracker). No login required in-app.
                </p>
                <div className={styles.actions}>
                    <button
                        type="button"
                        className={styles.refreshBtn}
                        onClick={() => { fetchStatus(); fetchSwarm(); }}
                    >
                        <RefreshCw size={16} /> Refresh
                    </button>
                    <button
                        type="button"
                        className={styles.seedBtn}
                        onClick={handleSeedFile}
                        disabled={seeding}
                    >
                        <Share2 size={16} /> {seeding ? 'Seeding…' : 'Seed file'}
                    </button>
                </div>
            </div>

            {error && (
                <div className={styles.error}>
                    <AlertCircle size={16} /> {error}
                </div>
            )}

            {swarmState?.error && (
                <div className={styles.warn}>
                    <AlertCircle size={16} /> Tracker: {swarmState.error}. Configure in Settings → Connections.
                </div>
            )}

            <section className={styles.section}>
                <h2><HardDrive size={18} /> Torrents</h2>
                <div className={styles.stats}>
                    <span>↓ {formatSpeed(syncStatus?.downloadSpeed ?? 0)}</span>
                    <span>↑ {formatSpeed(syncStatus?.uploadSpeed ?? 0)}</span>
                    <span>{syncStatus?.numPeers ?? 0} peers</span>
                </div>
                <ul className={styles.fileList}>
                    {torrentFiles.length === 0 && (
                        <li className={styles.empty}>No active torrents</li>
                    )}
                    {torrentFiles.map((f, i) => (
                        <li key={i} className={styles.fileItem}>
                            <FileVideo size={14} />
                            <span className={styles.fileName} title={f.name}>{f.name}</span>
                            <span className={styles.fileState}>{f.state}</span>
                            <span className={styles.fileProgress}>
                                {(f.progress * 100).toFixed(0)}%
                            </span>
                            <span className={styles.fileSpeed}>
                                ↓ {formatSpeed(f.downloadSpeed)} ↑ {formatSpeed(f.uploadSpeed)}
                            </span>
                        </li>
                    ))}
                </ul>
                <div className={styles.magnetRow}>
                    <input
                        type="text"
                        placeholder="magnet:?xt=..."
                        value={magnetInput}
                        onChange={(e) => setMagnetInput(e.target.value)}
                        className={styles.magnetInput}
                    />
                    <button
                        type="button"
                        className={styles.downloadBtn}
                        onClick={handleAddMagnet}
                    >
                        <Download size={16} /> Add download
                    </button>
                </div>
            </section>

            <section className={styles.section}>
                <h2><Server size={18} /> Swarm (PHP tracker)</h2>
                <div className={styles.stats}>
                    <span><Users size={14} /> {nodes.length} nodes</span>
                    <span><Hash size={14} /> {files.length} files</span>
                </div>
                {nodes.length > 0 && (
                    <ul className={styles.nodeList}>
                        {nodes.map((n) => (
                            <li key={n.id} className={styles.nodeItem}>
                                <Wifi size={14} />
                                <span>{n.hostname}</span>
                                <span className={styles.badge}>{n.status}</span>
                                {n.media_count != null && (
                                    <span className={styles.muted}>{n.media_count} files</span>
                                )}
                            </li>
                        ))}
                    </ul>
                )}
                {files.length > 0 && (
                    <ul className={styles.swarmFileList}>
                        {files.slice(0, 50).map((f) => (
                            <li key={f.hash} className={styles.swarmFileItem}>
                                <span className={styles.fileName} title={f.filename}>{f.filename}</span>
                                <span className={styles.muted}>{formatBytes(f.size_bytes)}</span>
                                <span className={styles.muted}>{f.node_count ?? 0} nodes</span>
                            </li>
                        ))}
                        {files.length > 50 && (
                            <li className={styles.muted}>… and {files.length - 50} more</li>
                        )}
                    </ul>
                )}
            </section>
        </div>
    );
}
