/**
 * P2PFileSync - Distributed File Synchronization with Torrent Protocol
 * 
 * Features:
 * - Download scheduling and queues
 * - Intelligent file handling with prioritization
 * - Torrent/BitTorrent protocol auto-sharing
 * - Node mode for distributed peer-to-peer sharing
 * - Real-time sync status and progress
 * - Peer discovery and management
 * - Bandwidth throttling and scheduling
 * - File integrity verification (hash checking)
 * - Automatic seeding for content availability
 * - Multi-node cluster management
 */

import React, { useState, useEffect, useCallback, useMemo } from 'react';
import styles from './P2PFileSync.module.css';
import {
    Share2, Download, Upload, Wifi, WifiOff, Server, HardDrive, Folder,
    File, FileVideo, FileAudio, FileImage, Play, Pause, Square, X, Check,
    Clock, Calendar, RefreshCw, Settings, AlertTriangle, CheckCircle,
    XCircle, ArrowDown, ArrowUp, ArrowRightLeft, Users, Globe, Zap,
    Plus, Minus, Trash2, Eye, Edit2, Copy, Link, Unlink, Search, Filter,
    ChevronDown, ChevronRight, MoreHorizontal, Activity, Database, Cpu,
    Network, Radio, Shield, Lock, Unlock, Hash, BarChart2
} from 'lucide-react';

// Types
type TransferStatus = 'queued' | 'downloading' | 'uploading' | 'paused' | 'completed' | 'error' | 'seeding';
type NodeStatus = 'online' | 'offline' | 'syncing' | 'error';
type Priority = 'low' | 'normal' | 'high' | 'urgent';

interface FileTransfer {
    id: string;
    name: string;
    path: string;
    size: number;
    type: 'video' | 'audio' | 'image' | 'document' | 'other';
    status: TransferStatus;
    progress: number;
    downloadSpeed: number;
    uploadSpeed: number;
    peers: number;
    seeders: number;
    leechers: number;
    eta?: number; // seconds
    priority: Priority;
    hash: string;
    addedAt: string;
    completedAt?: string;
    error?: string;
    scheduledStart?: string;
    tags: string[];
}

interface PeerNode {
    id: string;
    name: string;
    address: string;
    port: number;
    status: NodeStatus;
    lastSeen: string;
    uploadSpeed: number;
    downloadSpeed: number;
    sharedFiles: number;
    totalStorage: number;
    usedStorage: number;
    isLocal: boolean;
    isTrusted: boolean;
    version: string;
}

interface SyncFolder {
    id: string;
    path: string;
    name: string;
    totalFiles: number;
    syncedFiles: number;
    totalSize: number;
    syncedSize: number;
    status: 'synced' | 'syncing' | 'pending' | 'error';
    lastSync: string;
    autoSync: boolean;
    bidirectional: boolean;
    includedExtensions: string[];
    excludedPatterns: string[];
}

interface BandwidthSchedule {
    id: string;
    name: string;
    downloadLimit: number; // KB/s, 0 = unlimited
    uploadLimit: number;
    startTime: string;
    endTime: string;
    days: number[]; // 0-6 (Sunday-Saturday)
    active: boolean;
}

interface P2PFileSyncProps {
    onStartTransfer?: (transfer: FileTransfer) => void;
    onStopTransfer?: (transferId: string) => void;
    onNodeConnect?: (node: PeerNode) => void;
    onNodeDisconnect?: (nodeId: string) => void;
}

