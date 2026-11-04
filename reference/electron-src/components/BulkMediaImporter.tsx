// ============================================
// BULK MEDIA IMPORTER
// Enhanced with duplicate detection (hash-based + filename matching)
// ============================================

import React, { useState, useRef, useCallback, useEffect } from 'react';
import { MediaItem, MediaType } from '../lib/types';
import { getEnvironment, resolveFileUrl } from '../lib/fileResolver';
import { dbSaveMedia, dbGetAllMedia } from '../lib/mediaDbClient';
import { autoDetectMediaMetadata } from '../lib/mediaDetection';
import { buildImportInsights, ImportClusterSuggestion } from '../lib/aiHelper';
import { createClient, loadClients, saveClients } from '../lib/storage';
import { 
    Upload, Film, Radio, Tv, Megaphone, Play,
    Check, X, Loader2, FolderOpen, ChevronDown, AlertTriangle,
    Copy, RefreshCw, Hash, FileWarning, Sparkles, UserPlus, CalendarPlus
} from 'lucide-react';
import styles from './BulkMediaImporter.module.css';

interface ImportedFile {
    id: string;
    filename: string;
    filePath: string;
    type: MediaType;
    duration: number;
    thumbnail?: string;
    status: 'pending' | 'processing' | 'done' | 'error' | 'duplicate' | 'skipped';
    error?: string;
    fileSize?: number;
    hash?: string;
    duplicateOf?: string; // ID of existing duplicate
    duplicateMatch?: 'hash' | 'filename' | 'both';
    detectedType?: MediaType;
    detectionNotes?: string[];
    clientHint?: string;
    clientGroupHint?: string;
}

interface BulkMediaImporterProps {
    onImportComplete: (items: MediaItem[]) => void;
    onClose: () => void;
}

// Video-only extensions supported by the importer
const VIDEO_EXTENSIONS = ['mp4', 'webm', 'mov', 'avi', 'mkv', 'mxf', 'm4v', 'ts', 'mpg', 'mpeg', 'wmv', 'flv', 'vob', 'm2ts'];
const BLOCKED_MEDIA_EXTENSIONS = ['jpg', 'jpeg', 'png', 'gif', 'webp', 'svg', 'bmp', 'tif', 'tiff', 'mp3', 'wav', 'aac', 'm4a', 'flac', 'ogg', 'wma', 'aiff'];

const MEDIA_TYPE_OPTIONS: { value: MediaType; label: string; icon: React.ReactNode; color: string }[] = [
    { value: 'ad', label: 'Ads', icon: <Megaphone size={16} />, color: '#ef4444' },
    { value: 'program', label: 'Programs', icon: <Tv size={16} />, color: '#3b82f6' },
    { value: 'station_id', label: 'Station IDs', icon: <Radio size={16} />, color: '#22c55e' },
    { value: 'bumper', label: 'Bumpers', icon: <Play size={16} />, color: '#f59e0b' },
    { value: 'promo', label: 'Promos', icon: <Film size={16} />, color: '#8b5cf6' },
    { value: 'video', label: 'Videos', icon: <Film size={16} />, color: '#64748b' },
];

