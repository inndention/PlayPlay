import React, { useState, useRef, useEffect, useCallback, useMemo } from 'react';

import { useSafeAsync } from '../hooks/useSafeAsync';

import { MediaItem, MediaType, AdClient, Campaign, AdMetadata, BumperSubtype, Segment, MediaLibraryItemClickOptions } from "../lib/types";

import { loadClients, saveClients, loadLocalMedia, saveLocalMedia, loadLibraryMedia, saveLibraryMedia, loadCampaigns } from "../lib/storage";

import { dbFetchMedia, dbSaveMedia, dbDeleteMedia } from "../lib/mediaDbClient";

import { generateThumbnail } from "../lib/mediaUtils";

import { formatDuration, cn } from "../lib/utils";

import { storeFileInIndexedDB, getFileFromIndexedDB, deleteFileFromIndexedDB } from "../lib/browserFileStorage";

import { resolveFileUrl, resolveFileUrlSync, getEnvironment, generateThumbnail as generateThumbFromResolver } from "../lib/fileResolver";

import { autoDetectMediaMetadata, suggestTypeFromDuration } from "../lib/mediaDetection";

import { CommandsPanel, CommandItem } from "./CommandsPanel";

import {

    Film, Image as ImageIcon, Upload, Library, HardDrive, Edit2, X, Check,

    Filter, Sparkles, Calendar, Clock, Scissors, Grid, List, LayoutList,

    SortAsc, SortDesc, ChevronDown, Play, Star, Hash, FileText, Trash2,

    Archive, Eye, EyeOff, CheckSquare, Square, BarChart2, FolderOpen,

    Tv, Radio, Megaphone, RefreshCw, AlertTriangle, Plus, FileInput, Terminal,

    Activity, Folder, AudioWaveform, Heart, Info, Snowflake

} from "lucide-react";

import styles from "./MediaLibrary.module.css";

import { SegmentEditor } from "./SegmentEditor";

import { BulkMediaImporter } from "./BulkMediaImporter";

import { VideoScrubber } from "./VideoScrubber";

import { MediaFavorites } from "./MediaFavorites";

import { EnhancedMetadataPanel } from "./EnhancedMetadataPanel";

import { MediaAnalyzer } from "./MediaAnalyzer";

import { AssetInspector } from "./AssetInspector";

import { SmartCollections } from "./SmartCollections";

import { WatchFolders } from "./WatchFolders";

import { MediaStatsDashboard } from "./MediaStatsDashboard";

import { InputManager } from "./InputManager";

import type { LiveSourceConfig, LiveSourceKind } from "../lib/types";

import {

    getEffectiveLibraryPriorityScore,

    priorityDecayAnchorMs,

    getMediaBurnPreset,

    setMediaBurnPreset,

    type MediaBurnPreset,

} from "../lib/mediaPriority";



type ViewMode = 'grid' | 'list' | 'detailed';

type SortField = 'addedAt' | 'lastPlayedAt' | 'dateMade' | 'name' | 'priority' | 'libraryPriority' | 'duration' | 'type' | 'playCount';

type SortOrder = 'asc' | 'desc';

type MediaLibraryTab = 'library' | 'feeds' | 'local' | 'commands' | 'assets' | 'favorites' | 'analysis' | 'collections' | 'watchfolders' | 'stats';



const ALL_MEDIA_TYPES: MediaType[] = ['program', 'ad', 'station_id', 'bumper', 'promo', 'paid_program', 'video', 'image'];



// Storage keys for persistent data

const STORAGE_KEYS = {

    FAVORITES: 'playplay_media_favorites',

    RECENT: 'playplay_media_recent'

};



interface MediaLibraryProps {

    onItemClick?: (item: MediaItem, options?: MediaLibraryItemClickOptions) => void;

    onItemContextMenu?: (item: MediaItem, x: number, y: number) => void;

    guestMode?: boolean;

    preferredViewMode?: ViewMode; // Optional default view for specific screens

    isPlaying?: boolean; // Playout active status for close confirmations

}