export const P2PFileSync: React.FC<P2PFileSyncProps> = ({
    onStartTransfer,
    onStopTransfer,
    onNodeConnect,
    onNodeDisconnect
}) => {
    // State
    const [activeTab, setActiveTab] = useState<'transfers' | 'nodes' | 'folders' | 'settings'>('transfers');
    const [transfers, setTransfers] = useState<FileTransfer[]>([]);
    const [nodes, setNodes] = useState<PeerNode[]>([]);
    const [syncFolders, setSyncFolders] = useState<SyncFolder[]>([]);
    const [selectedTransfers, setSelectedTransfers] = useState<Set<string>>(new Set());
    const [filterStatus, setFilterStatus] = useState<string>('all');
    const [searchQuery, setSearchQuery] = useState('');
    const [showAddModal, setShowAddModal] = useState(false);
    const [showSettingsModal, setShowSettingsModal] = useState(false);
    const [newTransferUrl, setNewTransferUrl] = useState('');
    const [globalDownloadLimit, setGlobalDownloadLimit] = useState(0);
    const [globalUploadLimit, setGlobalUploadLimit] = useState(0);
    const [maxConnections, setMaxConnections] = useState(100);
    const [nodeMode, setNodeMode] = useState(true);
    const [autoSeed, setAutoSeed] = useState(true);
    const [expandedTransfer, setExpandedTransfer] = useState<string | null>(null);

    // Calculate statistics
    const stats = useMemo(() => {
        const activeDownloads = transfers.filter(t => t.status === 'downloading');
        const activeUploads = transfers.filter(t => t.status === 'uploading' || t.status === 'seeding');
        const totalDownloadSpeed = activeDownloads.reduce((sum, t) => sum + t.downloadSpeed, 0);
        const totalUploadSpeed = activeUploads.reduce((sum, t) => sum + t.uploadSpeed, 0);
        const totalPeers = transfers.reduce((sum, t) => sum + t.peers, 0);
        const onlineNodes = nodes.filter(n => n.status === 'online' || n.status === 'syncing').length;
        
        return {
            activeDownloads: activeDownloads.length,
            activeUploads: activeUploads.length,
            totalDownloadSpeed,
            totalUploadSpeed,
            totalPeers,
            onlineNodes,
            totalNodes: nodes.length,
            queuedTransfers: transfers.filter(t => t.status === 'queued').length,
            completedTransfers: transfers.filter(t => t.status === 'completed' || t.status === 'seeding').length
        };
    }, [transfers, nodes]);

    // Format bytes
    const formatBytes = (bytes: number, decimals = 2) => {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const dm = decimals < 0 ? 0 : decimals;
        const sizes = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
    };

    // Format speed
    const formatSpeed = (bytesPerSecond: number) => {
        return formatBytes(bytesPerSecond) + '/s';
    };

    // Format time
    const formatETA = (seconds?: number) => {
        if (!seconds || seconds <= 0) return '--';
        const totalSec = Math.round(seconds);
        const h = Math.floor(totalSec / 3600);
        const m = Math.floor((totalSec % 3600) / 60);
        const s = totalSec % 60;
        if (h > 0) return `${h}h ${m}m`;
        if (m > 0) return `${m}m ${s}s`;
        return `${s}s`;
    };

    // Get file icon
    const getFileIcon = (type: string) => {
        switch (type) {
            case 'video': return <FileVideo size={16} />;
            case 'audio': return <FileAudio size={16} />;
            case 'image': return <FileImage size={16} />;
            default: return <File size={16} />;
        }
    };

    // Get status icon
    const getStatusIcon = (status: TransferStatus) => {
        switch (status) {
            case 'downloading': return <ArrowDown size={14} className={styles.downloadIcon} />;
            case 'uploading':
            case 'seeding': return <ArrowUp size={14} className={styles.uploadIcon} />;
            case 'completed': return <CheckCircle size={14} className={styles.successIcon} />;
            case 'paused': return <Pause size={14} className={styles.pausedIcon} />;
            case 'queued': return <Clock size={14} className={styles.queuedIcon} />;
            case 'error': return <XCircle size={14} className={styles.errorIcon} />;
            default: return <Activity size={14} />;
        }
    };

    // Get node status icon
    const getNodeStatusIcon = (status: NodeStatus) => {
        switch (status) {
            case 'online': return <CheckCircle size={14} className={styles.successIcon} />;
            case 'syncing': return <RefreshCw size={14} className={`${styles.syncingIcon} ${styles.spinning}`} />;
            case 'offline': return <XCircle size={14} className={styles.offlineIcon} />;
            case 'error': return <AlertTriangle size={14} className={styles.errorIcon} />;
            default: return <Activity size={14} />;
        }
    };

    // Toggle transfer selection
    const toggleTransferSelection = (id: string) => {
        setSelectedTransfers(prev => {
            const next = new Set(prev);
            if (next.has(id)) {
                next.delete(id);
            } else {
                next.add(id);
            }
            return next;
        });
    };

    // Pause/Resume transfer
    const toggleTransferPause = (transfer: FileTransfer) => {
        setTransfers(prev => prev.map(t => {
            if (t.id !== transfer.id) return t;
            if (t.status === 'paused') {
                return { ...t, status: 'downloading' as TransferStatus };
            } else if (t.status === 'downloading' || t.status === 'uploading') {
                return { ...t, status: 'paused' as TransferStatus };
            }
            return t;
        }));
    };

    // Remove transfer
    const removeTransfer = (transferId: string) => {
        setTransfers(prev => prev.filter(t => t.id !== transferId));
        onStopTransfer?.(transferId);
    };

    // Add new transfer
    const addTransfer = () => {
        if (!newTransferUrl.trim()) return;
        
        const newTransfer: FileTransfer = {
            id: `t_${Date.now()}`,
            name: newTransferUrl.split('/').pop() || 'Unknown File',
            path: '/media/downloads/',
            size: 0,
            type: 'other',
            status: 'queued',
            progress: 0,
            downloadSpeed: 0,
            uploadSpeed: 0,
            peers: 0,
            seeders: 0,
            leechers: 0,
            priority: 'normal',
            hash: Math.random().toString(36).substring(2),
            addedAt: new Date().toISOString(),
            tags: []
        };
        
        setTransfers(prev => [...prev, newTransfer]);
        setNewTransferUrl('');
        setShowAddModal(false);
        onStartTransfer?.(newTransfer);
    };

    // Filter transfers
    const filteredTransfers = useMemo(() => {
        return transfers.filter(t => {
            if (filterStatus !== 'all' && t.status !== filterStatus) return false;
            if (searchQuery && !t.name.toLowerCase().includes(searchQuery.toLowerCase())) return false;
            return true;
        });
    }, [transfers, filterStatus, searchQuery]);

    return (
        <div className={styles.syncPanel}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <Share2 size={24} />
                    <h2>P2P File Sync</h2>
                    <span className={`${styles.nodeStatus} ${nodeMode ? styles.active : ''}`}>
                        <Radio size={12} />
                        {nodeMode ? 'Node Mode Active' : 'Node Mode Off'}
                    </span>
                </div>
                
                <div className={styles.headerStats}>
                    <div className={styles.speedStat}>
                        <ArrowDown size={14} className={styles.downloadIcon} />
                        <span>{formatSpeed(stats.totalDownloadSpeed)}</span>
                    </div>
                    <div className={styles.speedStat}>
                        <ArrowUp size={14} className={styles.uploadIcon} />
                        <span>{formatSpeed(stats.totalUploadSpeed)}</span>
                    </div>
                    <div className={styles.peerStat}>
                        <Users size={14} />
                        <span>{stats.totalPeers} peers</span>
                    </div>
                    <div className={styles.nodeStat}>
                        <Server size={14} />
                        <span>{stats.onlineNodes}/{stats.totalNodes} nodes</span>
                    </div>
                </div>
                
                <div className={styles.headerActions}>
                    <button onClick={() => setShowAddModal(true)}>
                        <Plus size={14} /> Add
                    </button>
                    <button onClick={() => setShowSettingsModal(true)}>
                        <Settings size={14} />
                    </button>
                </div>
            </div>

            {/* Tabs */}
            <div className={styles.tabs}>
                <button 
                    className={activeTab === 'transfers' ? styles.active : ''}
                    onClick={() => setActiveTab('transfers')}
                >
                    <ArrowRightLeft size={14} /> 
                    Transfers
                    {stats.activeDownloads > 0 && (
                        <span className={styles.badge}>{stats.activeDownloads}</span>
                    )}
                </button>
                <button 
                    className={activeTab === 'nodes' ? styles.active : ''}
                    onClick={() => setActiveTab('nodes')}
                >
                    <Server size={14} /> 
                    Nodes
                    <span className={styles.badge}>{stats.onlineNodes}</span>
                </button>
                <button 
                    className={activeTab === 'folders' ? styles.active : ''}
                    onClick={() => setActiveTab('folders')}
                >
                    <Folder size={14} /> Sync Folders
                </button>
                <button 
                    className={activeTab === 'settings' ? styles.active : ''}
                    onClick={() => setActiveTab('settings')}
                >
                    <Settings size={14} /> Settings
                </button>
            </div>

            {/* Content */}
            <div className={styles.content}>
                {activeTab === 'transfers' && (
                    <div className={styles.transfers}>
                        <div className={styles.transfersToolbar}>
                            <div className={styles.searchBox}>
                                <Search size={14} />
                                <input 
                                    type="text"
                                    placeholder="Search transfers..."
                                    value={searchQuery}
                                    onChange={(e) => setSearchQuery(e.target.value)}
                                />
                            </div>
                            <select 
                                value={filterStatus}
                                onChange={(e) => setFilterStatus(e.target.value)}
                            >
                                <option value="all">All Status</option>
                                <option value="downloading">Downloading</option>
                                <option value="uploading">Uploading</option>
                                <option value="seeding">Seeding</option>
                                <option value="paused">Paused</option>
                                <option value="queued">Queued</option>
                                <option value="completed">Completed</option>
                                <option value="error">Error</option>
                            </select>
                            {selectedTransfers.size > 0 && (
                                <div className={styles.bulkActions}>
                                    <button><Pause size={14} /> Pause All</button>
                                    <button><Play size={14} /> Resume All</button>
                                    <button><Trash2 size={14} /> Remove</button>
                                </div>
                            )}
                        </div>

                        <div className={styles.transferList}>
                            {filteredTransfers.map(transfer => (
                                <div 
                                    key={transfer.id}
                                    className={`${styles.transferItem} ${selectedTransfers.has(transfer.id) ? styles.selected : ''} ${expandedTransfer === transfer.id ? styles.expanded : ''}`}
                                >
                                    <div className={styles.transferMain} onClick={() => setExpandedTransfer(expandedTransfer === transfer.id ? null : transfer.id)}>
                                        <input 
                                            type="checkbox"
                                            checked={selectedTransfers.has(transfer.id)}
                                            onChange={() => toggleTransferSelection(transfer.id)}
                                            onClick={e => e.stopPropagation()}
                                        />
                                        
                                        <div className={styles.transferIcon}>
                                            {getFileIcon(transfer.type)}
                                        </div>
                                        
                                        <div className={styles.transferInfo}>
                                            <div className={styles.transferName}>{transfer.name}</div>
                                            <div className={styles.transferMeta}>
                                                <span>{formatBytes(transfer.size)}</span>
                                                <span>•</span>
                                                <span>{transfer.path}</span>
                                                {transfer.tags.length > 0 && (
                                                    <>
                                                        <span>•</span>
                                                        {transfer.tags.map(tag => (
                                                            <span key={tag} className={styles.tag}>{tag}</span>
                                                        ))}
                                                    </>
                                                )}
                                            </div>
                                        </div>
                                        
                                        <div className={styles.transferProgress}>
                                            <div className={styles.progressBar}>
                                                <div 
                                                    className={`${styles.progressFill} ${styles[transfer.status]}`}
                                                    style={{ width: `${transfer.progress}%` }}
                                                />
                                            </div>
                                            <span className={styles.progressText}>{transfer.progress}%</span>
                                        </div>
                                        
                                        <div className={styles.transferStatus}>
                                            {getStatusIcon(transfer.status)}
                                            <span className={styles[transfer.status]}>{transfer.status}</span>
                                        </div>
                                        
                                        <div className={styles.transferSpeed}>
                                            {transfer.status === 'downloading' && (
                                                <>
                                                    <span className={styles.download}>
                                                        <ArrowDown size={12} /> {formatSpeed(transfer.downloadSpeed)}
                                                    </span>
                                                    <span className={styles.eta}>ETA: {formatETA(transfer.eta)}</span>
                                                </>
                                            )}
                                            {(transfer.status === 'seeding' || transfer.status === 'uploading') && (
                                                <span className={styles.upload}>
                                                    <ArrowUp size={12} /> {formatSpeed(transfer.uploadSpeed)}
                                                </span>
                                            )}
                                        </div>
                                        
                                        <div className={styles.transferPeers}>
                                            <Users size={12} />
                                            <span>{transfer.seeders}S / {transfer.leechers}L</span>
                                        </div>
                                        
                                        <div className={styles.transferActions}>
                                            <button 
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    toggleTransferPause(transfer);
                                                }}
                                                title={transfer.status === 'paused' ? 'Resume' : 'Pause'}
                                            >
                                                {transfer.status === 'paused' ? <Play size={14} /> : <Pause size={14} />}
                                            </button>
                                            <button 
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    removeTransfer(transfer.id);
                                                }}
                                                title="Remove"
                                            >
                                                <Trash2 size={14} />
                                            </button>
                                            <button onClick={(e) => e.stopPropagation()}>
                                                <MoreHorizontal size={14} />
                                            </button>
                                        </div>
                                    </div>
                                    
                                    {expandedTransfer === transfer.id && (
                                        <div className={styles.transferDetails}>
                                            <div className={styles.detailGrid}>
                                                <div className={styles.detailItem}>
                                                    <label>Hash</label>
                                                    <span className={styles.hash}>{transfer.hash}</span>
                                                </div>
                                                <div className={styles.detailItem}>
                                                    <label>Added</label>
                                                    <span>{new Date(transfer.addedAt).toLocaleString()}</span>
                                                </div>
                                                {transfer.completedAt && (
                                                    <div className={styles.detailItem}>
                                                        <label>Completed</label>
                                                        <span>{new Date(transfer.completedAt).toLocaleString()}</span>
                                                    </div>
                                                )}
                                                {transfer.scheduledStart && (
                                                    <div className={styles.detailItem}>
                                                        <label>Scheduled Start</label>
                                                        <span>{new Date(transfer.scheduledStart).toLocaleString()}</span>
                                                    </div>
                                                )}
                                                <div className={styles.detailItem}>
                                                    <label>Priority</label>
                                                    <select 
                                                        value={transfer.priority}
                                                        onChange={(e) => {
                                                            setTransfers(prev => prev.map(t => 
                                                                t.id === transfer.id ? { ...t, priority: e.target.value as Priority } : t
                                                            ));
                                                        }}
                                                    >
                                                        <option value="low">Low</option>
                                                        <option value="normal">Normal</option>
                                                        <option value="high">High</option>
                                                        <option value="urgent">Urgent</option>
                                                    </select>
                                                </div>
                                            </div>
                                            {transfer.error && (
                                                <div className={styles.errorMessage}>
                                                    <AlertTriangle size={14} />
                                                    {transfer.error}
                                                </div>
                                            )}
                                        </div>
                                    )}
                                </div>
                            ))}
                            
                            {filteredTransfers.length === 0 && (
                                <div className={styles.emptyState}>
                                    <ArrowRightLeft size={48} />
                                    <p>No transfers</p>
                                    <span>Add a file or magnet link to start</span>
                                </div>
                            )}
                        </div>
                    </div>
                )}

                {activeTab === 'nodes' && (
                    <div className={styles.nodes}>
                        <div className={styles.nodesHeader}>
                            <div className={styles.nodesSummary}>
                                <span className={styles.online}>{stats.onlineNodes} Online</span>
                                <span className={styles.offline}>{stats.totalNodes - stats.onlineNodes} Offline</span>
                            </div>
                            <button className={styles.addNodeBtn}>
                                <Plus size={14} /> Add Node
                            </button>
                        </div>
                        
                        <div className={styles.nodeGrid}>
                            {nodes.map(node => (
                                <div key={node.id} className={`${styles.nodeCard} ${styles[node.status]}`}>
                                    <div className={styles.nodeHeader}>
                                        <div className={styles.nodeIcon}>
                                            {node.isLocal ? <HardDrive size={20} /> : <Server size={20} />}
                                        </div>
                                        <div className={styles.nodeInfo}>
                                            <h4>{node.name}</h4>
                                            <span className={styles.nodeAddress}>{node.address}:{node.port}</span>
                                        </div>
                                        <div className={styles.nodeStatusBadge}>
                                            {getNodeStatusIcon(node.status)}
                                            <span>{node.status}</span>
                                        </div>
                                    </div>
                                    
                                    <div className={styles.nodeStats}>
                                        <div className={styles.nodeStat}>
                                            <ArrowUp size={12} />
                                            <span>{formatSpeed(node.uploadSpeed)}</span>
                                        </div>
                                        <div className={styles.nodeStat}>
                                            <ArrowDown size={12} />
                                            <span>{formatSpeed(node.downloadSpeed)}</span>
                                        </div>
                                        <div className={styles.nodeStat}>
                                            <File size={12} />
                                            <span>{node.sharedFiles} files</span>
                                        </div>
                                    </div>
                                    
                                    <div className={styles.nodeStorage}>
                                        <div className={styles.storageBar}>
                                            <div 
                                                className={styles.storageFill}
                                                style={{ width: `${(node.usedStorage / node.totalStorage) * 100}%` }}
                                            />
                                        </div>
                                        <span>{formatBytes(node.usedStorage)} / {formatBytes(node.totalStorage)}</span>
                                    </div>
                                    
                                    <div className={styles.nodeFooter}>
                                        <span className={styles.nodeVersion}>v{node.version}</span>
                                        {node.isTrusted && (
                                            <span className={styles.trustedBadge}>
                                                <Shield size={10} /> Trusted
                                            </span>
                                        )}
                                        <span className={styles.lastSeen}>
                                            Last seen: {new Date(node.lastSeen).toLocaleString()}
                                        </span>
                                    </div>
                                    
                                    <div className={styles.nodeActions}>
                                        <button title="Sync"><RefreshCw size={14} /></button>
                                        <button title="Settings"><Settings size={14} /></button>
                                        <button title="Disconnect"><Unlink size={14} /></button>
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>
                )}

                {activeTab === 'folders' && (
                    <div className={styles.folders}>
                        <div className={styles.foldersHeader}>
                            <h3>Synchronized Folders</h3>
                            <button className={styles.addFolderBtn}>
                                <Plus size={14} /> Add Folder
                            </button>
                        </div>
                        
                        <div className={styles.folderList}>
                            {syncFolders.map(folder => (
                                <div key={folder.id} className={styles.folderCard}>
                                    <div className={styles.folderIcon}>
                                        <Folder size={24} />
                                    </div>
                                    
                                    <div className={styles.folderInfo}>
                                        <h4>{folder.name}</h4>
                                        <span className={styles.folderPath}>{folder.path}</span>
                                        <div className={styles.folderMeta}>
                                            <span>{folder.syncedFiles}/{folder.totalFiles} files</span>
                                            <span>•</span>
                                            <span>{formatBytes(folder.syncedSize)} / {formatBytes(folder.totalSize)}</span>
                                        </div>
                                    </div>
                                    
                                    <div className={styles.folderProgress}>
                                        <div className={styles.progressBar}>
                                            <div 
                                                className={`${styles.progressFill} ${styles[folder.status]}`}
                                                style={{ width: `${(folder.syncedFiles / folder.totalFiles) * 100}%` }}
                                            />
                                        </div>
                                        <span>{Math.round((folder.syncedFiles / folder.totalFiles) * 100)}%</span>
                                    </div>
                                    
                                    <div className={`${styles.folderStatus} ${styles[folder.status]}`}>
                                        {folder.status === 'synced' && <CheckCircle size={14} />}
                                        {folder.status === 'syncing' && <RefreshCw size={14} className={styles.spinning} />}
                                        {folder.status === 'pending' && <Clock size={14} />}
                                        {folder.status === 'error' && <XCircle size={14} />}
                                        <span>{folder.status}</span>
                                    </div>
                                    
                                    <div className={styles.folderOptions}>
                                        {folder.autoSync && <span className={styles.optionBadge}>Auto</span>}
                                        {folder.bidirectional && <span className={styles.optionBadge}>Bidirectional</span>}
                                    </div>
                                    
                                    <div className={styles.folderActions}>
                                        <button title="Sync Now"><RefreshCw size={14} /></button>
                                        <button title="Settings"><Settings size={14} /></button>
                                        <button title="Remove"><Trash2 size={14} /></button>
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>
                )}

                {activeTab === 'settings' && (
                    <div className={styles.settings}>
                        <div className={styles.settingsSection}>
                            <h3><Network size={16} /> Network Settings</h3>
                            <div className={styles.settingRow}>
                                <label>
                                    <span>Node Mode</span>
                                    <small>Enable to participate in distributed file sharing network</small>
                                </label>
                                <input 
                                    type="checkbox"
                                    checked={nodeMode}
                                    onChange={(e) => setNodeMode(e.target.checked)}
                                />
                            </div>
                            <div className={styles.settingRow}>
                                <label>
                                    <span>Auto-Seed Completed Downloads</span>
                                    <small>Continue sharing files after download completes</small>
                                </label>
                                <input 
                                    type="checkbox"
                                    checked={autoSeed}
                                    onChange={(e) => setAutoSeed(e.target.checked)}
                                />
                            </div>
                            <div className={styles.settingRow}>
                                <label>
                                    <span>Maximum Connections</span>
                                    <small>Total number of peer connections</small>
                                </label>
                                <input 
                                    type="number"
                                    value={maxConnections}
                                    onChange={(e) => setMaxConnections(parseInt(e.target.value) || 0)}
                                    min={1}
                                    max={500}
                                />
                            </div>
                        </div>
                        
                        <div className={styles.settingsSection}>
                            <h3><Zap size={16} /> Bandwidth Limits</h3>
                            <div className={styles.settingRow}>
                                <label>
                                    <span>Download Limit</span>
                                    <small>0 = Unlimited</small>
                                </label>
                                <div className={styles.limitInput}>
                                    <input 
                                        type="number"
                                        value={globalDownloadLimit}
                                        onChange={(e) => setGlobalDownloadLimit(parseInt(e.target.value) || 0)}
                                        min={0}
                                    />
                                    <span>KB/s</span>
                                </div>
                            </div>
                            <div className={styles.settingRow}>
                                <label>
                                    <span>Upload Limit</span>
                                    <small>0 = Unlimited</small>
                                </label>
                                <div className={styles.limitInput}>
                                    <input 
                                        type="number"
                                        value={globalUploadLimit}
                                        onChange={(e) => setGlobalUploadLimit(parseInt(e.target.value) || 0)}
                                        min={0}
                                    />
                                    <span>KB/s</span>
                                </div>
                            </div>
                        </div>
                        
                        <div className={styles.settingsSection}>
                            <h3><Calendar size={16} /> Scheduled Limits</h3>
                            <p className={styles.settingsNote}>
                                Configure bandwidth limits for specific times of day
                            </p>
                            <button className={styles.addScheduleBtn}>
                                <Plus size={14} /> Add Schedule
                            </button>
                        </div>
                    </div>
                )}
            </div>

            {/* Add Transfer Modal */}
            {showAddModal && (
                <div className={styles.modal} onClick={() => setShowAddModal(false)}>
                    <div className={styles.modalContent} onClick={e => e.stopPropagation()}>
                        <h3>Add Transfer</h3>
                        <div className={styles.formGroup}>
                            <label>Torrent/Magnet Link or URL</label>
                            <input
                                type="text"
                                value={newTransferUrl}
                                onChange={(e) => setNewTransferUrl(e.target.value)}
                                placeholder="Enter magnet link, torrent URL, or file URL"
                            />
                        </div>
                        <div className={styles.formGroup}>
                            <label>Or select a .torrent file</label>
                            <button className={styles.browseBtn}>
                                <Folder size={14} /> Browse
                            </button>
                        </div>
                        <div className={styles.modalActions}>
                            <button onClick={() => setShowAddModal(false)}>Cancel</button>
                            <button onClick={addTransfer} disabled={!newTransferUrl.trim()}>
                                <Plus size={14} /> Add Transfer
                            </button>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
};

export default P2PFileSync;