const generateId = () => `import_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;

// Simple hash function for quick duplicate detection
async function hashFile(filePath: string): Promise<string | null> {
    const api = (window as any).electronAPI;
    if (api?.hashFile) {
        try {
            return await api.hashFile(filePath);
        } catch {
            return null;
        }
    }
    return null;
}

// Get file size
async function getFileSize(filePath: string): Promise<number> {
    const api = (window as any).electronAPI;
    if (api?.getFileStats) {
        try {
            const stats = await api.getFileStats(filePath);
            return stats?.size || 0;
        } catch {
            return 0;
        }
    }
    return 0;
}

export function BulkMediaImporter({ onImportComplete, onClose }: BulkMediaImporterProps) {
    const [files, setFiles] = useState<ImportedFile[]>([]);
    const [selectedType, setSelectedType] = useState<MediaType>('video');
    const [isProcessing, setIsProcessing] = useState(false);
    const [showTypeDropdown, setShowTypeDropdown] = useState(false);
    const [existingMedia, setExistingMedia] = useState<MediaItem[]>([]);
    const [existingHashes, setExistingHashes] = useState<Map<string, string>>(new Map()); // hash -> mediaId
    const [existingFilenames, setExistingFilenames] = useState<Map<string, string>>(new Map()); // filename -> mediaId
    const [duplicateAction, setDuplicateAction] = useState<'skip' | 'replace' | 'keep_both'>('skip');
    const [isCheckingDuplicates, setIsCheckingDuplicates] = useState(false);
    const [aiSuggestions, setAiSuggestions] = useState<ImportClusterSuggestion[]>([]);
    const [acceptedSuggestionIds, setAcceptedSuggestionIds] = useState<Set<string>>(new Set());
    
    const fileInputRef = useRef<HTMLInputElement>(null);
    const folderInputRef = useRef<HTMLInputElement>(null);
    const env = getEnvironment();

    // Load existing media for duplicate detection
    useEffect(() => {
        loadExistingMedia();
    }, []);

    const loadExistingMedia = async () => {
        try {
            const media = await dbGetAllMedia();
            setExistingMedia(media || []);
            
            // Build hash and filename indexes
            const hashMap = new Map<string, string>();
            const filenameMap = new Map<string, string>();
            
            (media || []).forEach((m: MediaItem) => {
                // Index by filename
                if (m.filename) {
                    filenameMap.set(m.filename.toLowerCase(), m.id);
                }
                // Index by file path (for hash-based matching we'd need to load hashes)
                if (m.filePath) {
                    const filename = m.filePath.split(/[\\/]/).pop()?.toLowerCase() || '';
                    if (filename) {
                        filenameMap.set(filename, m.id);
                    }
                }
            });
            
            setExistingFilenames(filenameMap);
            console.log(`[Importer] Loaded ${media?.length || 0} existing media items for duplicate detection`);
        } catch (e) {
            console.error('Failed to load existing media:', e);
        }
    };

    // Auto-detect type from filename patterns
    const detectTypeFromFilename = (filename: string): MediaType => {
        const lower = filename.toLowerCase();
        
        if (lower.includes('_ad') || lower.includes('ad_') || lower.includes('commercial') || lower.includes('spot')) {
            return 'ad';
        }
        if (lower.includes('_id') || lower.includes('id_') || lower.includes('station') || lower.includes('ident')) {
            return 'station_id';
        }
        if (lower.includes('bump') || lower.includes('_bmp') || lower.includes('bmp_')) {
            return 'bumper';
        }
        if (lower.includes('promo') || lower.includes('teaser') || lower.includes('trailer')) {
            return 'promo';
        }
        if (lower.includes('program') || lower.includes('episode') || lower.includes('show') || lower.includes('_ep')) {
            return 'program';
        }
        
        return selectedType;
    };

    // Check for duplicates
    const checkForDuplicate = useCallback((filename: string, hash?: string): { 
        isDuplicate: boolean; 
        duplicateOf?: string; 
        matchType?: 'hash' | 'filename' | 'both';
    } => {
        const lowerFilename = filename.toLowerCase();
        const existingByFilename = existingFilenames.get(lowerFilename);
        const existingByHash = hash ? existingHashes.get(hash) : undefined;
        
        if (existingByHash && existingByFilename) {
            return { isDuplicate: true, duplicateOf: existingByHash, matchType: 'both' };
        }
        if (existingByHash) {
            return { isDuplicate: true, duplicateOf: existingByHash, matchType: 'hash' };
        }
        if (existingByFilename) {
            return { isDuplicate: true, duplicateOf: existingByFilename, matchType: 'filename' };
        }
        
        return { isDuplicate: false };
    }, [existingFilenames, existingHashes]);

    const handleFilesSelected = useCallback(async (filePaths: string[]) => {
        setIsCheckingDuplicates(true);
        
        const newFiles: ImportedFile[] = [];
        
        for (const filePath of filePaths) {
            const filename = filePath.split(/[\\/]/).pop() || 'unknown';
            const ext = filename.split('.').pop()?.toLowerCase() || '';
            if (!VIDEO_EXTENSIONS.includes(ext)) {
                newFiles.push({
                    id: generateId(),
                    filename,
                    filePath,
                    type: 'video',
                    duration: 0,
                    status: 'skipped',
                    error: BLOCKED_MEDIA_EXTENSIONS.includes(ext)
                        ? 'Skipped: importer does not ingest images or music/audio'
                        : 'Skipped: unsupported video extension',
                });
                continue;
            }
            const detectedType = detectTypeFromFilename(filename);
            
            // Get file size
            const fileSize = await getFileSize(filePath);
            
            // Check for duplicate
            const duplicateCheck = checkForDuplicate(filename);
            
            const file: ImportedFile = {
                id: generateId(),
                filename,
                filePath,
                type: detectedType,
                duration: 0,
                status: duplicateCheck.isDuplicate ? 'duplicate' : 'pending',
                fileSize,
                duplicateOf: duplicateCheck.duplicateOf,
                duplicateMatch: duplicateCheck.matchType,
                detectedType,
                detectionNotes: ext === 'mpg' || ext === 'mpeg' ? ['mpg-treated-as-mp4-container'] : [],
            };
            
            newFiles.push(file);
        }
        
        // Check for duplicates within the current batch
        const seenFilenames = new Set<string>();
        const seenPaths = new Set<string>();
        
        newFiles.forEach(file => {
            const lowerFilename = file.filename.toLowerCase();
            
            // Check if already in this batch
            if (seenFilenames.has(lowerFilename) || seenPaths.has(file.filePath)) {
                if (file.status !== 'duplicate') {
                    file.status = 'duplicate';
                    file.duplicateMatch = 'filename';
                    file.error = 'Duplicate in current batch';
                }
            }
            
            seenFilenames.add(lowerFilename);
            seenPaths.add(file.filePath);
        });
        
        setFiles(prev => {
            const nextFiles = [...prev, ...newFiles];
            setAiSuggestions(buildImportInsights(nextFiles).suggestions);
            return nextFiles;
        });
        setIsCheckingDuplicates(false);
    }, [selectedType, checkForDuplicate]);

    const handleImportClick = async () => {
        const w = typeof window !== 'undefined' ? (window as any) : null;
        
        if (w?.electronAPI?.openFileDialog) {
            try {
                const result = await w.electronAPI.openFileDialog({
                    properties: ['openFile', 'multiSelections'],
                    filters: [
                        { name: 'Video Files Only', extensions: VIDEO_EXTENSIONS }
                    ]
                });
                
                if (!result.canceled && result.filePaths?.length > 0) {
                    handleFilesSelected(result.filePaths);
                }
            } catch (err) {
                console.error('Failed to open file dialog:', err);
            }
        } else {
            fileInputRef.current?.click();
        }
    };

    const handleFolderImport = async () => {
        const w = typeof window !== 'undefined' ? (window as any) : null;
        
        if (w?.electronAPI) {
            try {
                // Use openFolderDialog to pick directory
                let folderPaths: string[] = [];
                if (w.electronAPI.openFolderDialog) {
                    folderPaths = await w.electronAPI.openFolderDialog();
                } else if (w.electronAPI.openFileDialog) {
                    const result = await w.electronAPI.openFileDialog({ properties: ['openDirectory'] });
                    if (!result.canceled) folderPaths = result.filePaths || [];
                }

                if (folderPaths.length === 0) return;

                const folderPath = folderPaths[0];
                console.log('[BulkImport] Scanning folder:', folderPath);

                // Use scanFolder which already exists in main.js
                const scannedFiles = await w.electronAPI.scanFolder(folderPath);
                console.log('[BulkImport] scanFolder returned:', scannedFiles?.length, 'entries');

                if (!scannedFiles || scannedFiles.length === 0) {
                    alert(`No files found in folder: ${folderPath}`);
                    return;
                }

                const mediaFilePaths: string[] = scannedFiles
                    .filter((f: { path: string; extension: string }) => {
                        const ext = (f.extension || '').toLowerCase().replace('.', '');
                        return VIDEO_EXTENSIONS.includes(ext);
                    })
                    .map((f: { path: string }) => f.path);

                console.log('[BulkImport] Media files found:', mediaFilePaths.length);

                if (mediaFilePaths.length === 0) {
                    alert(`No video files found in: ${folderPath}`);
                    return;
                }

                handleFilesSelected(mediaFilePaths);
            } catch (err) {
                console.error('Failed to import folder:', err);
                alert('Failed to scan folder. Check console for details.');
            }
        } else {
            // Web fallback — use a folder-type file input
            folderInputRef.current?.click();
        }
    };

    const handleHtmlFileSelect = async (e: React.ChangeEvent<HTMLInputElement>) => {
        if (e.target.files?.length) {
            const filePaths = Array.from(e.target.files).map(f => (f as any).path || f.name);
            handleFilesSelected(filePaths);
            e.target.value = '';
        }
    };

    const updateFileType = (id: string, type: MediaType) => {
        setFiles(prev => prev.map(f => f.id === id ? { ...f, type } : f));
    };

    const updateFileStatus = (id: string, status: ImportedFile['status']) => {
        setFiles(prev => prev.map(f => f.id === id ? { ...f, status } : f));
    };

    const removeFile = (id: string) => {
        setFiles(prev => {
            const nextFiles = prev.filter(f => f.id !== id);
            setAiSuggestions(buildImportInsights(nextFiles).suggestions);
            return nextFiles;
        });
    };

    const setAllToType = (type: MediaType) => {
        setFiles(prev => prev.map(f => ({ ...f, type })));
        setShowTypeDropdown(false);
    };

    // Mark duplicate for import anyway
    const markForImport = (id: string) => {
        setFiles(prev => prev.map(f => f.id === id ? { ...f, status: 'pending' } : f));
    };

    // Skip all duplicates
    const skipAllDuplicates = () => {
        setFiles(prev => prev.map(f => f.status === 'duplicate' ? { ...f, status: 'skipped' } : f));
    };

    // Import all (including duplicates)
    const importAllDuplicates = () => {
        setFiles(prev => prev.map(f => f.status === 'duplicate' ? { ...f, status: 'pending' } : f));
    };

    const acceptAiSuggestion = (suggestion: ImportClusterSuggestion, mode: 'client' | 'event') => {
        const existingClients = loadClients();
        let clientId: string | undefined;

        if (mode === 'client') {
            const existing = existingClients.find(client => client.name.toLowerCase() === suggestion.label.toLowerCase());
            const client = existing || {
                ...createClient(suggestion.label),
                notes: `AI import helper created this draft from ${suggestion.files.length} matching files. Confirm contact, billing, run dates, and whether this is sponsor/event inventory.`,
            };

            clientId = client.id;
            if (!existing) {
                saveClients([...existingClients, client]);
            }
        }

        const suggestionFileIds = new Set(suggestion.files.map(file => file.id));
        setFiles(prev => prev.map(file => {
            if (!suggestionFileIds.has(file.id)) return file;
            return {
                ...file,
                clientHint: mode === 'client' ? clientId : undefined,
                clientGroupHint: `${mode}:${suggestion.label}`,
                type: mode === 'client' && (file.type === 'video' || file.type === 'promo') ? 'ad' : file.type,
                detectionNotes: [
                    ...(file.detectionNotes || []),
                    `ai-cluster:${mode}:${suggestion.label}`,
                ],
            };
        }));
        setAcceptedSuggestionIds(prev => new Set(prev).add(suggestion.id));
    };

    const processFiles = async () => {
        setIsProcessing(true);
        const importedMedia: MediaItem[] = [];
        const now = new Date().toISOString();

        // Filter files to process
        const filesToProcess = files.filter(f => f.status === 'pending');

        for (const file of filesToProcess) {
            setFiles(prev => prev.map(f => f.id === file.id ? { ...f, status: 'processing' } : f));

            try {
                const src = await resolveFileUrl(file.filePath, 'player');
                if (!src) throw new Error('Could not resolve file URL');

                let duration = 0;
                let thumbnail: string | undefined;

                if (true) { // video-only import
                    duration = await new Promise<number>((resolve) => {
                        const video = document.createElement('video');
                        video.preload = 'metadata';
                        video.onloadedmetadata = () => resolve(video.duration);
                        video.onerror = () => resolve(0);
                        video.src = src;
                    });

                    thumbnail = await new Promise<string | undefined>((resolve) => {
                        const video = document.createElement('video');
                        video.preload = 'metadata';
                        video.crossOrigin = 'anonymous';
                        video.onloadedmetadata = () => {
                            const canvas = document.createElement('canvas');
                            canvas.width = 160;
                            canvas.height = 90;
                            const ctx = canvas.getContext('2d');
                            if (ctx) {
                                video.currentTime = Math.min(duration * 0.1, 5);
                                video.onseeked = () => {
                                    ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
                                    resolve(canvas.toDataURL('image/jpeg', 0.6));
                                };
                            } else {
                                resolve(undefined);
                            }
                        };
                        video.onerror = () => resolve(undefined);
                        video.src = src;
                    });
                }

                const mediaItem: MediaItem = {
                    ...(() => {
                        const detected = autoDetectMediaMetadata(file.filename, duration);
                        const finalType = file.type === 'video' ? detected.type : file.type;
                        return {
                            type: finalType,
                            client: file.clientHint || (finalType === 'ad' ? detected.client : undefined),
                            bumperSubtype: finalType === 'bumper' ? detected.bumperSubtype : undefined,
                            programSeries: finalType === 'program' ? detected.title : undefined,
                            episodeDate: detected.episodeDate,
                            mediaClass: detected.mediaClass,
                            detectedMediaType: detected.type,
                            detectionNotes: detected.detectionNotes,
                            importSourceFormat: /\.(mpg|mpeg)$/i.test(file.filename) ? 'mpg-as-mp4' : undefined,
                            paused: false,
                            pauseReplacementType: finalType,
                            clientGroupId: file.clientGroupHint,
                        };
                    })(),
                    id: file.id,
                    title: file.filename.replace(/\.[^/.]+$/, ''),
                    duration,
                    src,
                    filePath: file.filePath,
                    filename: file.filename,
                    thumbnail: thumbnail || '#555',
                    segments: [],
                    addedAt: now,
                    dateMade: now,
                    priority: 0,
                    playCount: 0,
                };

                await dbSaveMedia(mediaItem);
                importedMedia.push(mediaItem);

                // Update existing indexes
                setExistingFilenames(prev => {
                    const newMap = new Map(prev);
                    newMap.set(file.filename.toLowerCase(), file.id);
                    return newMap;
                });

                setFiles(prev => prev.map(f => 
                    f.id === file.id 
                        ? { ...f, status: 'done', duration, thumbnail } 
                        : f
                ));
            } catch (err) {
                console.error('Failed to process file:', file.filename, err);
                setFiles(prev => prev.map(f => 
                    f.id === file.id 
                        ? { ...f, status: 'error', error: err instanceof Error ? err.message : 'Unknown error' } 
                        : f
                ));
            }
        }

        setIsProcessing(false);
        
        if (importedMedia.length > 0) {
            onImportComplete(importedMedia);
        }
    };

    const pendingCount = files.filter(f => f.status === 'pending').length;
    const doneCount = files.filter(f => f.status === 'done').length;
    const errorCount = files.filter(f => f.status === 'error').length;
    const duplicateCount = files.filter(f => f.status === 'duplicate').length;
    const skippedCount = files.filter(f => f.status === 'skipped').length;

    return (
        <div className={styles.overlay} onClick={onClose}>
            <div className={styles.modal} onClick={e => e.stopPropagation()}>
                <div className={styles.header}>
                    <h2>Bulk Media Import</h2>
                    <button className={styles.closeBtn} onClick={onClose}>
                        <X size={18} />
                    </button>
                </div>

                <div className={styles.content}>
                    {/* Import Actions */}
                    <div className={styles.importActions}>
                        <button className={styles.importBtn} onClick={handleImportClick}>
                            <Upload size={18} />
                            Select Files
                        </button>
                        {env.isElectron && (
                            <button className={styles.importBtn} onClick={handleFolderImport}>
                                <FolderOpen size={18} />
                                Import Folder
                            </button>
                        )}
                        
                        {/* Default type selector */}
                        <div className={styles.typeSelector}>
                            <span>Default Type:</span>
                            <div className={styles.typeDropdown}>
                                <button 
                                    className={styles.typeDropdownBtn}
                                    onClick={() => setShowTypeDropdown(!showTypeDropdown)}
                                    style={{ borderColor: MEDIA_TYPE_OPTIONS.find(t => t.value === selectedType)?.color }}
                                >
                                    {MEDIA_TYPE_OPTIONS.find(t => t.value === selectedType)?.icon}
                                    {MEDIA_TYPE_OPTIONS.find(t => t.value === selectedType)?.label}
                                    <ChevronDown size={14} />
                                </button>
                                {showTypeDropdown && (
                                    <div className={styles.typeDropdownMenu}>
                                        {MEDIA_TYPE_OPTIONS.map(opt => (
                                            <button
                                                key={opt.value}
                                                className={styles.typeOption}
                                                onClick={() => { setSelectedType(opt.value); setShowTypeDropdown(false); }}
                                                style={{ borderLeftColor: opt.color }}
                                            >
                                                {opt.icon}
                                                {opt.label}
                                            </button>
                                        ))}
                                    </div>
                                )}
                            </div>
                        </div>
                    </div>

                    {/* Duplicate Warning Banner */}
                    {duplicateCount > 0 && (
                        <div className={styles.duplicateBanner}>
                            <div className={styles.duplicateInfo}>
                                <FileWarning size={18} />
                                <span>
                                    <strong>{duplicateCount}</strong> duplicate file{duplicateCount > 1 ? 's' : ''} detected
                                </span>
                            </div>
                            <div className={styles.duplicateActions}>
                                <button onClick={skipAllDuplicates} className={styles.skipAllBtn}>
                                    Skip All
                                </button>
                                <button onClick={importAllDuplicates} className={styles.importAllBtn}>
                                    Import Anyway
                                </button>
                            </div>
                        </div>
                    )}

                    {aiSuggestions.length > 0 && (
                        <div className={styles.aiAssistPanel}>
                            <div className={styles.aiAssistHeader}>
                                <Sparkles size={16} />
                                <div>
                                    <strong>AI Import Helper</strong>
                                    <span>Shared-title patterns found. Confirm what these groups mean before import.</span>
                                </div>
                            </div>
                            <div className={styles.aiSuggestionList}>
                                {aiSuggestions.map(suggestion => {
                                    const accepted = acceptedSuggestionIds.has(suggestion.id);
                                    return (
                                        <div key={suggestion.id} className={styles.aiSuggestion}>
                                            <div className={styles.aiSuggestionText}>
                                                <strong>{suggestion.label}</strong>
                                                <span>{suggestion.question}</span>
                                                <small>{suggestion.reason} Confidence {Math.round(suggestion.confidence * 100)}%.</small>
                                            </div>
                                            <div className={styles.aiSuggestionActions}>
                                                <button
                                                    type="button"
                                                    onClick={() => acceptAiSuggestion(suggestion, 'client')}
                                                    disabled={accepted}
                                                >
                                                    <UserPlus size={13} />
                                                    Draft Client
                                                </button>
                                                <button
                                                    type="button"
                                                    onClick={() => acceptAiSuggestion(suggestion, 'event')}
                                                    disabled={accepted}
                                                >
                                                    <CalendarPlus size={13} />
                                                    Tag Event
                                                </button>
                                            </div>
                                        </div>
                                    );
                                })}
                            </div>
                        </div>
                    )}

                    {/* Set All Type Buttons */}
                    {files.length > 0 && (
                        <div className={styles.bulkTypeActions}>
                            <span>Set all to:</span>
                            {MEDIA_TYPE_OPTIONS.slice(0, 5).map(opt => (
                                <button
                                    key={opt.value}
                                    className={styles.bulkTypeBtn}
                                    onClick={() => setAllToType(opt.value)}
                                    style={{ backgroundColor: opt.color + '20', borderColor: opt.color, color: opt.color }}
                                >
                                    {opt.icon}
                                    {opt.label}
                                </button>
                            ))}
                        </div>
                    )}

                    {/* Checking Duplicates Indicator */}
                    {isCheckingDuplicates && (
                        <div className={styles.checkingDuplicates}>
                            <Loader2 size={16} className={styles.spinner} />
                            <span>Checking for duplicates...</span>
                        </div>
                    )}

                    {/* File List */}
                    <div className={styles.fileList}>
                        {files.length === 0 ? (
                            <div className={styles.emptyState}>
                                <Upload size={48} strokeWidth={1} />
                                <p>Select files or drag and drop to import</p>
                                <p className={styles.hint}>
                                    Files are preimport-scanned by filename and duration; image/music imports are skipped
                                </p>
                                <p className={styles.hint}>
                                    <Hash size={14} /> Duplicate detection enabled
                                </p>
                            </div>
                        ) : (
                            files.map(file => (
                                <div 
                                    key={file.id} 
                                    className={`${styles.fileItem} ${file.status === 'duplicate' ? styles.duplicateFile : ''} ${file.status === 'skipped' ? styles.skippedFile : ''}`}
                                >
                                    <div className={styles.fileStatus}>
                                        {file.status === 'pending' && <div className={styles.statusDot} />}
                                        {file.status === 'processing' && <Loader2 size={14} className={styles.spinner} />}
                                        {file.status === 'done' && <Check size={14} className={styles.checkIcon} />}
                                        {file.status === 'error' && <X size={14} className={styles.errorIcon} />}
                                        {file.status === 'duplicate' && <Copy size={14} className={styles.duplicateIcon} />}
                                        {file.status === 'skipped' && <AlertTriangle size={14} className={styles.skippedIcon} />}
                                    </div>
                                    
                                    {file.thumbnail && (
                                        <div 
                                            className={styles.fileThumbnail}
                                            style={{ backgroundImage: `url(${file.thumbnail})` }}
                                        />
                                    )}
                                    
                                    <div className={styles.fileInfo}>
                                        <div className={styles.fileName}>{file.filename}</div>
                                        <div className={styles.fileMeta}>
                                            {file.duration > 0 && (
                                                <span className={styles.fileDuration}>
                                                    {Math.floor(file.duration / 60)}:{Math.floor(file.duration % 60).toString().padStart(2, '0')}
                                                </span>
                                            )}
                                            {file.fileSize && file.fileSize > 0 && (
                                                <span className={styles.fileSize}>
                                                    {formatFileSize(file.fileSize)}
                                                </span>
                                            )}
                                        </div>
                                        {file.status === 'duplicate' && (
                                            <div className={styles.duplicateNote}>
                                                <Copy size={12} />
                                                Duplicate ({file.duplicateMatch === 'filename' ? 'same filename' : 'matching file'})
                                            </div>
                                        )}
                                        {file.status === 'skipped' && (
                                            <div className={styles.skippedNote}>
                                                Skipped (duplicate)
                                            </div>
                                        )}
                                        {file.error && <div className={styles.fileError}>{file.error}</div>}
                                    </div>
                                    
                                    {file.status === 'duplicate' && (
                                        <button
                                            className={styles.importAnywayBtn}
                                            onClick={() => markForImport(file.id)}
                                            title="Import anyway"
                                        >
                                            <RefreshCw size={12} />
                                            Import
                                        </button>
                                    )}
                                    
                                    <select
                                        className={styles.fileTypeSelect}
                                        value={file.type}
                                        onChange={e => updateFileType(file.id, e.target.value as MediaType)}
                                        disabled={file.status !== 'pending' && file.status !== 'duplicate'}
                                        style={{ 
                                            borderColor: MEDIA_TYPE_OPTIONS.find(t => t.value === file.type)?.color,
                                            color: MEDIA_TYPE_OPTIONS.find(t => t.value === file.type)?.color
                                        }}
                                    >
                                        {MEDIA_TYPE_OPTIONS.map(opt => (
                                            <option key={opt.value} value={opt.value}>{opt.label}</option>
                                        ))}
                                    </select>
                                    
                                    <button 
                                        className={styles.removeBtn}
                                        onClick={() => removeFile(file.id)}
                                        disabled={file.status === 'processing'}
                                    >
                                        <X size={14} />
                                    </button>
                                </div>
                            ))
                        )}
                    </div>
                </div>

                <div className={styles.footer}>
                    <div className={styles.stats}>
                        {files.length > 0 && (
                            <>
                                <span>{files.length} files</span>
                                {pendingCount > 0 && <span className={styles.statPending}>⏳ {pendingCount} ready</span>}
                                {doneCount > 0 && <span className={styles.statDone}>✓ {doneCount} imported</span>}
                                {errorCount > 0 && <span className={styles.statError}>✗ {errorCount} failed</span>}
                                {duplicateCount > 0 && <span className={styles.statDuplicate}>⚠ {duplicateCount} duplicates</span>}
                                {skippedCount > 0 && <span className={styles.statSkipped}>⊘ {skippedCount} skipped</span>}
                            </>
                        )}
                    </div>
                    <div className={styles.footerActions}>
                        <button className={styles.cancelBtn} onClick={onClose}>
                            Cancel
                        </button>
                        <button 
                            className={styles.processBtn}
                            onClick={processFiles}
                            disabled={pendingCount === 0 || isProcessing}
                        >
                            {isProcessing ? (
                                <>
                                    <Loader2 size={16} className={styles.spinner} />
                                    Processing...
                                </>
                            ) : (
                                <>
                                    <Upload size={16} />
                                    Import {pendingCount} Files
                                </>
                            )}
                        </button>
                    </div>
                </div>

                <input
                    ref={fileInputRef}
                    type="file"
                    multiple
                    accept={VIDEO_EXTENSIONS.map(e => `.${e}`).join(',')}
                    style={{ display: 'none' }}
                    onChange={handleHtmlFileSelect}
                />
            </div>
        </div>
    );
}

// Helper function to format file size
function formatFileSize(bytes: number): string {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${parseFloat((bytes / Math.pow(k, i)).toFixed(1))} ${sizes[i]}`;
}