export function MediaLibrary({ onItemClick, onItemContextMenu, guestMode = false, preferredViewMode, isPlaying = false }: MediaLibraryProps) {

    const { safeSetState } = useSafeAsync();

    const [activeTab, setActiveTab] = useState<MediaLibraryTab>('library');

    const [localMedia, setLocalMedia] = useState<MediaItem[]>(() => loadLocalMedia());

    const [editingItem, setEditingItem] = useState<MediaItem | null>(null);

    const [selectedTypes, setSelectedTypes] = useState<Set<MediaType>>(new Set(ALL_MEDIA_TYPES));

    const [showArchived, setShowArchived] = useState(false);

    // Extended filter set

    const [missingFilter, setMissingFilter] = useState<'all' | 'playable' | 'missing'>('all');

    const [playFilter, setPlayFilter] = useState<'all' | 'never' | 'recent' | 'stale'>('all');

    const [staleDays, setStaleDays] = useState<number>(30);

    const [durationMin, setDurationMin] = useState<number | null>(null);   // seconds

    const [durationMax, setDurationMax] = useState<number | null>(null);   // seconds

    const [showFilters, setShowFilters] = useState(false);

    const [showPurgeMenu, setShowPurgeMenu] = useState(false);

    const [showTypeFilter, setShowTypeFilter] = useState(false);

    const [viewMode, setViewMode] = useState<ViewMode>(preferredViewMode || 'grid');

    const [sortField, setSortField] = useState<SortField>('addedAt');

    const [sortOrder, setSortOrder] = useState<SortOrder>('desc');

    const [showContextMenu, setShowContextMenu] = useState(false);

    const [contextMenuPos, setContextMenuPos] = useState({ x: 0, y: 0 });

    const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set());

    const [showBulkImporter, setShowBulkImporter] = useState(false);

    const [showLiveCreator, setShowLiveCreator] = useState(false);

    const [rebuildingThumbs, setRebuildingThumbs] = useState(false);

    const [missingFiles, setMissingFiles] = useState<Set<string>>(new Set());

    const [showBulkTypeChange, setShowBulkTypeChange] = useState(false);

    const [showMasterPlayImporter, setShowMasterPlayImporter] = useState(false);

    const [showRecentOnly, setShowRecentOnly] = useState(false);

    const [burnPreset, setBurnPresetState] = useState<MediaBurnPreset>(() => getMediaBurnPreset());



    // Enhanced MediaLibrary state

    const [favorites, setFavorites] = useState<Set<string>>(() => {

        try {

            const saved = localStorage.getItem(STORAGE_KEYS.FAVORITES);

            return saved ? new Set(JSON.parse(saved)) : new Set();

        } catch { return new Set(); }

    });

    const [recentlyAccessed, setRecentlyAccessed] = useState<string[]>(() => {

        try {

            const saved = localStorage.getItem(STORAGE_KEYS.RECENT);

            return saved ? JSON.parse(saved) : [];

        } catch { return []; }

    });

    const [scrubberItem, setScrubberItem] = useState<MediaItem | null>(null);

    const [metadataItem, setMetadataItem] = useState<MediaItem | null>(null);

    const [analysisItem, setAnalysisItem] = useState<MediaItem | null>(null);

    

    const fileInputRef = useRef<HTMLInputElement>(null);

    const masterPlayInputRef = useRef<HTMLInputElement>(null);

    const contextMenuRef = useRef<HTMLDivElement>(null);

    const typeFilterRef = useRef<HTMLDivElement>(null);

    // Tracks which IDs were last persisted to the DB so the reconcile effect

    // can DELETE items that disappeared from libraryMedia — fixes the bug

    // where deleted items reappeared on reload.

    const dbKnownIdsRef = useRef<Set<string>>(new Set());

    

    // Persist favorites and recent

    useEffect(() => {

        localStorage.setItem(STORAGE_KEYS.FAVORITES, JSON.stringify([...favorites]));

    }, [favorites]);

    

    useEffect(() => {

        localStorage.setItem(STORAGE_KEYS.RECENT, JSON.stringify(recentlyAccessed));

    }, [recentlyAccessed]);

    

    // Favorites management

    const toggleFavorite = useCallback((itemId: string) => {

        setFavorites(prev => {

            const next = new Set(prev);

            if (next.has(itemId)) {

                next.delete(itemId);

            } else {

                next.add(itemId);

            }

            return next;

        });

    }, []);

    

    // Track recently accessed items

    const trackRecentAccess = useCallback((itemId: string) => {

        setRecentlyAccessed(prev => {

            const filtered = prev.filter(id => id !== itemId);

            return [itemId, ...filtered].slice(0, 50);

        });

    }, []);

    

    const clearRecentlyAccessed = useCallback(() => {

        setRecentlyAccessed([]);

    }, []);

    

    // Open video scrubber

    const openVideoScrubber = useCallback((item: MediaItem) => {

        if (item.type === 'video' || item.type === 'program' || item.type === 'ad' || item.type === 'promo') {

            setScrubberItem(item);

        }

    }, []);

    

    // Handle segment creation from video scrubber — update both segments (legacy) and segmentCollections

    const handleCreateSegment = useCallback((mediaId: string, segment: Segment) => {

        const updateMediaWithSegment = (media: MediaItem[]) => {

            return media.map(item => {

                if (item.id !== mediaId) return item;

                const newSegments = [...(item.segments || []), segment];

                // Keep segmentCollections in sync so SegmentEditor and playlist see the segment

                if (item.segmentCollections && item.segmentCollections.length > 0) {

                    const activeId = item.activeCollectionId || item.segmentCollections.find(c => c.isActive)?.id || item.segmentCollections[0].id;

                    const collections = item.segmentCollections.map(c =>

                        c.id === activeId

                            ? { ...c, segments: [...c.segments, segment] }

                            : c

                    );

                    return { ...item, segments: newSegments, segmentCollections: collections };

                }

                const defaultCollection = {

                    id: `segcol_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`,

                    name: 'Default',

                    segments: newSegments,

                    isActive: true,

                };

                return {

                    ...item,

                    segments: newSegments,

                    segmentCollections: [defaultCollection],

                    activeCollectionId: defaultCollection.id,

                };

            });

        };

        setLocalMedia(prev => updateMediaWithSegment(prev));

        setLibraryMedia(prev => updateMediaWithSegment(prev));

    }, []);

    

    // Save marks from video scrubber

    const handleSaveMarks = useCallback((mediaId: string, markIn: number | null, markOut: number | null) => {

        const updateMediaWithMarks = (media: MediaItem[]) => {

            return media.map(item => {

                if (item.id === mediaId) {

                    return { ...item, markIn: markIn ?? undefined, markOut: markOut ?? undefined };

                }

                return item;

            });

        };

        

        setLocalMedia(prev => updateMediaWithMarks(prev));

        setLibraryMedia(prev => updateMediaWithMarks(prev));

    }, []);



    // Align view mode to caller preference when it changes (e.g., scheduler wants list view)

    useEffect(() => {

        if (preferredViewMode) {

            setViewMode(preferredViewMode);

        }

    }, [preferredViewMode]);



    // Load clients for auto-detection

    const [clients, setClients] = useState<AdClient[]>(() => loadClients());



    const [libraryMedia, setLibraryMedia] = useState<MediaItem[]>(() => loadLibraryMedia([]));

    const [searchQuery, setSearchQuery] = useState('');



    // Close context menu and type filter when clicking outside

    useEffect(() => {

        const handleClickOutside = (e: MouseEvent) => {

            if (contextMenuRef.current && !contextMenuRef.current.contains(e.target as Node)) {

                setShowContextMenu(false);

            }

            if (typeFilterRef.current && !typeFilterRef.current.contains(e.target as Node)) {

                setShowTypeFilter(false);

            }

        };

        document.addEventListener('mousedown', handleClickOutside);

        return () => document.removeEventListener('mousedown', handleClickOutside);

    }, []);



    // Load saved local media on mount (rebuild file:// URLs from filePath or IndexedDB)

    useEffect(() => {

        let cancelled = false;

        const loadData = () => {

            const saved = loadLocalMedia();

            const env = getEnvironment();

            if (saved.length > 0) {

                (async () => {

                    const restored = await Promise.all(

                        saved.map(async (item) => {

                            // Electron mode: rebuild URL via universal resolver

                            if (item.filePath && env.isElectron) {

                                const src = await resolveFileUrl(item.filePath, 'player');

                                return {

                                    ...item,

                                    src,

                                };

                            }

                            // Browser mode: restore from IndexedDB if stored

                            if (item.storedFileId && env.isBrowser) {

                                try {

                                    const blobUrl = await getFileFromIndexedDB(item.storedFileId);

                                    if (blobUrl) {

                                        return {

                                            ...item,

                                            src: blobUrl,

                                        };

                                    }

                                } catch (err) {

                                    console.warn('[MediaLibrary] Failed to restore file from IndexedDB:', item.storedFileId, err);

                                }

                            }

                            return item;

                        })

                    );

                    if (!cancelled) safeSetState(() => setLocalMedia(restored));

                })();

            }

        };



        loadData();



        const handleRefresh = () => {

            console.log('[MediaLibrary] Refreshing data...');

            loadData();

            // Also refresh library media from DB

            (async () => {

                const dbMedia = await dbFetchMedia();

                if (dbMedia.length > 0) {

                    safeSetState(() => setLibraryMedia(dbMedia));

                }

            })();

        };



        window.addEventListener('media-library-update', handleRefresh);

        return () => {

            cancelled = true;

            window.removeEventListener('media-library-update', handleRefresh);

        };

    }, [safeSetState]);



    // Save local media whenever it changes (including when emptied)

    useEffect(() => {

        saveLocalMedia(localMedia);

    }, [localMedia]);



    // Load library media from DB (Electron) or fallback to localStorage defaults

    useEffect(() => {

        let cancelled = false;

        (async () => {

            const env = getEnvironment();

            const dbMedia = await dbFetchMedia();

            if (cancelled) return;

            if (dbMedia.length > 0) {

                // Rebuild URLs from filePath via universal resolver so library stays valid across sessions

                const restored = await Promise.all(dbMedia.map(async item => {

                    // Ensure filename is set

                    const filename = item.filename ||

                        (item.filePath ? item.filePath.split(/[\\/]/).pop() : null) ||

                        item.title ||

                        '';



                    if (item.filePath && env.isElectron) {

                        const src = await resolveFileUrl(item.filePath, 'player');

                        return {

                            ...item,

                            filename,

                            src,

                        };

                    }

                    return { ...item, filename };

                }));

                if (!cancelled) {

                    setLibraryMedia(restored);

                    // Seed the DB-known-ids ref so the reconcile effect doesn't

                    // think these are "new" deletions on its first run.

                    dbKnownIdsRef.current = new Set(restored.map(i => i.id));

                }

            } else {

                // Seed DB with current library (MOCK or stored)

                const seed = loadLibraryMedia([]);

                if (!cancelled) setLibraryMedia(seed);

                for (const item of seed) {

                    if (cancelled) return;

                    await dbSaveMedia(item);

                }

                if (!cancelled) dbKnownIdsRef.current = new Set(seed.map(i => i.id));

            }

        })();

        return () => { cancelled = true; };

    }, []);



    // Missing file detection (Electron only)

    useEffect(() => {

        let cancelled = false;

        (async () => {

            const env = getEnvironment();

            const api = (window as any).electronAPI;

            if (!env.isElectron || !api?.pathExists) return;

            const checks = await Promise.all(

                libraryMedia.map(async (m) => {

                    if (!m.filePath) return { id: m.id, exists: true };

                    const res = await api.pathExists(m.filePath);

                    return { id: m.id, exists: !!res?.exists };

                })

            );

            if (cancelled) return;

            const missing = new Set<string>(checks.filter(c => !c.exists).map(c => c.id));

            safeSetState(() => setMissingFiles(missing));

        })();

        return () => { cancelled = true; };

    }, [libraryMedia, safeSetState]);



    // Persist library media edits between sessions (localStorage + DB).

    // CRITICAL: must save even when libraryMedia is empty, otherwise deleting

    // the last item leaves stale localStorage and the items reappear on reload.

    // Also: must DELETE items from DB that are no longer in the array, not

    // just upsert what's still there — otherwise removed items resurrect.

    useEffect(() => {

        if (!libraryMedia) return;

        saveLibraryMedia(libraryMedia);



        // Reconcile DB with the current array (upsert present, delete absent)

        (async () => {

            const presentIds = new Set(libraryMedia.map(i => i.id));

            for (const item of libraryMedia) {

                await dbSaveMedia(item);

            }

            // Delete anything that was in the DB last cycle but is gone now

            for (const id of dbKnownIdsRef.current) {

                if (!presentIds.has(id)) {

                    await dbDeleteMedia(id);

                }

            }

            dbKnownIdsRef.current = presentIds;

        })();

    }, [libraryMedia]);



    // Auto-copy file to CasperCG media folder on import

    const autoCopyToCasperCG = async (filePath: string): Promise<string | null> => {

        const electronAPI = (window as any).electronAPI;

        if (!electronAPI?.copyFilesToCasperCG) return null;



        try {

            const result = await electronAPI.copyFilesToCasperCG([filePath]);

            if (result.success && result.results[0]?.success) {

                console.log('[MediaLibrary] Auto-copied to CasperCG:', result.results[0].destPath);

                return result.results[0].destPath;

            }

        } catch (error) {

            console.warn('[MediaLibrary] Failed to auto-copy to CasperCG:', error);

        }

        return null;

    };



    // Process file from Electron file path (has full path)

    // Uses the universal file resolver for cross-platform compatibility

    const processFileFromPath = async (filePath: string) => {

        const env = getEnvironment();

        if (!env.isElectron) {

            console.error('[MediaLibrary] processFileFromPath called in browser mode - use processFile with File objects instead.');

            return;

        }



        // Auto-copy to CasperCG media folder

        autoCopyToCasperCG(filePath);



        // Extract filename and extension from path (works in both Windows and Unix)

        const filename = filePath.split(/[\\/]/).pop() || 'unknown';

        const ext = filename.includes('.')

            ? filename.substring(filename.lastIndexOf('.')).toLowerCase()

            : '';

        const isImage = ['.jpg', '.jpeg', '.png', '.gif', '.webp'].includes(ext);



        // Use the universal file resolver to get a playable URL

        const fileUrl = await resolveFileUrl(filePath, 'player');

        console.log('[MediaLibrary] File URL resolved:', fileUrl, 'from path:', filePath);



        if (!fileUrl) {

            console.error('[MediaLibrary] Could not resolve file URL for:', filePath);

            return;

        }



        const now = new Date().toISOString();



        if (isImage) {

            const img = new Image();

            img.onload = () => {

                const canvas = document.createElement('canvas');

                const maxSize = 320;

                let width = img.width;

                let height = img.height;



                if (width > maxSize || height > maxSize) {

                    const ratio = Math.min(maxSize / width, maxSize / height);

                    width = width * ratio;

                    height = height * ratio;

                }



                canvas.width = width;

                canvas.height = height;

                const ctx = canvas.getContext('2d');

                if (ctx) {

                    ctx.drawImage(img, 0, 0, width, height);

                    const thumb = canvas.toDataURL('image/jpeg', 0.7);

                    const newItem: MediaItem = {

                        id: `local_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,

                        title: filename,

                        duration: 0,

                        type: 'image',

                        src: fileUrl,

                        filePath: filePath,

                        filename: filename,

                        thumbnail: thumb,

                        segments: [],

                        addedAt: now,

                        dateMade: now,

                        priority: 0,

                        playCount: 0,

                    };

                    setLocalMedia(prev => {

                        const updated = [...prev, newItem];

                        saveLocalMedia(updated);

                        // Also save to DB immediately

                        dbSaveMedia(newItem).catch(console.error);

                        return updated;

                    });

                    // Switch to local tab to show the imported file

                    setActiveTab('local');

                }

            };

            img.onerror = () => {

                console.error('Failed to load image:', filePath);

            };

            img.src = fileUrl;

            return;

        }



        // Video file

        return new Promise<void>((resolve) => {

            const video = document.createElement('video');

            video.preload = 'metadata';

            video.onloadedmetadata = async () => {

                try {

                    // Generate thumbnail from video

                    const thumb = await generateThumbnailFromPath(filePath, video.duration);



                    // Auto-detect type and client based on filename and duration

                    const detected = autoDetectMediaMetadata(filename, video.duration, clients);



                    const newItem: MediaItem = {

                        id: `local_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,

                        title: detected.title || filename,

                        duration: video.duration,

                        type: detected.type,

                        src: fileUrl,

                        filePath: filePath,

                        filename: filename,

                        thumbnail: thumb || '#555',

                        segments: [],

                        addedAt: now,

                        dateMade: now,

                        episodeDate: detected.episodeDate,

                        client: detected.client,

                        bumperSubtype: detected.bumperSubtype,

                        priority: 0,

                        playCount: 0,

                    };

                    setLocalMedia(prev => {

                        const updated = [...prev, newItem];

                        saveLocalMedia(updated);

                        // Also save to DB immediately

                        dbSaveMedia(newItem).catch(console.error);

                        return updated;

                    });

                    // Switch to local tab to show the imported file

                    setActiveTab('local');



                    // Trigger background ffmpeg analysis (transcript + content index)

                    const electronAPI = (window as any).electronAPI;

                    if (electronAPI?.analyzeAsset) {

                        try {

                            const queued = await electronAPI.analyzeAsset(filePath, { transcribe: true });

                            console.log('[MediaLibrary] Queued ffmpeg analysis:', filePath, queued);

                            if (queued?.ok && queued?.data?.status !== 'cached') {

                                // Poll for results and update item when ready

                                const pollForAnalysis = async () => {

                                    const MAX_POLL = 60; // 5 minutes max (60 * 5s)

                                    for (let i = 0; i < MAX_POLL; i++) {

                                        await new Promise(r => setTimeout(r, 5000));

                                        try {

                                            const res = await electronAPI.analyzeGetResult(filePath);

                                            if (res?.ok && res?.data) {

                                                const d = res.data;

                                                const updates: Partial<MediaItem> = {};



                                                // Transcript

                                                if (d.audio?.transcript) {

                                                    updates.transcriptRecords = [{

                                                        id: `stt_${newItem.id}`,

                                                        provider: 'local-whisper',

                                                        transcript: d.audio.transcript,

                                                        language: d.audio.language || 'en',

                                                        confidence: 0.85,

                                                        startedAt: Date.now(),

                                                        source: 'import',

                                                    }];

                                                }



                                                // Content index / intent

                                                if (d.context) {

                                                    const ctx = d.context;

                                                    updates.contentIndex = {

                                                        summary: ctx.summary || '',

                                                        topics: ctx.topics || [],

                                                        people: ctx.people || [],

                                                        places: ctx.places || [],

                                                        clients: ctx.clients || [],

                                                        events: ctx.events || [],

                                                        sponsorCandidates: ctx.sponsorCandidates || [],

                                                        programCandidates: ctx.programCandidates || [],

                                                        contextTags: [

                                                            ...(ctx.contentType ? [ctx.contentType] : []),

                                                            ...(ctx.riskFlags || []),

                                                        ],

                                                        lastIndexedAt: d.analyzedAt || new Date().toISOString(),

                                                    };

                                                }



                                                // Update localMedia and DB

                                                if (Object.keys(updates).length > 0) {

                                                    setLocalMedia(prev => {

                                                        const idx = prev.findIndex(m => m.id === newItem.id);

                                                        if (idx === -1) return prev;

                                                        const updated = [...prev];

                                                        updated[idx] = { ...updated[idx], ...updates };

                                                        saveLocalMedia(updated);

                                                        dbSaveMedia(updated[idx]).catch(console.error);

                                                        return updated;

                                                    });

                                                    console.log('[MediaLibrary] Updated item with analysis:', newItem.id, Object.keys(updates));

                                                }

                                                break;

                                            }

                                        } catch (pollErr) {

                                            console.warn('[MediaLibrary] Analysis poll error:', pollErr);

                                        }

                                    }

                                };

                                pollForAnalysis().catch(console.error);

                            }

                        } catch (err) {

                            console.warn('[MediaLibrary] Failed to queue analysis:', err);

                        }

                    }



                    resolve();

                } catch (err) {

                    console.error('Failed to process video:', filePath, err);

                    resolve();

                }

            };

            video.onerror = () => {

                console.error('Failed to load video:', filePath);

                resolve();

            };

            video.src = fileUrl;

        });

    };



    // Helper to generate thumbnail from file path using universal resolver

    const generateThumbnailFromPath = async (filePath: string, duration: number): Promise<string | null> => {

        // Use the universal file resolver

        const fileUrl = await resolveFileUrl(filePath, 'thumbnail');

        if (!fileUrl) return null;



        return new Promise((resolve) => {

            const video = document.createElement('video');

            video.preload = 'metadata';

            video.crossOrigin = 'anonymous'; // Allow CORS for canvas capture

            video.onloadedmetadata = () => {

                const canvas = document.createElement('canvas');

                canvas.width = 320;

                canvas.height = 180;

                const ctx = canvas.getContext('2d');

                if (ctx) {

                    video.currentTime = Math.min(duration * 0.1, 5);

                    video.onseeked = () => {

                        ctx.drawImage(video, 0, 0, canvas.width, canvas.height);

                        const thumb = canvas.toDataURL('image/jpeg', 0.7);

                        resolve(thumb);

                    };

                } else {

                    resolve(null);

                }

            };

            video.onerror = () => resolve(null);

            video.src = fileUrl;

        });

    };



    const handleRebuildThumbnails = async () => {

        setRebuildingThumbs(true);

        try {

            const env = getEnvironment();

            const updatedLibrary = await Promise.all(libraryMedia.map(async (m) => {

                if (!m.filePath) return m;

                const thumb = m.type === 'image'

                    ? await generateThumbFromResolver(m.filePath)

                    : await generateThumbnailFromPath(m.filePath, m.duration || 0);

                const next = { ...m, thumbnail: thumb || m.thumbnail || '#555' };

                if (env.isElectron) dbSaveMedia(next).catch(console.error);

                return next;

            }));

            setLibraryMedia(updatedLibrary);



            // Also rebuild local tab items (Electron: filePath-based)

            const updatedLocal = await Promise.all(localMedia.map(async (m) => {

                if (!m.filePath) return m;

                const thumb = m.type === 'image'

                    ? await generateThumbFromResolver(m.filePath)

                    : await generateThumbnailFromPath(m.filePath, m.duration || 0);

                const next = { ...m, thumbnail: thumb || m.thumbnail || '#555' };

                if (env.isElectron) dbSaveMedia(next).catch(console.error);

                return next;

            }));

            setLocalMedia(updatedLocal);

            saveLocalMedia(updatedLocal);

        } finally {

            setRebuildingThumbs(false);

        }

    };



    // Process file from File object (HTML input or drag-drop) - may not have path

    // Handles both Electron (with filePath) and browser mode (with IndexedDB)

    const processFile = async (file: File) => {

        const isElectron = typeof window !== 'undefined' && (window as any).electronAPI;



        // In Electron, File objects from HTML input don't have .path

        // Try to get it, but it may be undefined

        const filePath: string | undefined = (file as any).path;



        // If we have filePath AND we're in Electron, use processFileFromPath

        // In browser mode, file:// URLs don't work, so always use blob URLs

        if (filePath && isElectron) {

            await processFileFromPath(filePath);

            return;

        }



        // Browser/npm mode: Store file and create persistent reference

        // (If we reach here, we're either in browser mode or Electron without filePath)

        {

            // Browser mode - process file with IndexedDB storage

            const now = new Date().toISOString();

            const filename = file.name;

            const fileId = `browser_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;



            // Create blob URL from the file for immediate use

            const blobUrl = URL.createObjectURL(file);



            // Store file in IndexedDB (if available)

            try {

                await storeFileInIndexedDB(file, fileId);

                console.log('File stored in IndexedDB:', fileId, filename);

            } catch (err) {

                console.warn('IndexedDB storage not available, using session-only blob URL:', err);

                // Continue with blob URL - it will work for this session

            }



            // Determine file type - use MediaType enum values

            let type: MediaType = 'program'; // Default to 'program' for videos

            if (file.type.startsWith('image/')) {

                type = 'image';

            } else if (file.type.startsWith('video/')) {

                type = 'program'; // Videos default to 'program' type

            }



            // Get duration and thumbnail

            let duration = 0;

            let thumbnail: string | null = null;



            if (type === 'program' || file.type.startsWith('video/')) {

                try {

                    const video = document.createElement('video');

                    video.preload = 'metadata';

                    video.src = blobUrl;



                    await new Promise((resolve, reject) => {

                        video.onloadedmetadata = () => {

                            duration = video.duration;

                            const canvas = document.createElement('canvas');

                            canvas.width = 320;

                            canvas.height = 180;

                            const ctx = canvas.getContext('2d');

                            if (ctx) {

                                video.currentTime = Math.min(duration * 0.1, 5);

                                video.onseeked = () => {

                                    ctx.drawImage(video, 0, 0, canvas.width, canvas.height);

                                    thumbnail = canvas.toDataURL('image/jpeg', 0.7);

                                    resolve(null);

                                };

                            } else {

                                resolve(null);

                            }

                        };

                        video.onerror = reject;

                    });

                } catch (err) {

                    console.error('Failed to process video metadata:', err);

                }

            } else {

                // For images, create thumbnail

                try {

                    const img = new Image();

                    img.src = blobUrl;

                    await new Promise((resolve, reject) => {

                        img.onload = () => {

                            const canvas = document.createElement('canvas');

                            canvas.width = 320;

                            canvas.height = 180;

                            const ctx = canvas.getContext('2d');

                            if (ctx) {

                                const imgAspect = img.width / img.height;

                                const canvasAspect = canvas.width / canvas.height;

                                let drawWidth = canvas.width;

                                let drawHeight = canvas.height;

                                let offsetX = 0;

                                let offsetY = 0;



                                if (imgAspect > canvasAspect) {

                                    drawHeight = canvas.width / imgAspect;

                                    offsetY = (canvas.height - drawHeight) / 2;

                                } else {

                                    drawWidth = canvas.height * imgAspect;

                                    offsetX = (canvas.width - drawWidth) / 2;

                                }



                                ctx.fillStyle = '#000';

                                ctx.fillRect(0, 0, canvas.width, canvas.height);

                                ctx.drawImage(img, offsetX, offsetY, drawWidth, drawHeight);

                                thumbnail = canvas.toDataURL('image/jpeg', 0.7);

                            }

                            resolve(null);

                        };

                        img.onerror = reject;

                    });

                } catch (err) {

                    console.error('Failed to process image:', err);

                }

            }



            const newItem: MediaItem = {

                id: fileId,

                title: filename.replace(/\.[^/.]+$/, ''),

                type: type,

                src: blobUrl,

                filename: filename,

                duration: duration,

                thumbnail: thumbnail || '#555',

                segments: [],

                addedAt: now,

                dateMade: now,

                priority: 0,

                playCount: 0,

                storedFileId: fileId, // Reference to IndexedDB if stored

            };



            // Update state and save using functional update to ensure consistency

            setLocalMedia(prev => {

                const updated = [...prev, newItem];

                // Save to localStorage with the updated array

                saveLocalMedia(updated);

                // Also save to DB if available

                if ((window as any).electronAPI) {

                    dbSaveMedia(newItem).catch(console.error);

                }

                console.log('Added file to localMedia:', {

                    id: newItem.id,

                    title: newItem.title,

                    type: newItem.type,

                    filename: newItem.filename,

                    totalItems: updated.length

                });

                return updated;

            });



            // Switch to local tab to show the imported file

            setActiveTab('local');



            console.log('Imported file:', filename, newItem);

            return;

        }



        // Electron mode but no path - shouldn't happen with file dialog

        console.warn('File imported without path in Electron - cannot persist.');

        alert('Files imported via drag-drop may not have full paths. Please use the "Import Files" button to select files properly.');

    };



    const handleFileSelect = async (e: React.ChangeEvent<HTMLInputElement>) => {

        if (e.target.files && e.target.files.length > 0) {

            const files = Array.from(e.target.files);

            console.log('handleFileSelect: Processing', files.length, 'file(s)');

            for (const file of files) {

                try {

                    await processFile(file);

                } catch (err) {

                    console.error('Error processing file:', file.name, err);

                    alert(`Failed to import ${file.name}: ${err instanceof Error ? err.message : String(err)}`);

                }

            }

            e.target.value = '';

            console.log('handleFileSelect: Finished processing files');

        }

    };



    const handleImportClick = async (e: React.MouseEvent) => {

        e.preventDefault();

        e.stopPropagation();



        // Use Electron's native file dialog when available

        const w = typeof window !== 'undefined' ? (window as any) : null;

        if (w?.electronAPI?.openFileDialog) {

            try {

                const result = await w.electronAPI.openFileDialog({

                    properties: ['openFile', 'multiSelections'],

                    filters: [

                        { name: 'Media Files', extensions: ['mp4', 'webm', 'mov', 'avi', 'mkv', 'jpg', 'jpeg', 'png', 'gif', 'webp'] },

                        { name: 'Video', extensions: ['mp4', 'webm', 'mov', 'avi', 'mkv'] },

                        { name: 'Images', extensions: ['jpg', 'jpeg', 'png', 'gif', 'webp'] },

                        { name: 'All Files', extensions: ['*'] }

                    ]

                });



                if (!result.canceled && result.filePaths && result.filePaths.length > 0) {

                    // Process files from Electron dialog - we have full paths

                    for (const filePath of result.filePaths) {

                        await processFileFromPath(filePath);

                    }

                }

            } catch (err) {

                console.error('Failed to open file dialog:', err);

                // Fallback to HTML input

                if (fileInputRef.current) {

                    fileInputRef.current.click();

                }

            }

        } else {

            // Fallback to HTML file input for web

            if (fileInputRef.current) {

                fileInputRef.current.click();

            }

        }

    };



    const handleCreateLiveSource = async (item: MediaItem) => {

        // Live sources are stored in libraryMedia + DB (Electron) for persistence

        setLibraryMedia(prev => [item, ...prev]);

        await dbSaveMedia(item).catch(console.error);

        setShowLiveCreator(false);

        setActiveTab('library');

    };



    const handleSaveEdit = async (updated: MediaItem) => {

        if (updated.id.startsWith('local_')) {

            setLocalMedia(prev => prev.map(i => i.id === updated.id ? updated : i));

        } else {

            setLibraryMedia(prev => prev.map(i => i.id === updated.id ? updated : i));

            await dbSaveMedia(updated);

        }

        setEditingItem(null);

    };



    const handleDeleteItem = async (id: string) => {

        // Find the item to check if it has storedFileId

        const allMedia = [...localMedia, ...libraryMedia];

        const itemToDelete = allMedia.find(i => i.id === id);



        // Clean up IndexedDB if this is a browser-stored file

        if (itemToDelete?.storedFileId && !(window as any).electronAPI) {

            try {

                await deleteFileFromIndexedDB(itemToDelete.storedFileId);

                console.log('Deleted file from IndexedDB:', itemToDelete.storedFileId);

            } catch (err) {

                console.warn('Failed to delete file from IndexedDB:', err);

            }

        }



        if (id.startsWith('local_') || id.startsWith('browser_')) {

            setLocalMedia(prev => prev.filter(i => i.id !== id));

        } else {

            setLibraryMedia(prev => prev.filter(i => i.id !== id));

            await dbDeleteMedia(id);

        }

        setEditingItem(null);

    };



    const handleDeleteSelected = async () => {

        if (selectedItems.size === 0) return;

        if (!confirm(`Delete ${selectedItems.size} selected item(s)?`)) return;



        // Clean up IndexedDB for browser-stored files

        const allMedia = [...localMedia, ...libraryMedia];

        const itemsToDelete = allMedia.filter(i => selectedItems.has(i.id));



        if (!(window as any).electronAPI) {

            // Browser mode - clean up IndexedDB

            for (const item of itemsToDelete) {

                if (item.storedFileId) {

                    try {

                        await deleteFileFromIndexedDB(item.storedFileId);

                    } catch (err) {

                        console.warn('Failed to delete file from IndexedDB:', item.storedFileId, err);

                    }

                }

            }

        }



        // Also delete from DB for library items

        for (const item of itemsToDelete) {

            if (!item.id.startsWith('local_') && !item.id.startsWith('browser_')) {

                try {

                    await dbDeleteMedia(item.id);

                } catch (err) {

                    console.warn('Failed to delete from DB:', item.id, err);

                }

            }

        }



        setLocalMedia(prev => prev.filter(i => !selectedItems.has(i.id)));

        setLibraryMedia(prev => prev.filter(i => !selectedItems.has(i.id)));

        setSelectedItems(new Set());

    };



    /**

     * Purge by predicate — removes from BOTH localStorage and DB so items can't

     * resurrect, plus IndexedDB cleanup for browser-stored files.

     */

    const purgeBy = async (

        scope: 'library' | 'local' | 'both',

        predicate: (m: MediaItem) => boolean,

        confirmLabel: string,

    ) => {

        const fromLib = (scope !== 'local') ? libraryMedia.filter(predicate) : [];

        const fromLocal = (scope !== 'library') ? localMedia.filter(predicate) : [];

        const total = fromLib.length + fromLocal.length;

        if (total === 0) {

            alert('Nothing matches — no items to purge.');

            return;

        }

        if (!confirm(`Purge ${total} item(s) — ${confirmLabel}?\nThis cannot be undone.`)) return;



        // IndexedDB cleanup for browser mode

        if (!(window as any).electronAPI) {

            for (const item of [...fromLib, ...fromLocal]) {

                if (item.storedFileId) {

                    try { await deleteFileFromIndexedDB(item.storedFileId); } catch { /* */ }

                }

            }

        }



        // DB cleanup for library items

        for (const item of fromLib) {

            try { await dbDeleteMedia(item.id); } catch { /* */ }

        }



        // Update React state — the existing reconcile effect handles localStorage

        if (fromLib.length) {

            const purgedLibIds = new Set(fromLib.map(i => i.id));

            setLibraryMedia(prev => prev.filter(i => !purgedLibIds.has(i.id)));

        }

        if (fromLocal.length) {

            const purgedLocalIds = new Set(fromLocal.map(i => i.id));

            setLocalMedia(prev => prev.filter(i => !purgedLocalIds.has(i.id)));

        }

        setShowPurgeMenu(false);

    };



    const purgeMissing      = () => purgeBy('both', m => missingFiles.has(m.id), 'files that no longer exist on disk');

    const purgeNeverPlayed  = () => purgeBy('both', m => ((m as any).playCount || 0) === 0, 'items never played');

    const purgeStale        = () => purgeBy('both', m => {

        const lp = (m as any).lastPlayedAt ? new Date((m as any).lastPlayedAt).getTime() : 0;

        const pc = (m as any).playCount || 0;

        return pc > 0 && lp < staleCutoff;

    }, `items not played in ${staleDays}+ days`);

    const purgeArchived     = () => purgeBy('both', m => !!m.archived, 'archived items');

    const purgeFiltered     = () => {

        const ids = new Set(filteredMedia.map(m => m.id));

        return purgeBy('both', m => ids.has(m.id), 'all items currently visible (matches the active filters)');

    };

    const purgeAll          = () => purgeBy('both', () => true, 'EVERY item in the library and local media');



    const purgeRowStyle: React.CSSProperties = {

        display: 'block', width: '100%', textAlign: 'left',

        padding: '8px 12px', background: 'transparent', color: 'var(--text)',

        border: 'none', borderRadius: 4, cursor: 'pointer', fontSize: 13,

    };



    const handleContextMenu = (e: React.MouseEvent) => {

        e.preventDefault();

        setContextMenuPos({ x: e.clientX, y: e.clientY });

        setShowContextMenu(true);

    };



    const handleSort = (field: SortField) => {

        if (sortField === field) {

            setSortOrder(sortOrder === 'asc' ? 'desc' : 'asc');

        } else {

            setSortField(field);

            setSortOrder(field === 'name' ? 'asc' : 'desc');

        }

        setShowContextMenu(false);

    };



    const decayAnchorMs = priorityDecayAnchorMs();



    const handleBurnPresetChange = (e: React.ChangeEvent<HTMLSelectElement>) => {

        const v = e.target.value as MediaBurnPreset;

        setMediaBurnPreset(v);

        setBurnPresetState(v);

    };



    // Type filter helpers

    const toggleType = (type: MediaType) => {

        const newSelected = new Set(selectedTypes);

        if (newSelected.has(type)) {

            newSelected.delete(type);

        } else {

            newSelected.add(type);

        }

        setSelectedTypes(newSelected);

    };



    const selectAllTypes = () => setSelectedTypes(new Set(ALL_MEDIA_TYPES));

    const deselectAllTypes = () => setSelectedTypes(new Set());

    const allTypesSelected = selectedTypes.size === ALL_MEDIA_TYPES.length;



    // Archive helpers

    const handleArchiveItem = (id: string) => {

        if (id.startsWith('local_')) {

            setLocalMedia(prev => prev.map(item =>

                item.id === id ? { ...item, archived: true } : item

            ));

        } else {

            setLibraryMedia(prev => prev.map(item =>

                item.id === id ? { ...item, archived: true } : item

            ));

        }

    };



    const handleUnarchiveItem = (id: string) => {

        if (id.startsWith('local_')) {

            setLocalMedia(prev => prev.map(item =>

                item.id === id ? { ...item, archived: false } : item

            ));

        } else {

            setLibraryMedia(prev => prev.map(item =>

                item.id === id ? { ...item, archived: false } : item

            ));

        }

    };



    // Bulk type change for selected items

    const handleBulkTypeChange = async (newType: MediaType, client?: string) => {

        const selectedIds = Array.from(selectedItems);

        if (selectedIds.length === 0) return;



        const updateItem = (item: MediaItem): MediaItem => {

            if (!selectedIds.includes(item.id)) return item;

            const updated: MediaItem = { ...item, type: newType };

            if (newType === 'ad' && client) {

                updated.client = client;

            }

            return updated;

        };



        // Update local media

        const updatedLocal = localMedia.map(updateItem);

        setLocalMedia(updatedLocal);

        saveLocalMedia(updatedLocal);



        // Update library media

        const updatedLibrary = libraryMedia.map(updateItem);

        setLibraryMedia(updatedLibrary);

        saveLibraryMedia(updatedLibrary);



        // Persist to DB

        const env = getEnvironment();

        if (env.isElectron) {

            for (const id of selectedIds) {

                const item = [...updatedLocal, ...updatedLibrary].find(i => i.id === id);

                if (item) {

                    dbSaveMedia(item).catch(console.error);

                }

            }

        }



        setSelectedItems(new Set());

        setShowBulkTypeChange(false);

    };



    // Create new client on-the-fly

    const handleCreateClient = (name: string): AdClient => {

        const newClient: AdClient = {

            id: `client_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,

            name: name.trim(),

            color: `#${Math.floor(Math.random() * 16777215).toString(16).padStart(6, '0')}`,

            rules: {

                allowedHours: [],

                allowedDays: [],

                maxPlaysPerHour: 0,

                maxPlaysPerDay: 0,

                minIntervalMinutes: 0,

                priority: 5,

            },

        };

        const updatedClients = [...clients, newClient];

        setClients(updatedClients);

        saveClients(updatedClients);

        return newClient;

    };



    // MasterPlay .in playlist importer

    const handleMasterPlayImport = async (file: File) => {

        try {

            const text = await file.text();

            const lines = text.split(/\r?\n/).filter(l => l.trim());



            const importedItems: MediaItem[] = [];

            const now = new Date().toISOString();



            for (const line of lines) {

                // MasterPlay format typically has full path and optionally timing info

                // Try to extract path - common formats:

                // C:\path\to\file.mp4

                // "C:\path\to\file.mp4" 00:00:30

                // or just filename with duration



                let filePath = '';

                let duration = 0;



                // Check for quoted path

                const quotedMatch = line.match(/"([^"]+)"/);

                if (quotedMatch) {

                    filePath = quotedMatch[1];

                    // Look for duration after the path

                    const afterPath = line.substring(line.indexOf(quotedMatch[0]) + quotedMatch[0].length).trim();

                    const durationMatch = afterPath.match(/(\d{1,2}):(\d{2}):(\d{2})/);

                    if (durationMatch) {

                        duration = parseInt(durationMatch[1]) * 3600 + parseInt(durationMatch[2]) * 60 + parseInt(durationMatch[3]);

                    }

                } else {

                    // Try to extract file path (could be full path or just filename)

                    const parts = line.split(/\s+/);

                    if (parts.length > 0) {

                        // Check if first part looks like a path

                        if (parts[0].includes('\\') || parts[0].includes('/') || parts[0].includes('.')) {

                            filePath = parts[0];

                        }

                        // Look for duration in remaining parts

                        for (let i = 1; i < parts.length; i++) {

                            const durationMatch = parts[i].match(/(\d{1,2}):(\d{2}):?(\d{2})?/);

                            if (durationMatch) {

                                if (durationMatch[3]) {

                                    duration = parseInt(durationMatch[1]) * 3600 + parseInt(durationMatch[2]) * 60 + parseInt(durationMatch[3]);

                                } else {

                                    duration = parseInt(durationMatch[1]) * 60 + parseInt(durationMatch[2]);

                                }

                                break;

                            }

                        }

                    }

                }



                if (filePath) {

                    const filename = filePath.split(/[\\/]/).pop() || filePath;

                    const detected = autoDetectMediaMetadata(filename, duration, clients);



                    const item: MediaItem = {

                        id: `masterplay_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,

                        title: detected.title || filename.replace(/\.[^/.]+$/, ''),

                        duration: duration || 30, // Default 30 seconds if unknown

                        type: detected.type,

                        src: '', // Will be resolved when played

                        filePath: filePath,

                        filename: filename,

                        thumbnail: '#666',

                        addedAt: now,

                        dateMade: now,

                        client: detected.client,

                        bumperSubtype: detected.bumperSubtype,

                        priority: 0,

                        playCount: 0,

                    };



                    importedItems.push(item);

                }

            }



            if (importedItems.length > 0) {

                setLocalMedia(prev => {

                    const updated = [...prev, ...importedItems];

                    saveLocalMedia(updated);

                    return updated;

                });



                // Save to DB

                const env = getEnvironment();

                if (env.isElectron) {

                    for (const item of importedItems) {

                        dbSaveMedia(item).catch(console.error);

                    }

                }



                setActiveTab('local');

                alert(`Imported ${importedItems.length} items from MasterPlay playlist.`);

            } else {

                alert('No valid items found in the MasterPlay playlist.');

            }

        } catch (err) {

            console.error('Failed to import MasterPlay playlist:', err);

            alert('Failed to import MasterPlay playlist. Check console for details.');

        }



        setShowMasterPlayImporter(false);

    };



    const handleArchiveSelected = () => {

        if (selectedItems.size === 0) return;

        setLocalMedia(prev => prev.map(item =>

            selectedItems.has(item.id) ? { ...item, archived: true } : item

        ));

        setLibraryMedia(prev => prev.map(item =>

            selectedItems.has(item.id) ? { ...item, archived: true } : item

        ));

        setSelectedItems(new Set());

    };



    // Sorting logic

    const sortMedia = (media: MediaItem[]): MediaItem[] => {

        return [...media].sort((a, b) => {

            let comparison = 0;

            switch (sortField) {

                case 'addedAt':

                    comparison = (a.addedAt || '').localeCompare(b.addedAt || '');

                    break;

                case 'lastPlayedAt':

                    comparison = (a.lastPlayedAt || '').localeCompare(b.lastPlayedAt || '');

                    break;

                case 'dateMade':

                    comparison = (a.dateMade || a.episodeDate || '').localeCompare(b.dateMade || b.episodeDate || '');

                    break;

                case 'name':

                    comparison = a.title.localeCompare(b.title);

                    break;

                case 'priority':

                    comparison = (a.priority || 0) - (b.priority || 0);

                    break;

                case 'libraryPriority':

                    comparison =

                        getEffectiveLibraryPriorityScore(a, decayAnchorMs, burnPreset) -

                        getEffectiveLibraryPriorityScore(b, decayAnchorMs, burnPreset);

                    break;

                case 'duration':

                    comparison = a.duration - b.duration;

                    break;

                case 'type':

                    comparison = a.type.localeCompare(b.type);

                    break;

                case 'playCount':

                    comparison = (a.playCount || 0) - (b.playCount || 0);

                    break;

            }

            return sortOrder === 'asc' ? comparison : -comparison;

        });

    };



    const displayedMedia = activeTab === 'library' ? libraryMedia : localMedia;

    const now = Date.now();

    const staleCutoff = now - staleDays * 24 * 60 * 60 * 1000;

    const filteredMedia = displayedMedia.filter(m => {

        const typeMatch = selectedTypes.has(m.type);

        const archiveMatch = showArchived ? true : !m.archived;

        const query = searchQuery.trim().toLowerCase();

        const filename = m.filename ||

            (m.filePath ? m.filePath.split(/[\\/]/).pop() : '') ||

            (m.src && !m.src.startsWith('blob:') && !m.src.startsWith('http') ? m.src.split(/[\\/]/).pop() : '') ||

            '';

        const textMatch = !query

            ? true

            : m.title.toLowerCase().includes(query) || filename.toLowerCase().includes(query);

        const recentMatch = !showRecentOnly || recentlyAccessed.includes(m.id);



        // Missing-file filter

        const isMissing = missingFiles.has(m.id);

        const missingMatch =

            missingFilter === 'all' ? true :

            missingFilter === 'missing' ? isMissing :

            /* playable */ !isMissing;



        // Play-history filter

        const lastPlayedMs = (m as any).lastPlayedAt

            ? new Date((m as any).lastPlayedAt).getTime()

            : 0;

        const playCount = (m as any).playCount || 0;

        const playMatch =

            playFilter === 'all' ? true :

            playFilter === 'never' ? playCount === 0 :

            playFilter === 'recent' ? lastPlayedMs >= staleCutoff :

            /* stale */ (playCount > 0 && lastPlayedMs < staleCutoff);



        // Duration range filter (seconds)

        const dur = m.duration || 0;

        const durMatch =

            (durationMin == null || dur >= durationMin) &&

            (durationMax == null || dur <= durationMax);



        return typeMatch && archiveMatch && textMatch && recentMatch && missingMatch && playMatch && durMatch;

    });

    const sortedMedia = sortMedia(filteredMedia);



    const totalDurationSec = useMemo(() =>

        sortedMedia.reduce((acc, m) => acc + (m.duration || 0), 0), [sortedMedia]);

    const totalDurationFormatted = useMemo(() => {

        const h = Math.floor(totalDurationSec / 3600);

        const m = Math.floor((totalDurationSec % 3600) / 60);

        return h > 0 ? `${h}h ${m}m` : `${m}m`;

    }, [totalDurationSec]);



    // Count archived items

    const archivedCount = displayedMedia.filter(m => m.archived).length;



    const seriesNames = Array.from(new Set(libraryMedia.filter(m => m.programSeries).map(m => m.programSeries!)));



    const getSortLabel = (): string => {

        const labels: Record<SortField, string> = {

            addedAt: 'Recently Added',

            lastPlayedAt: 'Recently Played',

            dateMade: 'Date Made',

            name: 'Name',

            priority: 'Schedule priority',

            libraryPriority: 'Library score',

            duration: 'Length',

            type: 'Type',

            playCount: 'Times Played',

        };

        return labels[sortField];

    };



    const getTypeLabel = (type: MediaType): string => {

        const labels: Record<MediaType, string> = {

            program: 'Program',

            ad: 'Ad',

            station_id: 'Station ID',

            bumper: 'Bumper',

            promo: 'Promo',

            paid_program: 'Paid Program',

            video: 'Video',

            image: 'Image',

            live: 'Live',

            gap: 'Gap',

        };

        return labels[type] || type;

    };



    return (

        <div className={styles.container} onContextMenu={handleContextMenu}>

            <div className={styles.headerRow}>

                <h2 className={styles.header}>Media Library</h2>

                <div className={styles.viewControls}>

                    <input

                        className={styles.searchInput}

                        placeholder="Search title or filename..."

                        value={searchQuery}

                        onChange={e => setSearchQuery(e.target.value)}

                    />

                    <div className={styles.viewControlsButtons}>

                        <button

                            className={cn(styles.viewBtn, viewMode === 'grid' && styles.active)}

                            onClick={() => setViewMode('grid')}

                            title="Grid View"

                        >

                            <Grid size={14} />

                        </button>

                        <button

                            className={cn(styles.viewBtn, viewMode === 'list' && styles.active)}

                            onClick={() => setViewMode('list')}

                            title="List View"

                        >

                            <List size={14} />

                        </button>

                        <button

                            className={cn(styles.viewBtn, viewMode === 'detailed' && styles.active)}

                            onClick={() => setViewMode('detailed')}

                            title="Detailed View"

                        >

                            <LayoutList size={14} />

                        </button>

                    </div>

                </div>

            </div>



            {(activeTab === 'library' || activeTab === 'local') && (

                <div className={styles.statsStrip}>

                    <span className={styles.statsItem}>{sortedMedia.length} items</span>

                    <span className={styles.statsDivider}>·</span>

                    <span className={styles.statsItem}>{totalDurationFormatted} total</span>

                    {activeTab === 'library' && (

                        <>

                            <span className={styles.statsDivider}>·</span>

                            <label className={styles.statsItem} style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>

                                <span>Burn rate</span>

                                <select

                                    className={styles.burnPresetSelect}

                                    value={burnPreset}

                                    onChange={handleBurnPresetChange}

                                    title="Global decay speed for library scores (stored in this browser)"

                                >

                                    <option value="slow">slow</option>

                                    <option value="normal">normal</option>

                                    <option value="fast">fast</option>

                                </select>

                            </label>

                        </>

                    )}

                </div>

            )}



            <div className={styles.tabs}>

                <button

                    className={cn(styles.tab, activeTab === 'library' && styles.activeTab)}

                    onClick={() => setActiveTab('library')}

                >

                    <Library size={14} /> Library

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'feeds' && styles.activeTab)}

                    onClick={() => setActiveTab('feeds')}

                >

                    <Radio size={14} /> Feeds / Streams

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'local' && styles.activeTab)}

                    onClick={() => setActiveTab('local')}

                >

                    <HardDrive size={14} /> Local ({localMedia.length})

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'favorites' && styles.activeTab)}

                    onClick={() => setActiveTab('favorites')}

                >

                    <Star size={14} /> Favorites ({favorites.size})

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'analysis' && styles.activeTab)}

                    onClick={() => setActiveTab('analysis')}

                >

                    <Activity size={14} /> Analysis

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'collections' && styles.activeTab)}

                    onClick={() => setActiveTab('collections')}

                >

                    <Folder size={14} /> Smart Collections

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'watchfolders' && styles.activeTab)}

                    onClick={() => setActiveTab('watchfolders')}

                >

                    <FolderOpen size={14} /> Watch Folders

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'stats' && styles.activeTab)}

                    onClick={() => setActiveTab('stats')}

                >

                    <BarChart2 size={14} /> Stats

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'commands' && styles.activeTab)}

                    onClick={() => setActiveTab('commands')}

                >

                    <Terminal size={14} /> Commands

                </button>

                <button

                    className={cn(styles.tab, activeTab === 'assets' && styles.activeTab)}

                    onClick={() => setActiveTab('assets')}

                >

                    <Film size={14} /> Assets

                </button>

            </div>



            {/* Toolbar */}

            {(activeTab === 'library' || activeTab === 'local') && (

                <div className={styles.toolbar}>

                    <button

                        className={cn(styles.recentFilterBtn, showRecentOnly && styles.recentFilterBtnActive)}

                        onClick={() => setShowRecentOnly(!showRecentOnly)}

                        title={showRecentOnly ? 'Show all items' : 'Show recently opened only'}

                    >

                        <Clock size={12} />

                        <span>Recent{recentlyAccessed.length > 0 ? ` (${recentlyAccessed.length})` : ''}</span>

                    </button>

                    <div className={styles.filterGroup} ref={typeFilterRef}>

                        <button

                            className={styles.filterBtn}

                            onClick={(e) => { e.stopPropagation(); setShowTypeFilter(!showTypeFilter); }}

                        >

                            <Filter size={12} />

                            <span>Types ({selectedTypes.size}/{ALL_MEDIA_TYPES.length})</span>

                            <ChevronDown size={12} />

                        </button>



                        {showTypeFilter && (

                            <div className={styles.typeFilterDropdown}>

                                <div className={styles.typeFilterHeader}>

                                    <button

                                        className={styles.selectAllBtn}

                                        onClick={allTypesSelected ? deselectAllTypes : selectAllTypes}

                                    >

                                        {allTypesSelected ? <CheckSquare size={14} /> : <Square size={14} />}

                                        <span>{allTypesSelected ? 'Deselect All' : 'Select All'}</span>

                                    </button>

                                </div>

                                <div className={styles.typeFilterList}>

                                    {ALL_MEDIA_TYPES.map(type => (

                                        <label key={type} className={styles.typeFilterItem}>

                                            <input

                                                type="checkbox"

                                                checked={selectedTypes.has(type)}

                                                onChange={() => toggleType(type)}

                                            />

                                            <span className={cn(styles.typeBadgeSmall, styles[type])}>

                                                {getTypeLabel(type)}

                                            </span>

                                        </label>

                                    ))}

                                </div>

                            </div>

                        )}

                    </div>



                    {/* Archive Toggle */}

                    <button

                        className={cn(styles.archiveBtn, showArchived && styles.active)}

                        onClick={() => setShowArchived(!showArchived)}

                        title={showArchived ? 'Hide archived items' : 'Show archived items'}

                    >

                        {showArchived ? <Eye size={12} /> : <EyeOff size={12} />}

                        <span>Archived{archivedCount > 0 ? ` (${archivedCount})` : ''}</span>

                    </button>



                    {/* Extended Filters toggle */}

                    <button

                        className={cn(styles.archiveBtn, showFilters && styles.active)}

                        onClick={() => setShowFilters(s => !s)}

                        title="More filters"

                    >

                        <Filter size={12} />

                        <span>Filters{

                            (missingFilter !== 'all' || playFilter !== 'all' || durationMin != null || durationMax != null)

                                ? ' •' : ''

                        }</span>

                    </button>



                    {/* Purge menu */}

                    <div style={{ position: 'relative' }}>

                        <button

                            className={styles.deleteSelectedBtn}

                            onClick={() => setShowPurgeMenu(s => !s)}

                            title="Bulk purge with safety confirmation"

                        >

                            <Trash2 size={12} /><span>Purge…</span>

                        </button>

                        {showPurgeMenu && (

                            <div style={{

                                position: 'absolute', top: '100%', right: 0, marginTop: 4, zIndex: 20,

                                background: 'var(--surface)', border: '1px solid var(--border)',

                                borderRadius: 6, padding: 4, minWidth: 240,

                                boxShadow: '0 8px 24px rgba(0,0,0,0.4)',

                            }}>

                                <button onClick={purgeMissing}    style={purgeRowStyle}>Purge missing files</button>

                                <button onClick={purgeNeverPlayed} style={purgeRowStyle}>Purge never-played items</button>

                                <button onClick={purgeStale}      style={purgeRowStyle}>Purge stale ({staleDays}+ days)</button>

                                <button onClick={purgeArchived}    style={purgeRowStyle}>Purge archived items</button>

                                <button onClick={purgeFiltered}    style={purgeRowStyle}>Purge currently visible</button>

                                <hr style={{ border: 0, borderTop: '1px solid var(--border)', margin: '4px 0' }} />

                                <button onClick={purgeAll} style={{ ...purgeRowStyle, color: '#ef4444' }}>

                                    Purge EVERYTHING (nuclear)

                                </button>

                            </div>

                        )}

                    </div>



                    {/* Sort Button */}

                    <div className={styles.sortGroup}>

                        <button

                            className={styles.sortBtn}

                            onClick={(e) => { e.stopPropagation(); handleContextMenu(e); }}

                        >

                            {sortOrder === 'asc' ? <SortAsc size={12} /> : <SortDesc size={12} />}

                            <span>{getSortLabel()}</span>

                            <ChevronDown size={12} />

                        </button>

                    </div>



                    {/* Bulk Actions */}

                    {selectedItems.size > 0 && (

                        <>

                            <button className={styles.archiveSelectedBtn} onClick={() => setShowBulkTypeChange(true)}>

                                <Edit2 size={12} /> Change Type ({selectedItems.size})

                            </button>

                            <button className={styles.archiveSelectedBtn} onClick={handleArchiveSelected}>

                                <Archive size={12} /> Archive ({selectedItems.size})

                            </button>

                            <button className={styles.deleteSelectedBtn} onClick={handleDeleteSelected}>

                                <Trash2 size={12} /> Delete ({selectedItems.size})

                            </button>

                        </>

                    )}



                    <div className={styles.itemCount}>

                        {sortedMedia.length} items · {totalDurationFormatted}

                    </div>

                </div>

            )}



            {/* Extended filter panel */}

            {showFilters && (

                <div style={{

                    display: 'flex', flexWrap: 'wrap', gap: 16, alignItems: 'center',

                    padding: '8px 12px', borderBottom: '1px solid var(--border)',

                    background: 'var(--surface)', fontSize: 12,

                }}>

                    <label style={{ display: 'flex', gap: 6, alignItems: 'center' }}>

                        <span style={{ color: 'var(--text-secondary)' }}>File status:</span>

                        <select

                            value={missingFilter}

                            onChange={e => setMissingFilter(e.target.value as any)}

                            style={{ padding: '2px 6px', background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 4 }}

                        >

                            <option value="all">All</option>

                            <option value="playable">Playable only</option>

                            <option value="missing">Missing only ({missingFiles.size})</option>

                        </select>

                    </label>



                    <label style={{ display: 'flex', gap: 6, alignItems: 'center' }}>

                        <span style={{ color: 'var(--text-secondary)' }}>Play history:</span>

                        <select

                            value={playFilter}

                            onChange={e => setPlayFilter(e.target.value as any)}

                            style={{ padding: '2px 6px', background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 4 }}

                        >

                            <option value="all">All</option>

                            <option value="never">Never played</option>

                            <option value="recent">Played within {staleDays}d</option>

                            <option value="stale">Stale (not played in {staleDays}+ days)</option>

                        </select>

                        {(playFilter === 'recent' || playFilter === 'stale') && (

                            <input

                                type="number" min={1} max={3650}

                                value={staleDays}

                                onChange={e => setStaleDays(Math.max(1, parseInt(e.target.value, 10) || 1))}

                                style={{ width: 60, padding: '2px 6px', background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 4 }}

                            />

                        )}

                    </label>



                    <label style={{ display: 'flex', gap: 6, alignItems: 'center' }}>

                        <span style={{ color: 'var(--text-secondary)' }}>Duration (s):</span>

                        <input

                            type="number" min={0} placeholder="min"

                            value={durationMin ?? ''}

                            onChange={e => setDurationMin(e.target.value === '' ? null : Math.max(0, parseInt(e.target.value, 10) || 0))}

                            style={{ width: 70, padding: '2px 6px', background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 4 }}

                        />

                        <span style={{ color: 'var(--text-secondary)' }}>–</span>

                        <input

                            type="number" min={0} placeholder="max"

                            value={durationMax ?? ''}

                            onChange={e => setDurationMax(e.target.value === '' ? null : Math.max(0, parseInt(e.target.value, 10) || 0))}

                            style={{ width: 70, padding: '2px 6px', background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 4 }}

                        />

                    </label>



                    <button

                        onClick={() => {

                            setMissingFilter('all'); setPlayFilter('all');

                            setDurationMin(null); setDurationMax(null);

                        }}

                        style={{ padding: '2px 8px', background: 'transparent', border: '1px solid var(--border)', color: 'var(--text-secondary)', borderRadius: 4, cursor: 'pointer' }}

                    >

                        Reset filters

                    </button>

                </div>

            )}



            <div className={styles.content}>

                {activeTab === 'local' && (

                    <div className={styles.uploadContainer}>

                        <div className={styles.uploadRow}>

                            <div

                                className={styles.uploadArea}

                                onClick={handleImportClick}

                                onDragOver={(e) => { e.preventDefault(); e.stopPropagation(); }}

                                onDrop={(e) => {

                                    e.preventDefault();

                                    e.stopPropagation();

                                    if (e.dataTransfer.files.length > 0) {

                                        Array.from(e.dataTransfer.files).forEach(file => processFile(file));

                                    }

                                }}

                            >

                                <Upload size={24} />

                                <span>Import Files</span>

                            </div>

                            <button

                                className={styles.bulkImportBtn}

                                onClick={() => setShowBulkImporter(true)}

                            >

                                <FolderOpen size={20} />

                                <span>Bulk Import</span>

                                <span className={styles.bulkHint}>Ads, Programs, IDs, Bumpers...</span>

                            </button>

                            <button

                                className={styles.bulkImportBtn}

                                onClick={() => setShowLiveCreator(true)}

                                title="Create a live source (NDI / capture / URL / OBS scene / GStreamer)"

                            >

                                <Radio size={20} />

                                <span>New Live Source</span>

                                <span className={styles.bulkHint}>NDI, Capture, URL, OBS</span>

                            </button>

                            <button

                                className={styles.bulkImportBtn}

                                onClick={handleRebuildThumbnails}

                                disabled={rebuildingThumbs}

                                title="Regenerate thumbnails for library items and persist them"

                            >

                                <RefreshCw size={20} />

                                <span>{rebuildingThumbs ? 'Rebuilding…' : 'Rebuild Thumbnails'}</span>

                                <span className={styles.bulkHint}>All library items</span>

                            </button>

                            <button

                                className={styles.bulkImportBtn}

                                onClick={() => masterPlayInputRef.current?.click()}

                                title="Import MasterPlay .in playlist file"

                            >

                                <FileInput size={20} />

                                <span>Import MasterPlay</span>

                                <span className={styles.bulkHint}>.in playlist file</span>

                            </button>

                        </div>

                        <input

                            type="file"

                            ref={fileInputRef}

                            className={styles.hiddenInput}

                            accept="video/*,image/*"

                            multiple

                            onChange={handleFileSelect}

                        />

                        <input

                            type="file"

                            ref={masterPlayInputRef}

                            className={styles.hiddenInput}

                            accept=".in,.txt"

                            onChange={(e) => {

                                if (e.target.files?.[0]) {

                                    handleMasterPlayImport(e.target.files[0]);

                                    e.target.value = '';

                                }

                            }}

                        />

                    </div>

                )}



                {activeTab === 'feeds' && (

                    <InputManager />

                )}



                {activeTab === 'commands' && (

                    <CommandsPanel

                        onCommandClick={(command) => {

                            // Convert command to a pseudo media item for playlist insertion

                            if (onItemClick) {

                                const pseudoMedia: MediaItem = {

                                    id: command.id,

                                    title: command.label,

                                    duration: command.duration || 0,

                                    src: `command://${command.command}`,

                                    type: 'gap', // Use gap type for commands

                                    notes: JSON.stringify(command),

                                };

                                onItemClick(pseudoMedia, { isCommand: true, command });

                            }

                        }}

                    />

                )}

                

                {/* Favorites Tab */}

                {activeTab === 'favorites' && (

                    <MediaFavorites

                        mediaItems={[...libraryMedia, ...localMedia]}

                        favorites={favorites}

                        recentlyAccessed={recentlyAccessed}

                        onToggleFavorite={toggleFavorite}

                        onPlayItem={(item) => {

                            trackRecentAccess(item.id);

                            if (onItemClick) onItemClick(item);

                        }}

                        onItemClick={(item) => {

                            trackRecentAccess(item.id);

                            setMetadataItem(item);

                        }}

                        onClearRecent={clearRecentlyAccessed}

                    />

                )}

                

                {/* Analysis Tab — enhanced AssetInspector with FFmpeg 8.x analysis */}

                {activeTab === 'analysis' && analysisItem && (

                    <AssetInspector

                        item={analysisItem}

                        onClose={() => setAnalysisItem(null)}

                    />

                )}

                {activeTab === 'analysis' && !analysisItem && (

                    <div className={styles.emptyAnalysis}>

                        <Activity size={48} />

                        <p>Select a media item to analyze</p>

                        <span>Click the Analysis icon on any media card to start analysis</span>

                    </div>

                )}

                

                {/* Smart Collections Tab */}

                {activeTab === 'collections' && (

                    <SmartCollections

                        allMedia={[...libraryMedia, ...localMedia]}

                        onSelectCollection={(collection, items) => {

                            // Could filter library view or create a playlist

                            console.log('Collection selected:', collection.name, 'with', items.length, 'items');

                        }}

                        onMediaClick={(item) => {

                            trackRecentAccess(item.id);

                            if (onItemClick) onItemClick(item);

                        }}

                    />

                )}

                

                {/* Watch Folders Tab */}

                {activeTab === 'watchfolders' && (

                    <WatchFolders

                        onImportMedia={async (filePaths, type, metadata) => {

                            // Process imported files

                            console.log('Watch folder import:', filePaths, type, metadata);

                        }}

                    />

                )}

                

                {/* Stats Tab */}

                {activeTab === 'stats' && (

                    <MediaStatsDashboard

                        allMedia={[...libraryMedia, ...localMedia]}

                    />

                )}



                {activeTab === 'assets' && (

                    <AssetManager

                        allMedia={[...libraryMedia, ...localMedia]}

                        onMediaUpdate={(updates) => {

                            // Update both local and library media

                            const localUpdates = updates.filter(u => localMedia.some(m => m.id === u.id));

                            const libraryUpdates = updates.filter(u => libraryMedia.some(m => m.id === u.id));

                            if (localUpdates.length > 0) {

                                setLocalMedia(prev => {

                                    const updated = prev.map(item => {

                                        const update = localUpdates.find(u => u.id === item.id);

                                        return update || item;

                                    });

                                    saveLocalMedia(updated);

                                    return updated;

                                });

                            }

                            if (libraryUpdates.length > 0) {

                                setLibraryMedia(prev => {

                                    const updated = prev.map(item => {

                                        const update = libraryUpdates.find(u => u.id === item.id);

                                        return update || item;

                                    });

                                    saveLibraryMedia(updated);

                                    return updated;

                                });

                            }

                        }}

                    />

                )}



                {(activeTab === 'library' || activeTab === 'local') && (

                    <>

                        <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8, marginBottom: 8 }}>

                            <button

                                className={styles.bulkImportBtn}

                                onClick={handleRebuildThumbnails}

                                disabled={rebuildingThumbs}

                                title="Regenerate thumbnails for library items and persist them"

                            >

                                <RefreshCw size={18} />

                                <span>{rebuildingThumbs ? 'Rebuilding…' : 'Rebuild Thumbnails'}</span>

                            </button>

                        </div>

                        {viewMode === 'grid' && (

                            <div className={styles.grid}>

                                {sortedMedia.map((item) => (

                                    <MediaItemCard

                                        key={item.id}

                                        item={{ ...(item as any), __missing: missingFiles.has(item.id) } as any}

                                        viewMode="grid"

                                        selected={selectedItems.has(item.id)}

                                        onSelect={(selected) => {

                                            const newSelected = new Set(selectedItems);

                                            if (selected) newSelected.add(item.id);

                                            else newSelected.delete(item.id);

                                            setSelectedItems(newSelected);

                                        }}

                                        onClick={() => {

                                            trackRecentAccess(item.id);

                                            onItemClick?.(item);

                                        }}

                                        onEdit={() => setEditingItem(item)}

                                        onArchive={() => handleArchiveItem(item.id)}

                                        onUnarchive={() => handleUnarchiveItem(item.id)}

                                        onContextMenu={(e) => onItemContextMenu?.(item, e.clientX, e.clientY)}

                                        onOpenScrubber={() => openVideoScrubber(item)}

                                        onOpenMetadata={() => setMetadataItem(item)}

                                        onToggleFavorite={() => toggleFavorite(item.id)}

                                        isFavorite={favorites.has(item.id)}

                                        decayAnchorMs={decayAnchorMs}

                                        burnPreset={burnPreset}

                                    />

                                ))}

                            </div>

                        )}



                        {viewMode === 'list' && (

                            <div className={styles.list}>

                                {sortedMedia.map((item) => (

                                    <MediaItemCard

                                        key={item.id}

                                        item={{ ...(item as any), __missing: missingFiles.has(item.id) } as any}

                                        viewMode="list"

                                        selected={selectedItems.has(item.id)}

                                        onSelect={(selected) => {

                                            const newSelected = new Set(selectedItems);

                                            if (selected) newSelected.add(item.id);

                                            else newSelected.delete(item.id);

                                            setSelectedItems(newSelected);

                                        }}

                                        onClick={() => {

                                            trackRecentAccess(item.id);

                                            onItemClick?.(item);

                                        }}

                                        onEdit={() => setEditingItem(item)}

                                        onArchive={() => handleArchiveItem(item.id)}

                                        onUnarchive={() => handleUnarchiveItem(item.id)}

                                        onContextMenu={(e) => onItemContextMenu?.(item, e.clientX, e.clientY)}

                                        onOpenScrubber={() => openVideoScrubber(item)}

                                        onOpenMetadata={() => setMetadataItem(item)}

                                        onToggleFavorite={() => toggleFavorite(item.id)}

                                        isFavorite={favorites.has(item.id)}

                                        decayAnchorMs={decayAnchorMs}

                                        burnPreset={burnPreset}

                                    />

                                ))}

                            </div>

                        )}



                        {viewMode === 'detailed' && (

                            <div className={styles.detailedList}>

                                <div className={styles.detailedHeader}>

                                    <div className={styles.detailedCheckbox}></div>

                                    <div className={styles.detailedThumb}>Thumb</div>

                                    <div className={styles.detailedName} onClick={() => handleSort('name')}>

                                        Name {sortField === 'name' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedType} onClick={() => handleSort('type')}>

                                        Type {sortField === 'type' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedDuration} onClick={() => handleSort('duration')}>

                                        Duration {sortField === 'duration' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedPlays} onClick={() => handleSort('playCount')}>

                                        Plays {sortField === 'playCount' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedLibScore} onClick={() => handleSort('libraryPriority')}>

                                        Lib {sortField === 'libraryPriority' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedPriority} onClick={() => handleSort('priority')}>

                                        Sched {sortField === 'priority' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedAdded} onClick={() => handleSort('addedAt')}>

                                        Added {sortField === 'addedAt' && (sortOrder === 'asc' ? '↑' : '↓')}

                                    </div>

                                    <div className={styles.detailedActions}>Actions</div>

                                </div>

                                {sortedMedia.map((item) => (

                                    <MediaItemCard

                                        key={item.id}

                                        item={{ ...(item as any), __missing: missingFiles.has(item.id) } as any}

                                        viewMode="detailed"

                                        selected={selectedItems.has(item.id)}

                                        onSelect={(selected) => {

                                            const newSelected = new Set(selectedItems);

                                            if (selected) newSelected.add(item.id);

                                            else newSelected.delete(item.id);

                                            setSelectedItems(newSelected);

                                        }}

                                        onClick={() => {

                                            trackRecentAccess(item.id);

                                            onItemClick?.(item);

                                        }}

                                        onEdit={() => setEditingItem(item)}

                                        onArchive={() => handleArchiveItem(item.id)}

                                        onUnarchive={() => handleUnarchiveItem(item.id)}

                                        onOpenScrubber={() => openVideoScrubber(item)}

                                        onOpenMetadata={() => setMetadataItem(item)}

                                        onToggleFavorite={() => toggleFavorite(item.id)}

                                        isFavorite={favorites.has(item.id)}

                                        onContextMenu={(e) => onItemContextMenu?.(item, e.clientX, e.clientY)}

                                        decayAnchorMs={decayAnchorMs}

                                        burnPreset={burnPreset}

                                    />

                                ))}

                            </div>

                        )}



                        {sortedMedia.length === 0 && (

                            <div className={styles.emptyState}>

                                No media found. {activeTab === 'local' ? 'Import some files to get started.' : 'Try adjusting filters.'}

                            </div>

                        )}

                    </>

                )}

            </div>



            {/* Context Menu */}

            {showContextMenu && (

                <div

                    ref={contextMenuRef}

                    className={styles.contextMenu}

                    style={{ top: contextMenuPos.y, left: contextMenuPos.x }}

                >

                    <div className={styles.contextMenuTitle}>Sort By</div>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'addedAt' && styles.active)}

                        onClick={() => handleSort('addedAt')}

                    >

                        <Clock size={14} /> Most Recent Added {sortField === 'addedAt' && (sortOrder === 'desc' ? '(Newest)' : '(Oldest)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'lastPlayedAt' && styles.active)}

                        onClick={() => handleSort('lastPlayedAt')}

                    >

                        <Play size={14} /> Most Recent Played {sortField === 'lastPlayedAt' && (sortOrder === 'desc' ? '(Newest)' : '(Oldest)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'playCount' && styles.active)}

                        onClick={() => handleSort('playCount')}

                    >

                        <BarChart2 size={14} /> Times Played {sortField === 'playCount' && (sortOrder === 'desc' ? '(Most)' : '(Least)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'dateMade' && styles.active)}

                        onClick={() => handleSort('dateMade')}

                    >

                        <Calendar size={14} /> Date Made {sortField === 'dateMade' && (sortOrder === 'desc' ? '(Newest)' : '(Oldest)')}

                    </button>

                    <div className={styles.contextMenuDivider} />

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'name' && styles.active)}

                        onClick={() => handleSort('name')}

                    >

                        <FileText size={14} /> Name {sortField === 'name' && (sortOrder === 'asc' ? '(A-Z)' : '(Z-A)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'libraryPriority' && styles.active)}

                        onClick={() => handleSort('libraryPriority')}

                    >

                        <Activity size={14} /> Library score {sortField === 'libraryPriority' && (sortOrder === 'desc' ? '(High)' : '(Low)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'priority' && styles.active)}

                        onClick={() => handleSort('priority')}

                    >

                        <Star size={14} /> Schedule priority {sortField === 'priority' && (sortOrder === 'desc' ? '(High)' : '(Low)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'duration' && styles.active)}

                        onClick={() => handleSort('duration')}

                    >

                        <Hash size={14} /> Length {sortField === 'duration' && (sortOrder === 'desc' ? '(Longest)' : '(Shortest)')}

                    </button>

                    <button

                        className={cn(styles.contextMenuItem, sortField === 'type' && styles.active)}

                        onClick={() => handleSort('type')}

                    >

                        <Film size={14} /> Type {sortField === 'type' && (sortOrder === 'asc' ? '(A-Z)' : '(Z-A)')}

                    </button>

                </div>

            )}



            {editingItem && (

                <div className={styles.editorOverlay}>

                    <MediaEditor

                        item={editingItem}

                        onSave={handleSaveEdit}

                        onDelete={handleDeleteItem}

                        onCancel={() => setEditingItem(null)}

                    />

                </div>

            )}



            {showBulkImporter && (

                <BulkMediaImporter

                    onImportComplete={(items) => {

                        setLocalMedia(prev => [...prev, ...items]);

                        saveLocalMedia([...localMedia, ...items]);

                        setShowBulkImporter(false);

                    }}

                    onClose={() => setShowBulkImporter(false)}

                />

            )}



            {showLiveCreator && (

                <LiveSourceCreator

                    onCancel={() => setShowLiveCreator(false)}

                    onCreate={handleCreateLiveSource}

                />

            )}



            {showBulkTypeChange && (

                <BulkTypeChangeModal

                    selectedCount={selectedItems.size}

                    clients={clients}

                    onApply={handleBulkTypeChange}

                    onCreateClient={handleCreateClient}

                    onClose={() => setShowBulkTypeChange(false)}

                />

            )}

            

            {/* Video Scrubber Modal */}

            {scrubberItem && (

                <VideoScrubber

                    mediaItem={scrubberItem}

                    onClose={() => setScrubberItem(null)}

                    onCreateSegment={(segment) => {

                        handleCreateSegment(scrubberItem.id, segment);

                    }}

                    onSaveMarks={(markIn, markOut) => {

                        handleSaveMarks(scrubberItem.id, markIn, markOut);

                    }}

                />

            )}

            

            {/* Enhanced Metadata Panel */}

            {metadataItem && (

                <EnhancedMetadataPanel

                    item={metadataItem}

                    onClose={() => setMetadataItem(null)}

                    onUpdate={(updatedItem) => {

                        // Update in local or library media

                        const updateMedia = (media: MediaItem[]) => {

                            return media.map(item => item.id === updatedItem.id ? updatedItem : item);

                        };

                        setLocalMedia(prev => updateMedia(prev));

                        setLibraryMedia(prev => updateMedia(prev));

                        setMetadataItem(null);

                    }}

                    onToggleFavorite={toggleFavorite}

                    isFavorite={favorites.has(metadataItem.id)}

                />

            )}

        </div>

    );

}



function LiveSourceCreator({

    onCancel,

    onCreate,

}: {

    onCancel: () => void;

    onCreate: (item: MediaItem) => void | Promise<void>;

}) {

    const [title, setTitle] = useState('Live Source');

    const [kind, setKind] = useState<LiveSourceKind>('ndi');

    const [duration, setDuration] = useState(300);

    const [ndiName, setNdiName] = useState('');

    const [device, setDevice] = useState('');

    const [url, setUrl] = useState('');

    const [obsScene, setObsScene] = useState('');

    const [pipeline, setPipeline] = useState('');



    const buildLiveSource = (): LiveSourceConfig => ({

        kind,

        name: title,

        ndiName: kind === 'ndi' ? ndiName : undefined,

        device: kind === 'capture' ? device : undefined,

        url: kind === 'url' ? url : undefined,

        obsScene: kind === 'obs_scene' ? obsScene : undefined,

        pipeline: pipeline || undefined,

    });



    const handleCreate = async () => {

        const now = new Date().toISOString();

        const id = `live_${Date.now()}_${Math.random().toString(36).slice(2, 7)}`;

        const liveSource = buildLiveSource();



        const item: MediaItem = {

            id,

            title: title || 'Live Source',

            type: 'live',

            duration: Math.max(1, duration || 1),

            // Not directly playable by internal player; external engines interpret liveSource

            src: `live:${id}`,

            thumbnail: '#16a34a',

            liveSource,

            addedAt: now,

            dateMade: now,

            playCount: 0,

            priority: 0,

        };



        await onCreate(item);

    };



    return (

        <div className={styles.editorOverlay} style={{ zIndex: 2100 }}>

            <div className={styles.editor} style={{ maxWidth: 620 }}>

                <div className={styles.editorHeader}>

                    <h3>Create Live Source</h3>

                    <button onClick={onCancel} className={styles.iconBtn}><X size={16} /></button>

                </div>



                <div className={styles.editorGrid}>

                    <div className={styles.editorColumn}>

                        <label className={styles.label}>Name</label>

                        <input className={styles.input} value={title} onChange={e => setTitle(e.target.value)} />



                        <label className={styles.label}>Type</label>

                        <select className={styles.select} value={kind} onChange={e => setKind(e.target.value as LiveSourceKind)}>

                            <option value="ndi">NDI</option>

                            <option value="capture">Capture Device</option>

                            <option value="url">URL Feed (RTSP/SRT/HTTP/UDP)</option>

                            <option value="obs_scene">OBS Scene (fallback)</option>

                            <option value="gstreamer">Custom GStreamer Pipeline</option>

                        </select>



                        <label className={styles.label}>Playout Duration (seconds)</label>

                        <input className={styles.input} type="number" min={1} value={duration} onChange={e => setDuration(parseInt(e.target.value) || 300)} />

                    </div>



                    <div className={styles.editorColumn}>

                        {kind === 'ndi' && (

                            <>

                                <label className={styles.label}>NDI Source Name</label>

                                <input className={styles.input} value={ndiName} onChange={e => setNdiName(e.target.value)} placeholder="e.g. OBS-NDI (Program)" />

                            </>

                        )}

                        {kind === 'capture' && (

                            <>

                                <label className={styles.label}>Device Name</label>

                                <input className={styles.input} value={device} onChange={e => setDevice(e.target.value)} placeholder='Windows example: "USB Video"' />

                            </>

                        )}

                        {kind === 'url' && (

                            <>

                                <label className={styles.label}>URL</label>

                                <input className={styles.input} value={url} onChange={e => setUrl(e.target.value)} placeholder="rtsp://... or srt://... or http://..." />

                            </>

                        )}

                        {kind === 'obs_scene' && (

                            <>

                                <label className={styles.label}>OBS Scene Name</label>

                                <input className={styles.input} value={obsScene} onChange={e => setObsScene(e.target.value)} placeholder="Scene name in OBS" />

                            </>

                        )}



                        <label className={styles.label}>GStreamer Pipeline (optional)</label>

                        <textarea

                            className={styles.textarea}

                            value={pipeline}

                            onChange={e => setPipeline(e.target.value)}

                            placeholder='Use tokens: INPUT / ${INPUT} / ${NDI} / ${URL} / ${DEVICE}'

                            rows={4}

                        />

                    </div>

                </div>



                <div className={styles.editorActions}>

                    <button className={styles.cancelBtn} onClick={onCancel}>Cancel</button>

                    <button className={styles.saveBtn} onClick={handleCreate}>

                        <Check size={14} /> Create

                    </button>

                </div>

            </div>

        </div>

    );

}



// Bulk Type Change Modal

function BulkTypeChangeModal({

    selectedCount,

    clients,

    onApply,

    onCreateClient,

    onClose,

}: {

    selectedCount: number;

    clients: AdClient[];

    onApply: (type: MediaType, client?: string) => void;

    onCreateClient: (name: string) => AdClient;

    onClose: () => void;

}) {

    const [newType, setNewType] = useState<MediaType>('ad');

    const [selectedClient, setSelectedClient] = useState<string>('');

    const [showNewClient, setShowNewClient] = useState(false);

    const [newClientName, setNewClientName] = useState('');



    const handleApply = () => {

        if (newType === 'ad' && showNewClient && newClientName.trim()) {

            const client = onCreateClient(newClientName.trim());

            onApply(newType, client.id);

        } else {

            onApply(newType, selectedClient || undefined);

        }

    };



    return (

        <div className={styles.editorOverlay}>

            <div className={styles.editor}>

                <div className={styles.editorHeader}>

                    <h3>Bulk Type Change</h3>

                    <button onClick={onClose} className={styles.iconBtn}><X size={16} /></button>

                </div>



                <p style={{ marginBottom: '1rem', color: 'var(--text-secondary)' }}>

                    Change type for {selectedCount} selected item{selectedCount !== 1 ? 's' : ''}

                </p>



                <label className={styles.label}>New Type</label>

                <select

                    className={styles.select}

                    value={newType}

                    onChange={e => setNewType(e.target.value as MediaType)}

                >

                    <option value="program">Program</option>

                    <option value="ad">Ad</option>

                    <option value="station_id">Station ID</option>

                    <option value="bumper">Bumper</option>

                    <option value="promo">Promo</option>

                    <option value="paid_program">Paid Program</option>

                    <option value="video">Video</option>

                    <option value="image">Image</option>

                </select>



                {newType === 'ad' && (

                    <>

                        <label className={styles.label}>Client</label>

                        {!showNewClient ? (

                            <div style={{ display: 'flex', gap: '0.5rem' }}>

                                <select

                                    className={styles.select}

                                    value={selectedClient}

                                    onChange={e => setSelectedClient(e.target.value)}

                                    style={{ flex: 1 }}

                                >

                                    <option value="">-- No Client --</option>

                                    {clients.map(c => (

                                        <option key={c.id} value={c.id}>{c.name}</option>

                                    ))}

                                </select>

                                <button

                                    className={styles.iconBtn}

                                    onClick={() => setShowNewClient(true)}

                                    title="Create new client"

                                >

                                    <Plus size={16} />

                                </button>

                            </div>

                        ) : (

                            <div style={{ display: 'flex', gap: '0.5rem' }}>

                                <input

                                    className={styles.input}

                                    value={newClientName}

                                    onChange={e => setNewClientName(e.target.value)}

                                    placeholder="New client name..."

                                    style={{ flex: 1 }}

                                />

                                <button

                                    className={styles.iconBtn}

                                    onClick={() => { setShowNewClient(false); setNewClientName(''); }}

                                    title="Cancel"

                                >

                                    <X size={16} />

                                </button>

                            </div>

                        )}

                    </>

                )}



                <div className={styles.editorActions}>

                    <button className={styles.cancelBtn} onClick={onClose}>Cancel</button>

                    <button className={styles.saveBtn} onClick={handleApply}>

                        <Check size={14} /> Apply to {selectedCount} items

                    </button>

                </div>

            </div>

        </div>

    );

}



// Media Item Card - supports multiple view modes

function MediaItemCard({

    item,

    viewMode,

    selected,

    onSelect,

    onClick,

    onEdit,

    onArchive,

    onUnarchive,

    onContextMenu,

    onOpenScrubber,

    onOpenMetadata,

    onToggleFavorite,

    isFavorite,

    decayAnchorMs,

    burnPreset,

}: {

    item: MediaItem;

    viewMode: ViewMode;

    selected: boolean;

    onSelect: (selected: boolean) => void;

    onClick?: () => void;

    onEdit: () => void;

    onArchive?: () => void;

    onUnarchive?: () => void;

    onContextMenu?: (e: React.MouseEvent) => void;

    onOpenScrubber?: () => void;

    onOpenMetadata?: () => void;

    onToggleFavorite?: () => void;

    isFavorite?: boolean;

    decayAnchorMs: number;

    burnPreset: MediaBurnPreset;

}) {

    const handleDragStart = (e: React.DragEvent) => {

        e.dataTransfer.setData('application/json', JSON.stringify(item));

        e.dataTransfer.effectAllowed = 'copy';

    };



    const formatDate = (dateStr?: string) => {

        if (!dateStr) return '-';

        try {

            return new Date(dateStr).toLocaleDateString();

        } catch {

            return '-';

        }

    };



    const formatDateTime = (dateStr?: string) => {

        if (!dateStr) return 'Never';

        try {

            const d = new Date(dateStr);

            return `${d.toLocaleDateString()} ${d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`;

        } catch {

            return 'Never';

        }

    };



    const libScore = getEffectiveLibraryPriorityScore(item, decayAnchorMs, burnPreset);



    if (viewMode === 'detailed') {

        return (

            <div

                className={cn(styles.detailedRow, selected && styles.selected, item.archived && styles.archived)}

                onContextMenu={onContextMenu}

                draggable

                onDragStart={handleDragStart}

            >

                <div className={styles.detailedCheckbox}>

                    <input

                        type="checkbox"

                        checked={selected}

                        onChange={(e) => onSelect(e.target.checked)}

                        onClick={(e) => e.stopPropagation()}

                    />

                </div>

                <div className={styles.detailedThumb} onClick={onClick}>

                    <div

                        className={styles.thumbImg}

                        style={{

                            backgroundColor: item.thumbnail?.startsWith('#') ? item.thumbnail : 'var(--surface-highlight)',

                            backgroundImage: item.thumbnail?.startsWith('data:') ? `url(${item.thumbnail})` : undefined,

                        }}

                    >

                        {!item.thumbnail?.startsWith('data:') && <Film size={12} />}

                    </div>

                </div>

                <div className={styles.detailedName} onClick={onClick} title={item.title}>

                    <span className={styles.titleText}>

                        {item.archived && <Archive size={10} className={styles.archivedIcon} />}

                        {(item as any).__missing && (

                            <span title="Missing file (path not found)" style={{ display: 'inline-flex', alignItems: 'center' }}>

                                <AlertTriangle size={12} style={{ color: 'var(--warning)', marginRight: 6 }} />

                            </span>

                        )}

                        {item.title}

                    </span>

                    {item.programSeries && <span className={styles.seriesTag}>{item.programSeries}</span>}

                </div>

                <div className={styles.detailedType}>

                    <span className={cn(styles.typeBadgeSmall, styles[item.type])}>

                        {item.type.replace('_', ' ')}

                    </span>

                </div>

                <div className={styles.detailedDuration}>{formatDuration(item.duration)}</div>

                <div className={styles.detailedPlays}>

                    <span className={styles.playCountBadge}>{item.playCount || 0}</span>

                </div>

                <div className={styles.detailedLibScore} title="Library score (decaying heat; see docs/MEDIA_PRIORITY.md)">

                    <span className={styles.libScoreBadge}>{Math.round(libScore)}</span>

                    {item.priorityFrozen && (

                        <span title="Frozen — decay paused" style={{ display: 'inline-flex', alignItems: 'center' }}>

                            <Snowflake size={11} style={{ marginLeft: 4, opacity: 0.85 }} />

                        </span>

                    )}

                </div>

                <div className={styles.detailedPriority}>

                    {item.priority !== undefined && item.priority !== 0 ? (

                        <span className={styles.priorityBadge} data-priority={item.priority > 0 ? 'high' : 'low'}>

                            {item.priority > 0 ? '↑' : '↓'} {Math.abs(item.priority)}

                        </span>

                    ) : '-'}

                </div>

                <div className={styles.detailedAdded}>{formatDate(item.addedAt)}</div>

                <div className={styles.detailedActions}>

                    <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onEdit(); }} title="Edit">

                        <Edit2 size={12} />

                    </button>

                    {item.archived ? (

                        <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onUnarchive?.(); }} title="Unarchive">

                            <Eye size={12} />

                        </button>

                    ) : (

                        <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onArchive?.(); }} title="Archive">

                            <Archive size={12} />

                        </button>

                    )}

                </div>

            </div>

        );

    }



    if (viewMode === 'list') {

        return (

            <div

                className={cn(styles.listRow, selected && styles.selected)}

                onContextMenu={onContextMenu}

                draggable

                onDragStart={handleDragStart}

            >

                <input

                    type="checkbox"

                    checked={selected}

                    onChange={(e) => onSelect(e.target.checked)}

                    onClick={(e) => e.stopPropagation()}

                    className={styles.listCheckbox}

                />

                <div

                    className={styles.listThumb}

                    style={{

                        backgroundColor: item.thumbnail?.startsWith('#') ? item.thumbnail : 'var(--surface-highlight)',

                        backgroundImage: item.thumbnail?.startsWith('data:') ? `url(${item.thumbnail})` : undefined,

                    }}

                    onClick={onClick}

                >

                    {!item.thumbnail?.startsWith('data:') && <Film size={14} />}

                </div>

                <div className={styles.listInfo} onClick={onClick}>

                    <div className={styles.listTitle}>

                        {(item as any).__missing && (

                            <span title="Missing file (path not found)" style={{ display: 'inline-flex', alignItems: 'center' }}>

                                <AlertTriangle size={12} style={{ color: 'var(--warning)', marginRight: 6 }} />

                            </span>

                        )}

                        {item.title}

                    </div>

                    <div className={styles.listMeta}>

                        <span className={cn(styles.typeBadgeSmall, styles[item.type])}>{item.type.replace('_', ' ')}</span>

                        <span>{formatDuration(item.duration)}</span>

                        <span style={{ display: 'inline-flex', alignItems: 'center', gap: 3 }}>

                            Lib {Math.round(libScore)}

                            {item.priorityFrozen && <Snowflake size={10} aria-label="frozen" />}

                        </span>

                        <span>{item.playCount || 0} plays</span>

                        {item.availableStart && <span>Start {new Date(item.availableStart).toLocaleString()}</span>}

                        {item.availableEnd && <span>Expire {new Date(item.availableEnd).toLocaleString()}</span>}

                        {item.programSeries && <span className={styles.seriesTag}>{item.programSeries}</span>}

                    </div>

                </div>

                <button className={styles.editBtn} onClick={(e) => { e.stopPropagation(); onEdit(); }}>

                    <Edit2 size={12} />

                </button>

            </div>

        );

    }



    // Grid view (default)

    const isVideoType = item.type === 'video' || item.type === 'program' || item.type === 'ad' || item.type === 'promo';

    

    return (

        <div

            className={cn(styles.card, selected && styles.selected, item.archived && styles.archived)}

            onContextMenu={onContextMenu}

            draggable

            onDragStart={handleDragStart}

        >

            <input

                type="checkbox"

                checked={selected}

                onChange={(e) => onSelect(e.target.checked)}

                onClick={(e) => e.stopPropagation()}

                className={styles.cardCheckbox}

            />

            {item.archived && <div className={styles.archivedBadge}><Archive size={10} /></div>}

            {/* Favorite indicator */}

            {isFavorite && (

                <div className={styles.favoriteIndicator}>

                    <Star size={10} fill="currentColor" />

                </div>

            )}

            <div className={styles.thumbnail} onClick={onClick}>

                <div

                    className={styles.thumbnailInner}

                    style={{

                        backgroundColor: item.thumbnail?.startsWith('#') ? item.thumbnail : 'var(--surface-highlight)',

                        backgroundImage: item.thumbnail?.startsWith('data:') ? `url(${item.thumbnail})` : undefined,

                    }}

                >

                    {!item.thumbnail?.startsWith('data:') && (item.type === 'image' ? <ImageIcon opacity={0.5} /> : <Film opacity={0.5} />)}

                </div>

                <span className={styles.duration}>{formatDuration(item.duration)}</span>

                <span className={cn(styles.typeBadge, styles[item.type])}>{item.type.replace('_', ' ')}</span>

                <span className={styles.libScoreMini} title={item.priorityFrozen ? 'Library score (frozen)' : 'Library score'}>

                    {Math.round(libScore)}

                    {item.priorityFrozen && <Snowflake size={9} style={{ marginLeft: 2, verticalAlign: 'middle' }} />}

                </span>

                {item.priority !== undefined && item.priority !== 0 && (

                    <span className={styles.priorityIndicator} data-priority={item.priority > 0 ? 'high' : 'low'}>

                        {item.priority > 0 ? '★' : '▽'}

                    </span>

                )}

                {(item.playCount || 0) > 0 && (

                    <span className={styles.playCountIndicator}>{item.playCount}×</span>

                )}

                {/* Quick action overlay */}

                <div className={styles.cardQuickActions}>

                    {onToggleFavorite && (

                        <button

                            className={cn(styles.quickActionBtn, isFavorite && styles.favorited)}

                            onClick={(e) => { e.stopPropagation(); onToggleFavorite(); }}

                            title={isFavorite ? 'Remove from favorites' : 'Add to favorites'}

                        >

                            <Star size={14} fill={isFavorite ? 'currentColor' : 'none'} />

                        </button>

                    )}

                    {isVideoType && onOpenScrubber && (

                        <button

                            className={styles.quickActionBtn}

                            onClick={(e) => { e.stopPropagation(); onOpenScrubber(); }}

                            title="Open video scrubber"

                        >

                            <AudioWaveform size={14} />

                        </button>

                    )}

                    {onOpenMetadata && (

                        <button

                            className={styles.quickActionBtn}

                            onClick={(e) => { e.stopPropagation(); onOpenMetadata(); }}

                            title="View metadata"

                        >

                            <Info size={14} />

                        </button>

                    )}

                </div>

            </div>

            <div className={styles.info}>

                <div className={styles.titleRow}>

                    <div className={styles.title} title={item.title} onClick={onClick}>

                        {(item as any).__missing && (

                            <span title="Missing file (path not found)" style={{ display: 'inline-flex', alignItems: 'center' }}>

                                <AlertTriangle size={12} style={{ color: 'var(--warning)', marginRight: 6 }} />

                            </span>

                        )}

                        {item.title}

                    </div>

                    <button className={styles.editBtn} onClick={(e) => { e.stopPropagation(); onEdit(); }}>

                        <Edit2 size={12} />

                    </button>

                </div>

                {item.client && <div className={styles.meta}>Client: {item.client}</div>}

                {item.programSeries && <div className={styles.meta}>Series: {item.programSeries}</div>}

            </div>

        </div>

    );

}



