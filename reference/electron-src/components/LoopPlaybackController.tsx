// ============================================
// Loop Playback Controller
// UI for configuring non-time playmodes
// ============================================

import React, { useState, useCallback } from 'react';
import { PlayMode, FolderSortOrder, NonTimePlayConfig } from '../lib/types';
import { scanFolder, sortFiles, convertToPlaylistItems } from '../lib/folderScanner';
import { 
    FolderOpen, Play, Shuffle, Clock, Calendar, 
    FileText, HardDrive, X, Check
} from 'lucide-react';
import styles from './LoopPlaybackController.module.css';

const isElectron = !!(window as any).electronAPI;

interface LoopPlaybackControllerProps {
    channelId: string;
    currentConfig?: NonTimePlayConfig;
    onConfigChange: (config: NonTimePlayConfig) => void;
    onClose?: () => void;
}

export function LoopPlaybackController({
    channelId,
    currentConfig,
    onConfigChange,
    onClose
}: LoopPlaybackControllerProps) {
    const [mode, setMode] = useState<PlayMode>(currentConfig?.mode || 'scheduled');
    const [folderPath, setFolderPath] = useState<string>(currentConfig?.folderPath || '');
    const [sortOrder, setSortOrder] = useState<FolderSortOrder>(currentConfig?.sortOrder || 'name');
    const [loopUntilHour, setLoopUntilHour] = useState<number | undefined>(currentConfig?.loopUntilHour);
    const [scanning, setScanning] = useState(false);
    const [fileCount, setFileCount] = useState(0);

    const handleSelectFolder = useCallback(async () => {
        if (!isElectron) {
            alert('Folder selection is only available in Electron');
            return;
        }

        try {
            const result = await (window as any).electronAPI?.openFolderDialog?.();
            if (result && result.length > 0) {
                const path = result[0];
                setFolderPath(path);
                
                // Scan folder to show file count
                setScanning(true);
                try {
                    const files = await scanFolder(path);
                    const sorted = sortFiles(files, sortOrder);
                    setFileCount(sorted.length);
                } catch (error) {
                    console.error('[LoopPlaybackController] Error scanning folder:', error);
                } finally {
                    setScanning(false);
                }
            }
        } catch (error) {
            console.error('[LoopPlaybackController] Error selecting folder:', error);
        }
    }, [sortOrder]);

    const handleSave = useCallback(() => {
        const config: NonTimePlayConfig = {
            mode,
            folderPath: mode === 'folder_loop' ? folderPath : undefined,
            sortOrder: mode === 'folder_loop' ? sortOrder : undefined,
            loopUntilHour,
        };
        onConfigChange(config);
        onClose?.();
    }, [mode, folderPath, sortOrder, loopUntilHour, onConfigChange, onClose]);

    const handleScanFolder = useCallback(async () => {
        if (!folderPath) return;
        
        setScanning(true);
        try {
            const files = await scanFolder(folderPath);
            const sorted = sortFiles(files, sortOrder);
            setFileCount(sorted.length);
        } catch (error) {
            console.error('[LoopPlaybackController] Error scanning folder:', error);
        } finally {
            setScanning(false);
        }
    }, [folderPath, sortOrder]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h3>Playback Mode Configuration</h3>
                {onClose && (
                    <button className={styles.closeBtn} onClick={onClose}>
                        <X size={18} />
                    </button>
                )}
            </div>

            <div className={styles.content}>
                {/* Mode Selection */}
                <div className={styles.section}>
                    <label className={styles.label}>Playback Mode</label>
                    <div className={styles.modeOptions}>
                        <button
                            className={`${styles.modeBtn} ${mode === 'scheduled' ? styles.active : ''}`}
                            onClick={() => setMode('scheduled')}
                        >
                            <Calendar size={16} />
                            <span>Scheduled</span>
                            <span className={styles.modeDesc}>Follow weekly schedule</span>
                        </button>
                        <button
                            className={`${styles.modeBtn} ${mode === 'playlist_loop' ? styles.active : ''}`}
                            onClick={() => setMode('playlist_loop')}
                        >
                            <Play size={16} />
                            <span>Playlist Loop</span>
                            <span className={styles.modeDesc}>Loop current playlist</span>
                        </button>
                        <button
                            className={`${styles.modeBtn} ${mode === 'folder_loop' ? styles.active : ''}`}
                            onClick={() => setMode('folder_loop')}
                        >
                            <HardDrive size={16} />
                            <span>Folder Loop</span>
                            <span className={styles.modeDesc}>Loop folder contents</span>
                        </button>
                    </div>
                </div>

                {/* Folder Configuration (for folder_loop) */}
                {mode === 'folder_loop' && (
                    <div className={styles.section}>
                        <label className={styles.label}>Folder</label>
                        <div className={styles.folderSelector}>
                            <input
                                type="text"
                                value={folderPath}
                                readOnly
                                className={styles.folderInput}
                                placeholder="No folder selected"
                            />
                            <button
                                className={styles.folderBtn}
                                onClick={handleSelectFolder}
                                disabled={scanning}
                            >
                                <FolderOpen size={16} />
                                {scanning ? 'Scanning...' : 'Select Folder'}
                            </button>
                            {folderPath && (
                                <button
                                    className={styles.scanBtn}
                                    onClick={handleScanFolder}
                                    disabled={scanning}
                                >
                                    <FileText size={14} />
                                    Refresh
                                </button>
                            )}
                        </div>
                        {fileCount > 0 && (
                            <div className={styles.fileCount}>
                                Found {fileCount} media files
                            </div>
                        )}

                        <label className={styles.label}>Sort Order</label>
                        <div className={styles.sortOptions}>
                            <button
                                className={`${styles.sortBtn} ${sortOrder === 'name' ? styles.active : ''}`}
                                onClick={() => setSortOrder('name')}
                            >
                                <FileText size={14} />
                                Name
                            </button>
                            <button
                                className={`${styles.sortBtn} ${sortOrder === 'date' ? styles.active : ''}`}
                                onClick={() => setSortOrder('date')}
                            >
                                <Calendar size={14} />
                                Date
                            </button>
                            <button
                                className={`${styles.sortBtn} ${sortOrder === 'size' ? styles.active : ''}`}
                                onClick={() => setSortOrder('size')}
                            >
                                <HardDrive size={14} />
                                Size
                            </button>
                            <button
                                className={`${styles.sortBtn} ${sortOrder === 'random' ? styles.active : ''}`}
                                onClick={() => setSortOrder('random')}
                            >
                                <Shuffle size={14} />
                                Random
                            </button>
                        </div>
                    </div>
                )}

                {/* Loop Until Hour */}
                <div className={styles.section}>
                    <label className={styles.label}>
                        <Clock size={14} />
                        Loop Until Hour (Optional)
                    </label>
                    <div className={styles.hourSelector}>
                        <input
                            type="number"
                            min="0"
                            max="23"
                            value={loopUntilHour ?? ''}
                            onChange={(e) => {
                                const val = e.target.value;
                                setLoopUntilHour(val === '' ? undefined : parseInt(val, 10));
                            }}
                            className={styles.hourInput}
                            placeholder="Loop forever"
                        />
                        <span className={styles.hourLabel}>:00</span>
                        {loopUntilHour !== undefined && (
                            <button
                                className={styles.clearBtn}
                                onClick={() => setLoopUntilHour(undefined)}
                            >
                                <X size={12} />
                            </button>
                        )}
                    </div>
                    <div className={styles.hourHelp}>
                        {loopUntilHour !== undefined 
                            ? `Playback will stop at ${loopUntilHour.toString().padStart(2, '0')}:00 and return to scheduled mode`
                            : 'Playback will loop continuously until manually stopped'}
                    </div>
                </div>
            </div>

            <div className={styles.footer}>
                <button className={styles.cancelBtn} onClick={onClose}>
                    Cancel
                </button>
                <button className={styles.saveBtn} onClick={handleSave}>
                    <Check size={16} />
                    Save Configuration
                </button>
            </div>
        </div>
    );
}

