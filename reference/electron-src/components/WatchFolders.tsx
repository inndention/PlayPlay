import React, { useState, useEffect, useCallback } from 'react';
import styles from './WatchFolders.module.css';
import { MediaType } from '../lib/types';
import {
    Folder, FolderPlus, FolderOpen, Eye, EyeOff, RefreshCw, Trash2, Edit2,
    Settings, Play, Pause, Check, X, AlertTriangle, Clock, Film, Image as ImageIcon,
    Tag, Tv, Megaphone, HardDrive, Activity, Zap, Plus, Save, FileText
} from 'lucide-react';

interface WatchFolder {
    id: string;
    name: string;
    path: string;
    enabled: boolean;
    autoImportType: MediaType;
    autoAssignClient?: string;
    autoAssignSeries?: string;
    watchSubfolders: boolean;
    deleteAfterImport: boolean;
    moveToFolder?: string;
    filePatterns: string[];
    lastScanned?: string;
    filesProcessed: number;
    createdAt: string;
}

interface WatchFoldersProps {
    onImportMedia?: (filePaths: string[], type: MediaType, metadata?: any) => Promise<void>;
}

const STORAGE_KEY = 'playplay_watch_folders';

const DEFAULT_PATTERNS = ['*.mp4', '*.mov', '*.webm', '*.avi', '*.mkv', '*.jpg', '*.jpeg', '*.png', '*.gif'];