// Media Editor Component

function MediaEditor({ item, onSave, onDelete, onCancel, isPlaying = false }: {

    item: MediaItem,

    onSave: (i: MediaItem) => void,

    onDelete: (id: string) => void,

    onCancel: () => void,

    isPlaying?: boolean

}) {

    const [formData, setFormData] = useState(item);

    const [clients, setClients] = useState<AdClient[]>([]);

    const [confirmDelete, setConfirmDelete] = useState(false);

    const [showSegmentEditor, setShowSegmentEditor] = useState(false);

    const [fileMissing, setFileMissing] = useState(false);

    const [showNewClient, setShowNewClient] = useState(false);

    const [newClientName, setNewClientName] = useState('');

    const [allPrograms, setAllPrograms] = useState<string[]>([]);



    useEffect(() => {

        setClients(loadClients());

        // Load all program series for bumper association

        const stored = loadLibraryMedia([]);

        const local = loadLocalMedia();

        const series = new Set<string>();

        [...stored, ...local].forEach(m => {

            if (m.programSeries) series.add(m.programSeries);

        });

        setAllPrograms(Array.from(series).sort());



        (async () => {

            const api = (window as any).electronAPI;

            if (formData.filePath && api?.pathExists) {

                const res = await api.pathExists(formData.filePath);

                setFileMissing(!res?.exists);

            } else {

                setFileMissing(false);

            }

        })();

    }, []);



    const handleCreateClient = () => {

        if (!newClientName.trim()) return;

        const newClient: AdClient = {

            id: `client_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,

            name: newClientName.trim(),

            color: `#${Math.floor(Math.random() * 16777215).toString(16).padStart(6, '0')}`,

            rules: {

                allowedHours: [],

                allowedDays: [],

                maxPlaysPerHour: 0,

                maxPlaysPerDay: 0,

                minIntervalMinutes: 0,

                priority: 5,

            },

        };

        const updatedClients = [...clients, newClient];

        setClients(updatedClients);

        saveClients(updatedClients);

        handleChange('client', newClient.id);

        setShowNewClient(false);

        setNewClientName('');

    };



    const handleChange = (field: keyof MediaItem, value: any) => {

        setFormData({ ...formData, [field]: value });

    };



    const handleDurationChange = (value: string) => {

        const parts = value.split(':');

        let seconds = 0;

        if (parts.length === 2) {

            seconds = parseInt(parts[0]) * 60 + parseInt(parts[1]);

        } else {

            seconds = parseInt(value) || 0;

        }

        handleChange('duration', seconds);

    };



    const formatDurationInput = (seconds: number): string => {

        const m = Math.floor(seconds / 60);

        const s = Math.floor(seconds % 60);

        return `${m}:${s.toString().padStart(2, '0')}`;

    };



    if (showSegmentEditor) {

        return (

            <div className={styles.editorOverlay} style={{ zIndex: 2000 }}>

                <SegmentEditor

                    item={formData}

                    onSave={async (collections, activeCollectionId) => {

                        const activeSegments = collections.find(c => c.isActive)?.segments || collections[0]?.segments || [];

                        const updated = {

                            ...formData,

                            segmentCollections: collections,

                            activeCollectionId: activeCollectionId,

                            segments: activeSegments // Legacy support

                        };

                        setFormData(updated);

                        // Persist immediately so segments show in playlist expansion and survive restart

                        dbSaveMedia(updated).catch(console.error);

                        setShowSegmentEditor(false);

                    }}

                    onClose={() => {

                        if (isPlaying) {

                            if (window.confirm('Are you sure you want to close the segment editor? Playout is active.')) {

                                setShowSegmentEditor(false);

                            }

                        } else {

                            setShowSegmentEditor(false);

                        }

                    }}

                />

            </div>

        );

    }



    return (

        <div className={styles.editor}>

            <div className={styles.editorHeader}>

                <h3>Edit Media</h3>

                <div style={{ display: 'flex', gap: '0.5rem' }}>

                    <button className={styles.iconBtn} onClick={() => setShowSegmentEditor(true)} title="Segment Editor">

                        <Scissors size={16} />

                    </button>

                    {fileMissing && (

                        <button

                            className={styles.iconBtn}

                            onClick={async () => {

                                const api = (window as any).electronAPI;

                                if (!api?.openFileDialog) return;

                                const res = await api.openFileDialog({ properties: ['openFile'] });

                                const p = res?.filePaths?.[0];

                                if (p) {

                                    setFormData(prev => ({ ...prev, filePath: p, filename: p.split(/[\\/]/).pop() || prev.filename }));

                                    setFileMissing(false);

                                }

                            }}

                            title="File missing - click to relink"

                        >

                            <AlertTriangle size={16} />

                        </button>

                    )}

                    <button onClick={onCancel} className={styles.iconBtn}><X size={16} /></button>

                </div>

            </div>



            <div className={styles.editorGrid}>

                <div className={styles.editorColumn}>

                    <label className={styles.label}>Title</label>

                    <input

                        className={styles.input}

                        value={formData.title}

                        onChange={e => handleChange('title', e.target.value)}

                    />



                    <label className={styles.label}>Type</label>

                    <select

                        className={styles.select}

                        value={formData.type}

                        onChange={e => handleChange('type', e.target.value)}

                    >

                        <option value="program">Program</option>

                        <option value="ad">Ad</option>

                        <option value="station_id">Station ID</option>

                        <option value="bumper">Bumper</option>

                        <option value="promo">Promo</option>

                        <option value="paid_program">Paid Program</option>

                        <option value="video">Video</option>

                        <option value="image">Image</option>

                    </select>



                    <label className={styles.label}>Duration (MM:SS)</label>

                    <input

                        className={styles.input}

                        value={formatDurationInput(formData.duration)}

                        onChange={e => handleDurationChange(e.target.value)}

                        placeholder="0:30"

                    />



                    <label className={styles.label}>Priority</label>

                    <input

                        type="number"

                        className={styles.input}

                        value={formData.priority || 0}

                        onChange={e => handleChange('priority', parseInt(e.target.value) || 0)}

                        placeholder="0"

                    />



                    {formData.type === 'ad' && (

                        <>

                            <label className={styles.label}>Client</label>

                            {!showNewClient ? (

                                <div style={{ display: 'flex', gap: '0.5rem' }}>

                                    <select

                                        className={styles.select}

                                        value={formData.client || ''}

                                        onChange={e => handleChange('client', e.target.value)}

                                        style={{ flex: 1 }}

                                    >

                                        <option value="">-- Select Client --</option>

                                        {clients.map(c => (

                                            <option key={c.id} value={c.id}>{c.name}</option>

                                        ))}

                                    </select>

                                    <button

                                        className={styles.iconBtn}

                                        onClick={() => setShowNewClient(true)}

                                        title="Create new client"

                                    >

                                        <Plus size={14} />

                                    </button>

                                </div>

                            ) : (

                                <div style={{ display: 'flex', gap: '0.5rem' }}>

                                    <input

                                        className={styles.input}

                                        value={newClientName}

                                        onChange={e => setNewClientName(e.target.value)}

                                        placeholder="New client name..."

                                        style={{ flex: 1 }}

                                        onKeyDown={e => { if (e.key === 'Enter') handleCreateClient(); }}

                                    />

                                    <button

                                        className={styles.iconBtn}

                                        onClick={handleCreateClient}

                                        title="Create"

                                    >

                                        <Check size={14} />

                                    </button>

                                    <button

                                        className={styles.iconBtn}

                                        onClick={() => { setShowNewClient(false); setNewClientName(''); }}

                                        title="Cancel"

                                    >

                                        <X size={14} />

                                    </button>

                                </div>

                            )}

                        </>

                    )}



                    {formData.type === 'bumper' && (

                        <>

                            <label className={styles.label}>Bumper Type</label>

                            <select

                                className={styles.select}

                                value={formData.bumperSubtype || 'generic'}

                                onChange={e => handleChange('bumperSubtype', e.target.value)}

                            >

                                <option value="generic">Generic</option>

                                <option value="breaking">Breaking News</option>

                                <option value="program">Program-Specific</option>

                            </select>

                            {formData.bumperSubtype === 'program' && (

                                <>

                                    <label className={styles.label}>Associated Program</label>

                                    <select

                                        className={styles.select}

                                        value={formData.bumperProgramId || ''}

                                        onChange={e => handleChange('bumperProgramId', e.target.value)}

                                    >

                                        <option value="">-- Select Program --</option>

                                        {allPrograms.map(p => (

                                            <option key={p} value={p}>{p}</option>

                                        ))}

                                    </select>

                                </>

                            )}

                        </>

                    )}

                </div>



                <div className={styles.editorColumn}>

                    {(formData.type === 'program' || formData.type === 'paid_program') && (

                        <>

                            <label className={styles.label}>Series Name</label>

                            <input

                                className={styles.input}

                                value={formData.programSeries || ''}

                                onChange={e => handleChange('programSeries', e.target.value)}

                                placeholder="e.g. Morning News"

                            />

                            <label className={styles.label}>Episode Date</label>

                            <input

                                type="date"

                                className={styles.input}

                                value={formData.episodeDate || ''}

                                onChange={e => handleChange('episodeDate', e.target.value)}

                            />

                        </>

                    )}



                    <label className={styles.label}>Date Made</label>

                    <input

                        type="date"

                        className={styles.input}

                        value={formData.dateMade?.split('T')[0] || ''}

                        onChange={e => handleChange('dateMade', e.target.value)}

                    />



                    <label className={styles.label}>Notes</label>

                    <textarea

                        className={styles.textarea}

                        value={formData.notes || ''}

                        onChange={e => handleChange('notes', e.target.value)}

                        placeholder="Any notes about this media..."

                        rows={3}

                    />



                    <label className={styles.label}>Source</label>

                    <input

                        className={styles.input}

                        value={formData.src || ''}

                        disabled

                        style={{ opacity: 0.6, fontSize: '0.75rem' }}

                        title={formData.src}

                    />



                    <div className={styles.statsRow}>

                        <div className={styles.stat}>

                            <span className={styles.statLabel}>Added:</span>

                            <span>{formData.addedAt ? new Date(formData.addedAt).toLocaleDateString() : 'Unknown'}</span>

                        </div>

                        <div className={styles.stat}>

                            <span className={styles.statLabel}>Played:</span>

                            <span>{formData.playCount || 0} times</span>

                        </div>

                    </div>



                    <label className={styles.checkboxLabel}>

                        <input

                            type="checkbox"

                            checked={formData.archived || false}

                            onChange={e => handleChange('archived', e.target.checked)}

                        />

                        <Archive size={14} />

                        <span>Archived (hidden from library but kept in playlists)</span>

                    </label>

                </div>

            </div>



            <div className={styles.editorActions}>

                {!confirmDelete ? (

                    <>

                        <button className={styles.deleteBtn} onClick={() => setConfirmDelete(true)}>

                            Delete

                        </button>

                        <button className={styles.saveBtn} onClick={() => onSave(formData)}>

                            <Check size={14} /> Save

                        </button>

                    </>

                ) : (

                    <>

                        <span style={{ color: 'var(--danger)', fontSize: '0.85rem' }}>Delete this item?</span>

                        <button className={styles.cancelBtn} onClick={() => setConfirmDelete(false)}>

                            Cancel

                        </button>

                        <button className={styles.confirmDeleteBtn} onClick={() => onDelete(item.id)}>

                            Confirm Delete

                        </button>

                    </>

                )}

            </div>

        </div>

    );

}



// ============================================

// ASSET MANAGER COMPONENT

// ============================================



interface AssetManagerProps {

    allMedia: MediaItem[];

    onMediaUpdate?: (updates: MediaItem[]) => void;

}



function AssetManager({ allMedia, onMediaUpdate }: AssetManagerProps) {

    const [casperMediaPath, setCasperMediaPath] = useState<string>('');

    const [casperFiles, setCasperFiles] = useState<Array<{ filename: string; filePath: string; size: number; mtime: string }>>([]);

    const [selectedMedia, setSelectedMedia] = useState<Set<string>>(new Set());

    const [copying, setCopying] = useState(false);

    const [copyResults, setCopyResults] = useState<Array<{ filename: string; success: boolean; alreadyExists?: boolean; error?: string }>>([]);

    const [showResults, setShowResults] = useState(false);

    const [campaigns, setCampaigns] = useState<Campaign[]>(() => loadCampaigns());

    const [clients, setClients] = useState<AdClient[]>(() => loadClients());

    const [bulkType, setBulkType] = useState<MediaType | ''>('');

    const [bulkCampaign, setBulkCampaign] = useState<string>('');

    const [filterCampaign, setFilterCampaign] = useState<string>('');

    const [filterClient, setFilterClient] = useState<string>('');

    const env = getEnvironment();



    const electronAPI = (window as any).electronAPI;



    // Load CasperCG media info on mount

    useEffect(() => {

        if (env.isElectron && electronAPI) {

            electronAPI.getCasperCGMediaPath().then((path: string) => {

                setCasperMediaPath(path);

            });

            refreshCasperMedia();

        }

    }, [env.isElectron]);



    const refreshCasperMedia = async () => {

        if (env.isElectron && electronAPI) {

            const result = await electronAPI.listCasperCGMedia();

            if (result.success) {

                setCasperFiles(result.files);

            }

        }

    };



    // Get media items with filePath that can be copied

    let copyableMedia = allMedia.filter(m => m.filePath);



    // Apply filters

    if (filterCampaign) {

        copyableMedia = copyableMedia.filter(m => m.adMetadata?.campaignId === filterCampaign);

    }

    if (filterClient) {

        if (filterClient === '__unassigned__') {

            copyableMedia = copyableMedia.filter(m => !(m.adMetadata?.client || m.client));

        } else {

            copyableMedia = copyableMedia.filter(m => m.adMetadata?.client === filterClient || m.client === filterClient);

        }

    }



    // Check if a media item is already in CasperCG media folder

    const isInCasperMedia = (filename: string) => {

        return casperFiles.some(f => f.filename.toLowerCase() === filename.toLowerCase());

    };



    const toggleSelectAll = () => {

        if (selectedMedia.size === copyableMedia.length) {

            setSelectedMedia(new Set());

        } else {

            setSelectedMedia(new Set(copyableMedia.map(m => m.id)));

        }

    };



    const handleCopyToCasper = async () => {

        if (selectedMedia.size === 0) return;



        const filePaths = copyableMedia

            .filter(m => selectedMedia.has(m.id) && m.filePath)

            .map(m => m.filePath as string);



        if (filePaths.length === 0) return;



        setCopying(true);

        setShowResults(false);



        try {

            if (env.isElectron && electronAPI) {

                const result = await electronAPI.copyFilesToCasperCG(filePaths);

                setCopyResults(result.results.map((r: any) => ({

                    filename: r.filename,

                    success: r.success,

                    alreadyExists: r.alreadyExists,

                    error: r.error,

                })));

                setShowResults(true);



                // Refresh CasperCG media list

                await refreshCasperMedia();



                // Clear selection

                setSelectedMedia(new Set());

            }

        } catch (error) {

            console.error('Failed to copy files:', error);

        } finally {

            setCopying(false);

        }

    };



    const handleCopyAll = async () => {

        const filePaths = copyableMedia

            .filter(m => m.filePath)

            .map(m => m.filePath as string);



        if (filePaths.length === 0) return;



        setCopying(true);

        setShowResults(false);



        try {

            if (env.isElectron && electronAPI) {

                const result = await electronAPI.copyFilesToCasperCG(filePaths);

                setCopyResults(result.results.map((r: any) => ({

                    filename: r.filename,

                    success: r.success,

                    alreadyExists: r.alreadyExists,

                    error: r.error,

                })));

                setShowResults(true);

                await refreshCasperMedia();

            }

        } catch (error) {

            console.error('Failed to copy files:', error);

        } finally {

            setCopying(false);

        }

    };



    const formatSize = (bytes: number) => {

        if (bytes < 1024) return `${bytes} B`;

        if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;

        if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;

        return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;

    };



    const handleBulkAssign = () => {

        if (selectedMedia.size === 0) {

            alert('Select media items first');

            return;

        }



        const updates: MediaItem[] = [];

        allMedia.forEach(item => {

            if (selectedMedia.has(item.id)) {

                const updated: MediaItem = { ...item };

                if (bulkType) {

                    updated.type = bulkType as MediaType;

                }

                if (bulkCampaign) {

                    updated.adMetadata = {

                        ...updated.adMetadata,

                        campaignId: bulkCampaign,

                    };

                    const campaign = campaigns.find(c => c.id === bulkCampaign);

                    if (campaign) {

                        updated.adMetadata.client = campaign.client;

                        updated.client = campaign.client;

                    }

                }

                updates.push(updated);

            }

        });



        // Save to DB

        updates.forEach(item => {

            dbSaveMedia(item).catch(console.error);

        });



        // Notify parent component

        if (onMediaUpdate) {

            onMediaUpdate(updates);

        }



        setSelectedMedia(new Set());

        setBulkType('');

        setBulkCampaign('');

    };



    if (!env.isElectron) {

        return (

            <div className={styles.assetManager}>

                <div className={styles.assetHeader}>

                    <h3>Asset Manager</h3>

                    <p style={{ color: 'var(--text-muted)' }}>

                        Asset management is only available in the Electron app.

                    </p>

                </div>

            </div>

        );

    }



    return (

        <div className={styles.assetManager}>

            <div className={styles.assetHeader}>

                <h3>📁 Asset Manager</h3>

                <p style={{ color: 'var(--text-muted)', marginBottom: '1rem' }}>

                    Copy media files to CasperCG media folder for playout

                </p>



                <div className={styles.casperPathInfo}>

                    <strong>CasperCG Media Path:</strong>

                    <code>{casperMediaPath || 'Loading...'}</code>

                </div>

            </div>



            {/* Copy Results */}

            {showResults && copyResults.length > 0 && (

                <div className={styles.copyResults}>

                    <div className={styles.copyResultsHeader}>

                        <span>Copy Results</span>

                        <button onClick={() => setShowResults(false)}><X size={14} /></button>

                    </div>

                    <div className={styles.copyResultsList}>

                        {copyResults.map((r, i) => (

                            <div key={i} className={cn(styles.copyResult, r.success ? styles.success : styles.error)}>

                                {r.success ? (

                                    r.alreadyExists ? (

                                        <><Check size={12} /> {r.filename} (already exists)</>

                                    ) : (

                                        <><Check size={12} /> {r.filename} copied</>

                                    )

                                ) : (

                                    <><X size={12} /> {r.filename}: {r.error}</>

                                )}

                            </div>

                        ))}

                    </div>

                </div>

            )}



            {/* Filters and Bulk Assignment */}

            <div className={styles.assetFilters}>

                <div className={styles.filterGroup}>

                    <label>Filter by Campaign:</label>

                    <select value={filterCampaign} onChange={(e) => setFilterCampaign(e.target.value)}>

                        <option value="">All Campaigns</option>

                        {campaigns.map(c => (

                            <option key={c.id} value={c.id}>{c.name}</option>

                        ))}

                    </select>

                </div>

                <div className={styles.filterGroup}>

                    <label>Filter by Client:</label>

                    <select value={filterClient} onChange={(e) => setFilterClient(e.target.value)}>

                        <option value="">All Clients</option>

                        <option value="__unassigned__">Unassigned</option>

                        {clients.map(c => (

                            <option key={c.id} value={c.id}>{c.name}</option>

                        ))}

                    </select>

                </div>

            </div>



            {/* Bulk Assignment */}

            {selectedMedia.size > 0 && (

                <div className={styles.bulkAssignment}>

                    <h4>Bulk Assign ({selectedMedia.size} selected)</h4>

                    <div className={styles.bulkControls}>

                        <div className={styles.bulkField}>

                            <label>Type:</label>

                            <select value={bulkType} onChange={(e) => setBulkType(e.target.value as MediaType | '')}>

                                <option value="">Keep Current</option>

                                <option value="program">Program</option>

                                <option value="ad">Ad</option>

                                <option value="bumper">Bumper</option>

                                <option value="station_id">Station ID</option>

                                <option value="promo">Promo</option>

                            </select>

                        </div>

                        <div className={styles.bulkField}>

                            <label>Campaign:</label>

                            <select value={bulkCampaign} onChange={(e) => setBulkCampaign(e.target.value)}>

                                <option value="">None</option>

                                {campaigns.map(c => (

                                    <option key={c.id} value={c.id}>{c.name}</option>

                                ))}

                            </select>

                        </div>

                        <button className={styles.applyBtn} onClick={handleBulkAssign}>

                            Apply

                        </button>

                    </div>

                </div>

            )}



            {/* Actions */}

            <div className={styles.assetActions}>

                <button

                    className={styles.copyAllBtn}

                    onClick={handleCopyAll}

                    disabled={copying || copyableMedia.length === 0}

                >

                    {copying ? '⏳ Copying...' : `📋 Copy All Media (${copyableMedia.length})`}

                </button>

                <button

                    className={styles.copySelectedBtn}

                    onClick={handleCopyToCasper}

                    disabled={copying || selectedMedia.size === 0}

                >

                    📋 Copy Selected ({selectedMedia.size})

                </button>

                <button className={styles.refreshBtn} onClick={refreshCasperMedia}>

                    🔄 Refresh

                </button>

            </div>



            {/* Media Library Items */}

            <div className={styles.assetSection}>

                <div className={styles.assetSectionHeader}>

                    <h4>Media Library ({copyableMedia.length} items with file paths)</h4>

                    <button onClick={toggleSelectAll} className={styles.selectAllBtn}>

                        {selectedMedia.size === copyableMedia.length ? <CheckSquare size={14} /> : <Square size={14} />}

                        {selectedMedia.size === copyableMedia.length ? 'Deselect All' : 'Select All'}

                    </button>

                </div>

                <div className={styles.assetList}>

                    {copyableMedia.map(item => {

                        const inCasper = isInCasperMedia(item.filename || item.title);

                        return (

                            <div

                                key={item.id}

                                className={cn(styles.assetItem, selectedMedia.has(item.id) && styles.selected, inCasper && styles.inCasper)}

                                onClick={() => {

                                    const newSelected = new Set(selectedMedia);

                                    if (newSelected.has(item.id)) {

                                        newSelected.delete(item.id);

                                    } else {

                                        newSelected.add(item.id);

                                    }

                                    setSelectedMedia(newSelected);

                                }}

                            >

                                <input

                                    type="checkbox"

                                    checked={selectedMedia.has(item.id)}

                                    onChange={() => { }}

                                />

                                <div className={styles.assetItemInfo}>

                                    <div className={styles.assetItemRow}>

                                        <span className={styles.assetItemName}>{item.filename || item.title}</span>

                                        <span className={styles.assetItemType}>{item.type}</span>

                                        {item.adMetadata?.campaignId && (

                                            <span className={styles.assetItemCampaign}>

                                                {campaigns.find(c => c.id === item.adMetadata?.campaignId)?.name || 'Campaign'}

                                            </span>

                                        )}

                                    </div>

                                    <span className={styles.assetItemPath}>{item.filePath}</span>

                                </div>

                                {inCasper && (

                                    <span className={styles.inCasperBadge}>✓ In CasperCG</span>

                                )}

                            </div>

                        );

                    })}

                    {copyableMedia.length === 0 && (

                        <div className={styles.emptyMessage}>

                            No media with file paths. Import some files first.

                        </div>

                    )}

                </div>

            </div>



            {/* CasperCG Media Folder Contents */}

            <div className={styles.assetSection}>

                <div className={styles.assetSectionHeader}>

                    <h4>CasperCG Media Folder ({casperFiles.length} files)</h4>

                </div>

                <div className={styles.assetList}>

                    {casperFiles.map(file => (

                        <div key={file.filePath} className={styles.casperFile}>

                            <Film size={14} />

                            <span className={styles.casperFileName}>{file.filename}</span>

                            <span className={styles.casperFileSize}>{formatSize(file.size)}</span>

                        </div>

                    ))}

                    {casperFiles.length === 0 && (

                        <div className={styles.emptyMessage}>

                            No files in CasperCG media folder.

                        </div>

                    )}

                </div>

            </div>

        </div>

    );

}