export function WatchFolders({ onImportMedia }: WatchFoldersProps) {
    const [folders, setFolders] = useState<WatchFolder[]>(() => {
        try {
            const saved = localStorage.getItem(STORAGE_KEY);
            return saved ? JSON.parse(saved) : [];
        } catch {
            return [];
        }
    });
    
    const [editingFolder, setEditingFolder] = useState<WatchFolder | null>(null);
    const [scanning, setScanningId] = useState<string | null>(null);
    const [scanResults, setScanResults] = useState<Map<string, { found: number; imported: number; errors: string[] }>>(new Map());
    const [isWatching, setIsWatching] = useState(false);
    const [watchInterval, setWatchIntervalId] = useState<number | null>(null);

    const electronAPI = (window as any).electronAPI;
    const isElectron = !!electronAPI;

    // Save folders to localStorage
    useEffect(() => {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(folders));
    }, [folders]);

    // Auto-watch loop
    useEffect(() => {
        if (isWatching && isElectron) {
            const interval = window.setInterval(() => {
                folders.filter(f => f.enabled).forEach(folder => {
                    scanFolder(folder, true);
                });
            }, 30000); // Scan every 30 seconds
            
            setWatchIntervalId(interval);
            return () => window.clearInterval(interval);
        } else if (watchInterval) {
            window.clearInterval(watchInterval);
            setWatchIntervalId(null);
        }
    }, [isWatching, folders, isElectron]);

    const scanFolder = useCallback(async (folder: WatchFolder, silent: boolean = false) => {
        if (!isElectron || !electronAPI.scanWatchFolder) return;
        
        if (!silent) setScanningId(folder.id);
        
        try {
            const result = await electronAPI.scanWatchFolder({
                path: folder.path,
                patterns: folder.filePatterns,
                recursive: folder.watchSubfolders
            });

            if (result.success && result.files.length > 0) {
                // Import found files
                if (onImportMedia) {
                    await onImportMedia(
                        result.files.map((f: any) => f.path),
                        folder.autoImportType,
                        {
                            client: folder.autoAssignClient,
                            series: folder.autoAssignSeries
                        }
                    );
                }

                // Update folder stats
                setFolders(prev => prev.map(f => 
                    f.id === folder.id 
                        ? { 
                            ...f, 
                            lastScanned: new Date().toISOString(),
                            filesProcessed: f.filesProcessed + result.files.length
                        }
                        : f
                ));

                // Handle post-import actions
                if (folder.deleteAfterImport || folder.moveToFolder) {
                    for (const file of result.files) {
                        if (folder.deleteAfterImport) {
                            await electronAPI.deleteFile?.(file.path);
                        } else if (folder.moveToFolder) {
                            await electronAPI.moveFile?.(file.path, folder.moveToFolder);
                        }
                    }
                }

                setScanResults(prev => new Map(prev).set(folder.id, {
                    found: result.files.length,
                    imported: result.files.length,
                    errors: []
                }));
            } else {
                setScanResults(prev => new Map(prev).set(folder.id, {
                    found: 0,
                    imported: 0,
                    errors: result.error ? [result.error] : []
                }));
            }
        } catch (error) {
            console.error('Scan error:', error);
            setScanResults(prev => new Map(prev).set(folder.id, {
                found: 0,
                imported: 0,
                errors: [String(error)]
            }));
        } finally {
            if (!silent) setScanningId(null);
        }
    }, [electronAPI, onImportMedia, isElectron]);

    const handleCreateFolder = () => {
        const newFolder: WatchFolder = {
            id: `watch_${Date.now()}`,
            name: 'New Watch Folder',
            path: '',
            enabled: true,
            autoImportType: 'program',
            watchSubfolders: false,
            deleteAfterImport: false,
            filePatterns: [...DEFAULT_PATTERNS],
            filesProcessed: 0,
            createdAt: new Date().toISOString()
        };
        setEditingFolder(newFolder);
    };

    const handleBrowseFolder = async () => {
        if (!isElectron || !electronAPI.openFolderDialog) return;
        
        try {
            const result = await electronAPI.openFolderDialog();
            if (!result.canceled && result.filePaths?.[0]) {
                setEditingFolder(prev => prev ? { ...prev, path: result.filePaths[0] } : null);
            }
        } catch (error) {
            console.error('Failed to open folder dialog:', error);
        }
    };

    const handleSaveFolder = (folder: WatchFolder) => {
        setFolders(prev => {
            const existing = prev.findIndex(f => f.id === folder.id);
            if (existing >= 0) {
                const updated = [...prev];
                updated[existing] = folder;
                return updated;
            }
            return [...prev, folder];
        });
        setEditingFolder(null);
    };

    const handleDeleteFolder = (id: string) => {
        if (!confirm('Delete this watch folder?')) return;
        setFolders(prev => prev.filter(f => f.id !== id));
    };

    const handleToggleFolder = (id: string) => {
        setFolders(prev => prev.map(f => 
            f.id === id ? { ...f, enabled: !f.enabled } : f
        ));
    };

    const handleScanAll = async () => {
        for (const folder of folders.filter(f => f.enabled)) {
            await scanFolder(folder);
        }
    };

    const getTypeIcon = (type: MediaType) => {
        switch (type) {
            case 'program': return Tv;
            case 'ad': return Megaphone;
            case 'station_id': return Tag;
            case 'bumper': return Zap;
            case 'promo': return Film;
            case 'image': return ImageIcon;
            default: return Film;
        }
    };

    if (!isElectron) {
        return (
            <div className={styles.container}>
                <div className={styles.notAvailable}>
                    <AlertTriangle size={48} />
                    <h3>Watch Folders</h3>
                    <p>This feature is only available in the desktop application.</p>
                    <p>Watch folders allow automatic import of media from designated directories.</p>
                </div>
            </div>
        );
    }

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <FolderOpen size={20} />
                    <h2>Watch Folders</h2>
                </div>
                <div className={styles.headerActions}>
                    <button 
                        className={`${styles.watchToggle} ${isWatching ? styles.active : ''}`}
                        onClick={() => setIsWatching(!isWatching)}
                    >
                        {isWatching ? <><Pause size={16} /> Stop Watching</> : <><Play size={16} /> Start Watching</>}
                    </button>
                    <button className={styles.scanAllBtn} onClick={handleScanAll} disabled={!!scanning}>
                        <RefreshCw size={16} className={scanning ? styles.spinning : ''} />
                        Scan All
                    </button>
                    <button className={styles.addBtn} onClick={handleCreateFolder}>
                        <FolderPlus size={16} /> Add Folder
                    </button>
                </div>
            </div>

            {/* Status Bar */}
            <div className={styles.statusBar}>
                <div className={styles.statusItem}>
                    <Activity size={14} className={isWatching ? styles.statusActive : ''} />
                    <span>{isWatching ? 'Watching' : 'Stopped'}</span>
                </div>
                <div className={styles.statusItem}>
                    <Folder size={14} />
                    <span>{folders.filter(f => f.enabled).length} active folders</span>
                </div>
                <div className={styles.statusItem}>
                    <FileText size={14} />
                    <span>{folders.reduce((sum, f) => sum + f.filesProcessed, 0)} files imported</span>
                </div>
            </div>

            {/* Folders List */}
            <div className={styles.foldersList}>
                {folders.length === 0 ? (
                    <div className={styles.emptyState}>
                        <FolderPlus size={48} />
                        <h3>No Watch Folders</h3>
                        <p>Add folders to automatically import media when new files appear.</p>
                        <button onClick={handleCreateFolder}>
                            <Plus size={16} /> Add Watch Folder
                        </button>
                    </div>
                ) : (
                    folders.map(folder => {
                        const TypeIcon = getTypeIcon(folder.autoImportType);
                        const result = scanResults.get(folder.id);
                        
                        return (
                            <div key={folder.id} className={`${styles.folderCard} ${!folder.enabled ? styles.disabled : ''}`}>
                                <div className={styles.folderHeader}>
                                    <div className={styles.folderIcon}>
                                        <Folder size={24} />
                                        {folder.enabled && isWatching && (
                                            <span className={styles.watchingIndicator} />
                                        )}
                                    </div>
                                    <div className={styles.folderInfo}>
                                        <h3>{folder.name}</h3>
                                        <span className={styles.folderPath}>{folder.path || 'No path set'}</span>
                                    </div>
                                    <div className={styles.folderActions}>
                                        <button
                                            className={`${styles.toggleBtn} ${folder.enabled ? styles.enabled : ''}`}
                                            onClick={() => handleToggleFolder(folder.id)}
                                            title={folder.enabled ? 'Disable' : 'Enable'}
                                        >
                                            {folder.enabled ? <Eye size={16} /> : <EyeOff size={16} />}
                                        </button>
                                        <button
                                            className={styles.scanBtn}
                                            onClick={() => scanFolder(folder)}
                                            disabled={scanning === folder.id}
                                            title="Scan now"
                                        >
                                            <RefreshCw size={16} className={scanning === folder.id ? styles.spinning : ''} />
                                        </button>
                                        <button
                                            className={styles.editBtn}
                                            onClick={() => setEditingFolder(folder)}
                                            title="Edit"
                                        >
                                            <Edit2 size={16} />
                                        </button>
                                        <button
                                            className={styles.deleteBtn}
                                            onClick={() => handleDeleteFolder(folder.id)}
                                            title="Delete"
                                        >
                                            <Trash2 size={16} />
                                        </button>
                                    </div>
                                </div>

                                <div className={styles.folderDetails}>
                                    <div className={styles.detailItem}>
                                        <TypeIcon size={14} />
                                        <span>Auto-tag as: {folder.autoImportType}</span>
                                    </div>
                                    {folder.watchSubfolders && (
                                        <div className={styles.detailItem}>
                                            <Folder size={14} />
                                            <span>Includes subfolders</span>
                                        </div>
                                    )}
                                    {folder.deleteAfterImport && (
                                        <div className={styles.detailItem}>
                                            <Trash2 size={14} />
                                            <span>Delete after import</span>
                                        </div>
                                    )}
                                    <div className={styles.detailItem}>
                                        <Clock size={14} />
                                        <span>
                                            Last scan: {folder.lastScanned 
                                                ? new Date(folder.lastScanned).toLocaleString() 
                                                : 'Never'}
                                        </span>
                                    </div>
                                    <div className={styles.detailItem}>
                                        <FileText size={14} />
                                        <span>{folder.filesProcessed} files processed</span>
                                    </div>
                                </div>

                                {/* Scan Results */}
                                {result && (
                                    <div className={`${styles.scanResult} ${result.errors.length > 0 ? styles.hasErrors : ''}`}>
                                        {result.errors.length > 0 ? (
                                            <><AlertTriangle size={14} /> {result.errors[0]}</>
                                        ) : result.found > 0 ? (
                                            <><Check size={14} /> Found {result.found} files, imported {result.imported}</>
                                        ) : (
                                            <><Check size={14} /> No new files found</>
                                        )}
                                    </div>
                                )}
                            </div>
                        );
                    })
                )}
            </div>

            {/* Editor Modal */}
            {editingFolder && (
                <div className={styles.editorOverlay}>
                    <div className={styles.editor}>
                        <div className={styles.editorHeader}>
                            <h3>{editingFolder.createdAt ? 'Edit Watch Folder' : 'Add Watch Folder'}</h3>
                            <button onClick={() => setEditingFolder(null)}><X size={20} /></button>
                        </div>

                        <div className={styles.editorContent}>
                            <div className={styles.formGroup}>
                                <label>Name</label>
                                <input
                                    value={editingFolder.name}
                                    onChange={e => setEditingFolder(prev => prev ? { ...prev, name: e.target.value } : null)}
                                    placeholder="Watch folder name..."
                                />
                            </div>

                            <div className={styles.formGroup}>
                                <label>Folder Path</label>
                                <div className={styles.pathInput}>
                                    <input
                                        value={editingFolder.path}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, path: e.target.value } : null)}
                                        placeholder="Select a folder..."
                                        readOnly
                                    />
                                    <button onClick={handleBrowseFolder}>
                                        <FolderOpen size={16} /> Browse
                                    </button>
                                </div>
                            </div>

                            <div className={styles.formRow}>
                                <div className={styles.formGroup}>
                                    <label>Auto-Import As</label>
                                    <select
                                        value={editingFolder.autoImportType}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, autoImportType: e.target.value as MediaType } : null)}
                                    >
                                        <option value="program">Program</option>
                                        <option value="ad">Advertisement</option>
                                        <option value="station_id">Station ID</option>
                                        <option value="bumper">Bumper</option>
                                        <option value="promo">Promo</option>
                                        <option value="video">Video</option>
                                        <option value="image">Image</option>
                                    </select>
                                </div>

                                <div className={styles.formGroup}>
                                    <label>Auto-Assign Client</label>
                                    <input
                                        value={editingFolder.autoAssignClient || ''}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, autoAssignClient: e.target.value } : null)}
                                        placeholder="Optional client name..."
                                    />
                                </div>
                            </div>

                            <div className={styles.formGroup}>
                                <label>Auto-Assign Series</label>
                                <input
                                    value={editingFolder.autoAssignSeries || ''}
                                    onChange={e => setEditingFolder(prev => prev ? { ...prev, autoAssignSeries: e.target.value } : null)}
                                    placeholder="Optional series name..."
                                />
                            </div>

                            <div className={styles.formGroup}>
                                <label>File Patterns (one per line)</label>
                                <textarea
                                    value={editingFolder.filePatterns.join('\n')}
                                    onChange={e => setEditingFolder(prev => prev ? { 
                                        ...prev, 
                                        filePatterns: e.target.value.split('\n').filter(p => p.trim()) 
                                    } : null)}
                                    placeholder="*.mp4&#10;*.mov&#10;*.webm"
                                    rows={4}
                                />
                            </div>

                            <div className={styles.checkboxGroup}>
                                <label>
                                    <input
                                        type="checkbox"
                                        checked={editingFolder.watchSubfolders}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, watchSubfolders: e.target.checked } : null)}
                                    />
                                    <span>Include subfolders</span>
                                </label>
                                <label>
                                    <input
                                        type="checkbox"
                                        checked={editingFolder.deleteAfterImport}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, deleteAfterImport: e.target.checked } : null)}
                                    />
                                    <span>Delete files after import</span>
                                </label>
                                <label>
                                    <input
                                        type="checkbox"
                                        checked={editingFolder.enabled}
                                        onChange={e => setEditingFolder(prev => prev ? { ...prev, enabled: e.target.checked } : null)}
                                    />
                                    <span>Enabled</span>
                                </label>
                            </div>
                        </div>

                        <div className={styles.editorActions}>
                            <button className={styles.cancelBtn} onClick={() => setEditingFolder(null)}>
                                Cancel
                            </button>
                            <button 
                                className={styles.saveBtn} 
                                onClick={() => handleSaveFolder(editingFolder)}
                                disabled={!editingFolder.path}
                            >
                                <Save size={16} /> Save Folder
                            </button>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
}

export default WatchFolders;
