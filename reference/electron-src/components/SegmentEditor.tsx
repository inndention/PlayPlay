import React, { useState, useRef, useEffect, useCallback, useMemo } from 'react';
import { useSafeAsync } from '../hooks/useSafeAsync';
import { MediaItem, Segment, SegmentCollection } from '../lib/types';
import { resolveMediaUrl } from '../lib/fileResolver';
import { extractFrames, detectScenes } from '../lib/mediaUtils';
import { dbSaveMedia } from '../lib/mediaDbClient';
import { 
    ZoomIn, ZoomOut, Scissors, Plus, Play, Pause, X, Check, Trash2, 
    Save, AudioWaveform, HardDrive, Database, Layers, Settings,
    ChevronDown, ChevronUp, Copy, Edit2, Library, FileOutput
} from 'lucide-react';
import { formatDuration, generateId } from '../lib/utils';
import { persistentLogger } from '../lib/persistentLogger';
import { logError } from '../lib/errorBus';
import styles from './SegmentEditor.module.css';

interface SegmentEditorProps {
    item: MediaItem;
    onSave: (collections: SegmentCollection[], activeCollectionId?: string) => void;
    onClose: () => void;
    onExportSegmentsToLibrary?: (newItems: MediaItem[]) => void;
}

export function SegmentEditor({ item, onSave, onClose, onExportSegmentsToLibrary }: SegmentEditorProps) {
    const { safeSetState } = useSafeAsync();
    const [zoom, setZoom] = useState(10); // px per second
    const [currentTime, setCurrentTime] = useState(0);
    const [isPlaying, setIsPlaying] = useState(false);
    const [collections, setCollections] = useState<SegmentCollection[]>(() => {
        if (item.segmentCollections && item.segmentCollections.length > 0) {
            return item.segmentCollections;
        }
        // Create default collection from legacy segments
        if (item.segments && item.segments.length > 0) {
            return [{
                id: `segcol_${generateId()}`,
                name: 'Default',
                segments: item.segments,
                isActive: true
            }];
        }
        return [{
            id: `segcol_${generateId()}`,
            name: 'Default',
            segments: [],
            isActive: true
        }];
    });
    const [activeCollectionId, setActiveCollectionId] = useState<string>(() => {
        if (item.activeCollectionId) return item.activeCollectionId;
        const active = collections.find(c => c.isActive);
        return active?.id || collections[0]?.id || '';
    });
    const [frames, setFrames] = useState<string[]>([]);
    const [selectedSegmentId, setSelectedSegmentId] = useState<string | null>(null);
    const [autoSegmentCount, setAutoSegmentCount] = useState(10);
    const [autoSegmentOverlap, setAutoSegmentOverlap] = useState(0);
    const [previewFrame, setPreviewFrame] = useState<{ url: string; x: number; time: number } | null>(null);
    const [isGeneratingPreview, setIsGeneratingPreview] = useState(false);
    const [loadInRAM, setLoadInRAM] = useState(false);
    const [videoLoadedInRAM, setVideoLoadedInRAM] = useState(false);
    const [showCollectionSelector, setShowCollectionSelector] = useState(false);
    const [editingCollectionName, setEditingCollectionName] = useState<string | null>(null);
    const [newCollectionName, setNewCollectionName] = useState('');
    // Hold-to-zoom: press and hold on seekbar magnifies that region
    const [isHoldZooming, setIsHoldZooming] = useState(false);
    const [holdZoomCenter, setHoldZoomCenter] = useState(0); // time at center of magnified view
    const [holdZoom, setHoldZoom] = useState(10); // normal zoom level saved
    const holdZoomTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
    // Fine-tune panel for selected segment
    const [showFineTune, setShowFineTune] = useState(false);

    const videoRef = useRef<HTMLVideoElement>(null);
    const previewVideoRef = useRef<HTMLVideoElement>(null);
    const previewCanvasRef = useRef<HTMLCanvasElement>(null);
    const timelineRef = useRef<HTMLDivElement>(null);
    const containerRef = useRef<HTMLDivElement>(null);
    const waveformRef = useRef<HTMLCanvasElement>(null);
    const videoBlobRef = useRef<Blob | null>(null);
    const dragRef = useRef<null | {
        id: string;
        mode: 'move' | 'start' | 'end' | 'joint'; // joint = boundary shared by two adjacent segments
        jointNextId?: string; // ID of segment to the right when mode=joint
        startX: number;
        startStart: number;
        startEnd: number;
        jointNextStart?: number; // startTime of right segment
    }>(null);
    const workerRef = useRef<Worker | null>(null);
    const lastPreviewRequestRef = useRef(0);

    // Initialize media worker
    useEffect(() => {
        workerRef.current = new Worker(new URL('../lib/mediaWorker.ts', import.meta.url), { type: 'module' });
        
        workerRef.current.onmessage = (e) => {
            if (e.data.type === 'preview_generated') {
                const { blob, time, clientX } = e.data;
                const url = URL.createObjectURL(blob);
                
                safeSetState(() => {
                    setPreviewFrame(prev => {
                        // Revoke old URL if it exists
                        if (prev && prev.url.startsWith('blob:')) {
                            URL.revokeObjectURL(prev.url);
                        }
                        return { url, time, x: clientX };
                    });
                    setIsGeneratingPreview(false);
                });
            }
        };

        return () => {
            workerRef.current?.terminate();
        };
    }, [safeSetState]);

    const activeCollection = useMemo(() => 
        collections.find(c => c.id === activeCollectionId) || collections[0],
        [collections, activeCollectionId]
    );
    const segments = activeCollection?.segments || [];

    // Get filename for auto-naming
    const filename = useMemo(() => {
        return item.filename || item.title || 'Untitled';
    }, [item.filename, item.title]);

    // Resolve a reliable playable URL via app-wide resolver (filePath or src)
    const [resolvedSrc, setResolvedSrc] = useState<string>('');
    const [urlResolutionDone, setUrlResolutionDone] = useState(false);

    useEffect(() => {
        if (!item) return;
        let cancelled = false;
        (async () => {
            try {
                let url = await resolveMediaUrl(item).catch(() => '');
                if (cancelled) return;
                if (!url && item?.filePath && (window as any).electronAPI?.getFileUrl) {
                    try {
                        url = await (window as any).electronAPI.getFileUrl(item.filePath) || '';
                    } catch (_) { /* ignore */ }
                }
                if (cancelled) return;
                safeSetState(() => {
                    setResolvedSrc(url || item?.src || '');
                    setUrlResolutionDone(true);
                });
            } catch (e) {
                if (cancelled) return;
                persistentLogger.warn('SegmentEditor: resolve URL failed', undefined, 'SegmentEditor');
                safeSetState(() => {
                    setResolvedSrc(item?.src || '');
                    setUrlResolutionDone(true);
                });
            }
        })();
        return () => { cancelled = true; };
    }, [item, safeSetState]);

    // Load video into RAM
    useEffect(() => {
        if (loadInRAM && !videoLoadedInRAM && resolvedSrc && !resolvedSrc.startsWith('signal:')) {
            (async () => {
                try {
                    const response = await fetch(resolvedSrc);
                    const blob = await response.blob();
                    videoBlobRef.current = blob;
                    const url = URL.createObjectURL(blob);
                    safeSetState(() => {
                        setResolvedSrc(url);
                        setVideoLoadedInRAM(true);
                    });
                } catch (e) {
                    persistentLogger.warn('Failed to load video into RAM: ' + String(e), undefined, 'SegmentEditor');
                }
            })();
        }
        return () => {
            if (videoLoadedInRAM && videoBlobRef.current && resolvedSrc?.startsWith('blob:')) {
                try {
                    URL.revokeObjectURL(resolvedSrc);
                } catch (_) { /* ignore */ }
            }
        };
    }, [loadInRAM, resolvedSrc, videoLoadedInRAM, safeSetState]);

    // Generate timeline frames once we have a resolved URL (fetch + extractFrames; avoid main-process read for large files)
    useEffect(() => {
        const url = resolvedSrc?.trim() || '';
        if (!url || url.startsWith('signal:')) return;

        let cancelled = false;
        const loadFrames = async () => {
            try {
                const response = await fetch(url);
                if (!response.ok || cancelled) return;
                const blob = await response.blob();
                if (cancelled) return;
                const file = new File([blob], 'temp.mp4', { type: blob.type || 'video/mp4' });
                const extracted = await extractFrames(file, 50);
                if (!cancelled && Array.isArray(extracted)) safeSetState(() => setFrames(extracted));
            } catch (e) {
                if (!cancelled) persistentLogger.warn('Could not load frames for timeline: ' + String(e), undefined, 'SegmentEditor');
            }
        };
        loadFrames();
        return () => { cancelled = true; };
    }, [resolvedSrc, safeSetState]);

    // Generate waveform
    const generateWaveform = useCallback(() => {
        const canvas = waveformRef.current;
        if (!canvas) return;

        const ctx = canvas.getContext('2d');
        if (!ctx) return;

        const width = canvas.width;
        const height = canvas.height;
        const samples = 300;
        const barWidth = width / samples;

        ctx.clearRect(0, 0, width, height);
        ctx.fillStyle = '#3b82f6';

        for (let i = 0; i < samples; i++) {
            const base = Math.sin(i * 0.1) * 0.3;
            const noise = Math.random() * 0.4;
            const spike = Math.random() > 0.95 ? Math.random() * 0.3 : 0;
            const value = Math.min(1, Math.abs(base + noise + spike));
            const barHeight = value * height * 0.8;
            const x = i * barWidth;
            const y = (height - barHeight) / 2;

            // Highlight segments
            const time = (i / samples) * (item.duration || 0);
            const inSegment = segments.some(s => time >= s.startTime && time <= s.endTime);
            ctx.fillStyle = inSegment ? '#22c55e' : '#3b82f6';

            ctx.fillRect(x, y, Math.max(1, barWidth - 1), barHeight);
        }

        // Draw playhead
        const progress = item.duration > 0 ? currentTime / item.duration : 0;
        const playheadX = progress * width;
        ctx.fillStyle = '#ef4444';
        ctx.fillRect(playheadX - 1, 0, 2, height);
    }, [item.duration, currentTime, segments]);

    useEffect(() => {
        const canvas = waveformRef.current;
        if (canvas) {
            const resizeObserver = new ResizeObserver(() => {
                canvas.width = canvas.offsetWidth;
                canvas.height = canvas.offsetHeight;
                generateWaveform();
            });
            resizeObserver.observe(canvas);
            return () => resizeObserver.disconnect();
        }
    }, [generateWaveform]);

    // Video Sync
    useEffect(() => {
        const v = videoRef.current;
        if (!v) return;

        const updateTime = () => setCurrentTime(v.currentTime);
        v.addEventListener('timeupdate', updateTime);
        return () => v.removeEventListener('timeupdate', updateTime);
    }, []);

    useEffect(() => {
        const v = videoRef.current;
        if (v && Math.abs(v.currentTime - currentTime) > 0.5 && !isPlaying) {
            v.currentTime = currentTime;
        }
    }, [currentTime, isPlaying]);

    const handlePlayPause = () => {
        const v = videoRef.current;
        if (v) {
            if (isPlaying) v.pause();
            else v.play();
            setIsPlaying(!isPlaying);
        }
    };

    const clamp = (v: number, min: number, max: number) => Math.max(min, Math.min(max, v));
    const minSegLen = 0.2; // seconds

    const timeFromClientX = (clientX: number) => {
        const el = containerRef.current;
        if (!el) return 0;
        const rect = el.getBoundingClientRect();
        const offsetX = clientX - rect.left + el.scrollLeft;
        return clamp(offsetX / zoom, 0, item.duration || 0);
    };

    const beginDrag = (e: React.MouseEvent, id: string, mode: 'move' | 'start' | 'end') => {
        try {
            e.preventDefault();
            e.stopPropagation();
            const seg = segments.find(s => s.id === id);
            if (!seg) return;
            const start = Number(seg.startTime) || 0;
            const end = Number(seg.endTime) ?? start;
            if (!Number.isFinite(start) || !Number.isFinite(end)) return;
            setSelectedSegmentId(id);
            dragRef.current = {
                id,
                mode,
                startX: e.clientX,
                startStart: start,
                startEnd: Math.max(end, start),
            };
            window.addEventListener('mousemove', onDragMove);
            window.addEventListener('mouseup', endDrag);
            document.body.style.userSelect = 'none';
            document.body.style.cursor = mode === 'move' ? 'grabbing' : 'ew-resize';
        } catch (err) {
            persistentLogger.warn('SegmentEditor beginDrag: ' + String(err), undefined, 'SegmentEditor');
        }
    };

    // Joint drag: drag the boundary between two adjacent segments simultaneously
    const beginJointDrag = (e: React.MouseEvent, leftSeg: Segment, rightSeg: Segment) => {
        e.preventDefault();
        e.stopPropagation();
        setSelectedSegmentId(leftSeg.id);
        dragRef.current = {
            id: leftSeg.id,
            mode: 'joint',
            jointNextId: rightSeg.id,
            startX: e.clientX,
            startStart: leftSeg.startTime,
            startEnd: leftSeg.endTime,
            jointNextStart: rightSeg.startTime,
        };
        window.addEventListener('mousemove', onDragMove);
        window.addEventListener('mouseup', endDrag);
        document.body.style.userSelect = 'none';
        document.body.style.cursor = 'ew-resize';
    };

    // Hold-to-zoom: pressing and holding on the timeline zooms in around that point for fine adjustment.
    const handleTimelineMouseDown = (e: React.MouseEvent) => {
        const time = timeFromClientX(e.clientX);
        holdZoomTimerRef.current = setTimeout(() => {
            setHoldZoom(zoom);
            setHoldZoomCenter(time);
            setIsHoldZooming(true);
            setShowFineTune(true);
            setZoom(prev => Math.min(prev * 10, 1000));
            // Scroll so the zoomed point stays centered
            requestAnimationFrame(() => {
                if (containerRef.current) {
                    const newZoom = Math.min(zoom * 10, 1000);
                    const targetScrollX = time * newZoom - containerRef.current.clientWidth / 2;
                    containerRef.current.scrollLeft = Math.max(0, targetScrollX);
                }
            });
        }, 320);
    };

    const handleTimelineMouseUp = () => {
        if (holdZoomTimerRef.current) {
            clearTimeout(holdZoomTimerRef.current);
            holdZoomTimerRef.current = null;
        }
        if (isHoldZooming) {
            setIsHoldZooming(false);
            setShowFineTune(false);
            setZoom(holdZoom);
        }
    };

    const onDragMove = (ev: MouseEvent) => {
        try {
            const d = dragRef.current;
            if (!d) return;
            const dx = ev.clientX - d.startX;
            const dt = dx / zoom;
            const time = timeFromClientX(ev.clientX);
            if (resolvedSrc && !resolvedSrc.startsWith('signal:')) {
                generatePreviewFrame(time, ev.clientX);
            }

            setCollections(prev => prev.map(collection => {
                if (collection.id !== activeCollectionId) return collection;
                return {
                    ...collection,
                    segments: collection.segments.map(s => {
                        if (d.mode === 'joint') {
                            const jointTime = clamp(d.startEnd + dt, 0, item.duration || 0);
                            if (s.id === d.id) return { ...s, endTime: jointTime };
                            if (s.id === d.jointNextId) return { ...s, startTime: jointTime };
                            return s;
                        }
                        if (s.id !== d.id) return s;
                        let start = d.startStart;
                        let end = d.startEnd;
                        if (d.mode === 'move') {
                            const len = end - start;
                            start = clamp(d.startStart + dt, 0, (item.duration || 0) - len);
                            end = start + len;
                        } else if (d.mode === 'start') {
                            start = clamp(d.startStart + dt, 0, end - minSegLen);
                        } else {
                            end = clamp(d.startEnd + dt, start + minSegLen, item.duration || 0);
                        }
                        return { ...s, startTime: start, endTime: end };
                    })
                };
            }));
        } catch (err) {
            persistentLogger.warn('SegmentEditor onDragMove: ' + String(err), undefined, 'SegmentEditor');
        }
    };

    const endDrag = () => {
        dragRef.current = null;
        window.removeEventListener('mousemove', onDragMove);
        window.removeEventListener('mouseup', endDrag);
        document.body.style.userSelect = '';
        document.body.style.cursor = '';
        setPreviewFrame(null);
        if (holdZoomTimerRef.current) {
            clearTimeout(holdZoomTimerRef.current);
            holdZoomTimerRef.current = null;
        }
    };

    const handleZoom = (delta: number) => {
        setZoom(prev => Math.max(1, Math.min(200, prev + delta)));
    };

    const handleTimelineClick = (e: React.MouseEvent) => {
        try {
            if (!containerRef.current) return;
            const rect = containerRef.current.getBoundingClientRect();
            const offsetX = e.clientX - rect.left + containerRef.current.scrollLeft;
            const time = Math.max(0, Math.min(item.duration || 0, offsetX / zoom));
            setCurrentTime(time);
            if (videoRef.current) videoRef.current.currentTime = time;
        } catch (err) {
            persistentLogger.warn('SegmentEditor handleTimelineClick: ' + String(err), undefined, 'SegmentEditor');
        }
    };

    const handleAddSegment = () => {
        const start = Math.max(0, currentTime - 5);
        const end = Math.min(item.duration || 0, currentTime + 5);
        const segmentNum = segments.length + 1;
        const newSeg: Segment = {
            id: `seg_${Date.now()}`,
            label: `${filename} segment${segmentNum}`,
            startTime: start,
            endTime: end,
            color: '#3b82f6'
        };
        
        setCollections(prev => prev.map(collection => {
            if (collection.id !== activeCollectionId) return collection;
            return {
                ...collection,
                segments: [...collection.segments, newSeg]
            };
        }));
        setSelectedSegmentId(newSeg.id);
    };

    const handleAutoSegment = async () => {
        const sourceUrl = resolvedSrc || item.src;
        if (sourceUrl && !sourceUrl.startsWith('signal:')) {
            try {
                const response = await fetch(sourceUrl);
                const blob = await response.blob();
                const file = new File([blob], item.filename || 'temp.mp4', { type: blob.type });
                const detected = await detectScenes(file, autoSegmentCount);
                
                // Apply overlap if specified
                const segmentsWithOverlap = detected.map((seg, index) => {
                    if (index === 0) return seg;
                    const prevSeg = detected[index - 1];
                    const overlap = autoSegmentOverlap;
                    const newStart = Math.max(0, seg.startTime - overlap);
                    const newEnd = Math.min(item.duration || 0, seg.endTime + overlap);
                    return {
                        ...seg,
                        startTime: newStart,
                        endTime: newEnd
                    };
                });

                // Auto-name segments
                const autoNamedSegments = segmentsWithOverlap.map((seg, index) => ({
                    ...seg,
                    label: `${filename} segment${index + 1}`
                }));

                setCollections(prev => prev.map(collection => {
                    if (collection.id !== activeCollectionId) return collection;
                    return {
                        ...collection,
                        segments: autoNamedSegments
                    };
                }));
            } catch (e) {
                persistentLogger.warn('Auto-segmentation failed: ' + String(e), undefined, 'SegmentEditor');
                alert("Could not run auto-segmentation on this source. Check that the file is accessible.");
            }
        } else {
            alert("Auto-segmentation requires a valid video file source.");
        }
    };

    // Generate preview frame on hover/scrub (timeout so we never hang; only when video is ready)
    const generatePreviewFrame = useCallback(async (time: number, clientX: number) => {
        const video = previewVideoRef.current;
        if (!video || !workerRef.current || isGeneratingPreview) return;
        const src = (resolvedSrc || item?.src || '').trim();
        if (!src || src.startsWith('signal:')) return;
        if (video.readyState < 2) return; // HAVE_CURRENT_DATA

        setIsGeneratingPreview(true);
        try {
            const duration = Number.isFinite(video.duration) ? video.duration : (item?.duration ?? 0);
            const t = Math.max(0, Math.min(time, duration || 0));
            video.currentTime = t;

            await new Promise<void>((resolve, reject) => {
                const timeout = setTimeout(() => {
                    video.removeEventListener('seeked', onSeeked);
                    video.removeEventListener('error', onErr);
                    reject(new Error('Preview seek timeout'));
                }, 2500);
                const onSeeked = () => {
                    clearTimeout(timeout);
                    video.removeEventListener('seeked', onSeeked);
                    video.removeEventListener('error', onErr);
                    resolve();
                };
                const onErr = () => {
                    clearTimeout(timeout);
                    video.removeEventListener('seeked', onSeeked);
                    video.removeEventListener('error', onErr);
                    reject(new Error('Preview video error'));
                };
                video.addEventListener('seeked', onSeeked);
                video.addEventListener('error', onErr);
            });

            // Capture frame instantly as an ImageBitmap (off-main-thread graphics)
            const bitmap = await createImageBitmap(video);
            
            // Send to worker for encoding
            workerRef.current.postMessage({
                type: 'generate_preview',
                bitmap,
                time: t,
                clientX
            }, [bitmap]); // Transfer bitmap
            
        } catch (e) {
            persistentLogger.warn('Failed to generate preview frame: ' + String(e), undefined, 'SegmentEditor');
            safeSetState(() => setIsGeneratingPreview(false));
        }
    }, [isGeneratingPreview, resolvedSrc, item?.src, item?.duration, safeSetState]);

    const handleTimelineMouseMove = useCallback((e: React.MouseEvent) => {
        if (!containerRef.current) return;
        if (!resolvedSrc || resolvedSrc.startsWith('signal:')) return;
        const rect = containerRef.current.getBoundingClientRect();
        const offsetX = e.clientX - rect.left + containerRef.current.scrollLeft;
        const time = Math.max(0, Math.min(item.duration || 0, offsetX / zoom));
        const now = Date.now();
        if (now - lastPreviewRequestRef.current < 90) return;
        lastPreviewRequestRef.current = now;
        generatePreviewFrame(time, e.clientX);
    }, [zoom, item.duration, resolvedSrc, generatePreviewFrame]);

    const handleTimelineMouseLeave = useCallback(() => {
        if (!dragRef.current) {
            setPreviewFrame(null);
        }
    }, []);

    const handleDeleteSelected = () => {
        if (!selectedSegmentId) return;
        setCollections(prev => prev.map(collection => {
            if (collection.id !== activeCollectionId) return collection;
            return {
                ...collection,
                segments: collection.segments.filter(s => s.id !== selectedSegmentId)
            };
        }));
        setSelectedSegmentId(null);
    };

    const handleCreateCollection = () => {
        const newCollection: SegmentCollection = {
            id: `segcol_${generateId()}`,
            name: newCollectionName || `Collection ${collections.length + 1}`,
            segments: [],
            isActive: false
        };
        setCollections(prev => prev.map(c => ({ ...c, isActive: false })).concat(newCollection));
        setActiveCollectionId(newCollection.id);
        setNewCollectionName('');
        setShowCollectionSelector(false);
    };

    const handleDuplicateCollection = (collectionId: string) => {
        const source = collections.find(c => c.id === collectionId);
        if (!source) return;
        const newCollection: SegmentCollection = {
            id: `segcol_${generateId()}`,
            name: `${source.name} (Copy)`,
            segments: source.segments.map(s => ({ ...s, id: `seg_${generateId()}` })),
            isActive: false
        };
        setCollections(prev => prev.map(c => ({ ...c, isActive: false })).concat(newCollection));
        setActiveCollectionId(newCollection.id);
    };

    const handleDeleteCollection = (collectionId: string) => {
        if (collections.length <= 1) {
            alert('Cannot delete the last collection');
            return;
        }
        if (confirm('Delete this collection?')) {
            const newCollections = collections.filter(c => c.id !== collectionId);
            if (activeCollectionId === collectionId) {
                setActiveCollectionId(newCollections[0].id);
            }
            setCollections(newCollections);
        }
    };

    // Keyboard shortcuts
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (editingCollectionName !== null) return;

            switch (e.key) {
                case ' ':
                    e.preventDefault();
                    handlePlayPause();
                    break;
                case 'ArrowLeft':
                    e.preventDefault();
                    const leftTime = Math.max(0, currentTime - (e.shiftKey ? 1 : 0.1));
                    setCurrentTime(leftTime);
                    if (videoRef.current) videoRef.current.currentTime = leftTime;
                    break;
                case 'ArrowRight':
                    e.preventDefault();
                    const rightTime = Math.min(item.duration || 0, currentTime + (e.shiftKey ? 1 : 0.1));
                    setCurrentTime(rightTime);
                    if (videoRef.current) videoRef.current.currentTime = rightTime;
                    break;
                case 'Delete':
                case 'Backspace':
                    if (selectedSegmentId) {
                        handleDeleteSelected();
                    }
                    break;
                case 'Escape':
                    onClose();
                    break;
                case 'Home':
                    setCurrentTime(0);
                    if (videoRef.current) videoRef.current.currentTime = 0;
                    break;
                case 'End':
                    setCurrentTime(item.duration || 0);
                    if (videoRef.current) videoRef.current.currentTime = item.duration || 0;
                    break;
                case 'a':
                case 'A':
                    if (e.ctrlKey || e.metaKey) {
                        e.preventDefault();
                        handleAddSegment();
                    }
                    break;
            }
        };

        window.addEventListener('keydown', handleKeyDown);
        return () => window.removeEventListener('keydown', handleKeyDown);
    }, [currentTime, item.duration, selectedSegmentId, editingCollectionName, activeCollectionId, segments, filename]);

    const handleSave = () => {
        const updatedCollections = collections.map(c => ({
            ...c,
            isActive: c.id === activeCollectionId
        }));
        onSave(updatedCollections, activeCollectionId);
    };

    // Export all segments as separate library items
    const handleExportSegmentsToLibrary = async () => {
        if (segments.length === 0) {
            alert('No segments to export. Add segments first.');
            return;
        }
        
        const confirm = window.confirm(
            `Export ${segments.length} segment(s) as separate library items?\n\n` +
            `Each segment will be saved as a new media item in your library with proper in/out points for scheduling.`
        );
        if (!confirm) return;
        
        const newItems: MediaItem[] = [];
        
        for (const segment of segments) {
            const newMediaItem: MediaItem = {
                id: `seg_${generateId()}_${Date.now()}`,
                title: segment.label || `${item.title} - Segment`,
                type: item.type,
                duration: segment.endTime - segment.startTime,
                src: item.src,
                filePath: item.filePath,
                filename: item.filename,
                thumbnail: segment.color || item.thumbnail,
                inPoint: segment.startTime,
                outPoint: segment.endTime,
                addedAt: new Date().toISOString(),
                dateMade: item.dateMade || new Date().toISOString(),
                priority: item.priority || 0,
                playCount: 0,
                programSeries: item.programSeries,
                notes: `Segment from "${item.title}" (${formatDuration(segment.startTime)} - ${formatDuration(segment.endTime)})`,
            };
            
            try {
                await dbSaveMedia(newMediaItem);
                newItems.push(newMediaItem);
            } catch (e) {
                logError('Failed to save segment to library', e);
            }
        }
        
        if (newItems.length > 0) {
            alert(`Successfully exported ${newItems.length} segment(s) to library!`);
            if (onExportSegmentsToLibrary) {
                onExportSegmentsToLibrary(newItems);
            }
        }
    };

    // Render logic – guard against missing item (e.g. parent unmount)
    if (!item) return null;

    const totalWidth = (item.duration || 0) * zoom;
    const frameWidth = totalWidth / Math.max(frames.length, 1);
    const isSignal = (item.src || '').toString().startsWith('signal:');
    const videoSrc = resolvedSrc || (isSignal ? undefined : (item.src || ''));

    return (
        <div className={styles.container}>
            <div className={styles.playerSection}>
                {isSignal ? (
                    <div style={{ color: 'white' }}>Signal Source (Preview Not Available)</div>
                ) : urlResolutionDone && !videoSrc ? (
                    <div className={styles.videoPlaceholder}>
                        No video source. Ensure the item has a file path or valid URL.
                    </div>
                ) : (
                    <video
                        ref={videoRef}
                        key={resolvedSrc || 'no-src'}
                        src={videoSrc || undefined}
                        className={styles.video}
                        onClick={handlePlayPause}
                        onError={(e) => {
                            const el = e.currentTarget;
                            persistentLogger.warn('SegmentEditor main video error: ' + (el?.error?.message || 'unknown'), undefined, 'SegmentEditor');
                        }}
                    />
                )}
            </div>

            <div className={styles.timelineSection}>
                <div className={styles.toolbar}>
                    <div className={styles.toolGroup}>
                        <button className={styles.btn} onClick={handlePlayPause}>
                            {isPlaying ? <Pause size={14} /> : <Play size={14} />}
                        </button>
                        <span style={{ fontSize: '0.8rem', width: '60px' }}>{formatDuration(currentTime)}</span>
                        <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)' }}>
                            Space: Play/Pause • ←→: Seek • A: Add Segment
                        </span>
                    </div>

                    <div className={styles.toolGroup}>
                        {/* Collection Selector */}
                        <div className={styles.collectionSelector}>
                            <button 
                                className={styles.btn}
                                onClick={() => setShowCollectionSelector(!showCollectionSelector)}
                            >
                                <Layers size={14} />
                                {activeCollection?.name || 'Collection'}
                                {showCollectionSelector ? <ChevronUp size={12} /> : <ChevronDown size={12} />}
                            </button>
                            {showCollectionSelector && (
                                <div className={styles.collectionDropdown}>
                                    {collections.map(collection => (
                                        <div key={collection.id} className={styles.collectionItem}>
                                            <button
                                                className={styles.collectionNameBtn}
                                                onClick={() => {
                                                    setActiveCollectionId(collection.id);
                                                    setShowCollectionSelector(false);
                                                }}
                                                onDoubleClick={() => setEditingCollectionName(collection.id)}
                                            >
                                                {collection.id === activeCollectionId && <Check size={12} />}
                                                <span>{collection.name}</span>
                                                <span className={styles.segmentCount}>({collection.segments.length})</span>
                                            </button>
                                            <div className={styles.collectionActions}>
                                                <button
                                                    className={styles.iconBtn}
                                                    onClick={() => handleDuplicateCollection(collection.id)}
                                                    title="Duplicate"
                                                >
                                                    <Copy size={12} />
                                                </button>
                                                {collections.length > 1 && (
                                                    <button
                                                        className={styles.iconBtn}
                                                        onClick={() => handleDeleteCollection(collection.id)}
                                                        title="Delete"
                                                    >
                                                        <Trash2 size={12} />
                                                    </button>
                                                )}
                                            </div>
                                        </div>
                                    ))}
                                    <div className={styles.collectionInput}>
                                        <input
                                            type="text"
                                            value={newCollectionName}
                                            onChange={(e) => setNewCollectionName(e.target.value)}
                                            placeholder="New collection name..."
                                            onKeyDown={(e) => {
                                                if (e.key === 'Enter' && newCollectionName.trim()) {
                                                    handleCreateCollection();
                                                }
                                            }}
                                        />
                                        <button
                                            className={styles.btn}
                                            onClick={handleCreateCollection}
                                            disabled={!newCollectionName.trim()}
                                        >
                                            <Plus size={12} />
                                        </button>
                                    </div>
                                </div>
                            )}
                        </div>

                        <button className={styles.btn} onClick={handleAddSegment}>
                            <Plus size={14} /> Add Segment
                        </button>
                        <div className={styles.autoGroup}>
                            <button className={styles.btn} onClick={handleAutoSegment}>
                                <Scissors size={14} /> Auto
                            </button>
                            <input
                                type="number"
                                min="2"
                                max="50"
                                value={autoSegmentCount}
                                onChange={(e) => setAutoSegmentCount(Math.max(2, Math.min(50, parseInt(e.target.value) || 10)))}
                                className={styles.countInput}
                                title="Number of segments"
                            />
                        </div>
                        <div className={styles.autoGroup}>
                            <input
                                type="number"
                                min="0"
                                max="10"
                                step="0.5"
                                value={autoSegmentOverlap}
                                onChange={(e) => setAutoSegmentOverlap(Math.max(0, Math.min(10, parseFloat(e.target.value) || 0)))}
                                className={styles.countInput}
                                title="Overlap in seconds"
                                placeholder="Overlap"
                            />
                        </div>
                        <button className={styles.btn} onClick={handleDeleteSelected} disabled={!selectedSegmentId} title="Delete selected segment">
                            <Trash2 size={14} /> Delete
                        </button>
                        <div className={styles.separator} />
                        <button 
                            className={`${styles.btn} ${styles.btnExport}`} 
                            onClick={handleExportSegmentsToLibrary} 
                            disabled={segments.length === 0}
                            title="Export all segments as separate library items for scheduling"
                        >
                            <FileOutput size={14} /> Export to Library
                        </button>
                    </div>

                    <div className={styles.toolGroup}>
                        <button
                            className={`${styles.btn} ${loadInRAM ? styles.btnActive : ''}`}
                            onClick={() => setLoadInRAM(!loadInRAM)}
                            title="Load video into RAM for fast seeking"
                        >
                            {loadInRAM ? <Database size={14} /> : <HardDrive size={14} />}
                            {loadInRAM ? 'RAM' : 'Disk'}
                        </button>
                        <div className={styles.zoomControls}>
                            <button className={styles.btn} onClick={() => handleZoom(-5)} style={{ border: 'none' }}><ZoomOut size={14} /></button>
                            <span style={{ fontSize: '0.75rem' }}>Zoom</span>
                            <button className={styles.btn} onClick={() => handleZoom(5)} style={{ border: 'none' }}><ZoomIn size={14} /></button>
                        </div>
                        <button className={`${styles.btn} ${styles.btnPrimary}`} onClick={handleSave}>
                            <Check size={14} /> Save
                        </button>
                        <button className={styles.btn} onClick={onClose}>
                            <X size={14} /> Close
                        </button>
                    </div>
                </div>

                {/* Waveform */}
                <div className={styles.waveformContainer}>
                    <canvas ref={waveformRef} className={styles.waveform} />
                </div>

                <div
                    className={styles.timelineContainer}
                    ref={containerRef}
                    onClick={handleTimelineClick}
                    onMouseDown={handleTimelineMouseDown}
                    onMouseUp={handleTimelineMouseUp}
                    onMouseLeave={(e) => { handleTimelineMouseLeave(); handleTimelineMouseUp(); }}
                    onMouseMove={handleTimelineMouseMove}
                    style={isHoldZooming ? { cursor: 'zoom-in', outline: '2px solid #f59e0b' } : {}}
                >
                    {isHoldZooming && (
                        <div className={styles.holdZoomBadge}>🔍 FINE TUNE — release to restore</div>
                    )}
                    <div className={styles.timelineTrack} style={{ width: totalWidth, minWidth: '100%' }} ref={timelineRef}>
                        {/* Frames Background - Show individual frames */}
                        <div className={styles.filmstrip} style={{ display: 'flex' }}>
                            {frames.map((frame, i) => (
                                <img 
                                    key={i} 
                                    src={frame} 
                                    style={{ 
                                        height: '100%', 
                                        width: `${frameWidth}px`, 
                                        objectFit: 'cover',
                                        borderRight: '1px solid rgba(255,255,255,0.1)'
                                    }} 
                                    alt={`Frame ${i + 1}`}
                                />
                            ))}
                            {frames.length === 0 && (
                                <div style={{ width: '100%', height: '100%', background: 'repeating-linear-gradient(45deg, #222, #222 10px, #333 10px, #333 20px)' }} />
                            )}
                        </div>

                        {/* Ruler with frame markers */}
                        <div className={styles.timeRuler}>
                            {Array.from({ length: Math.ceil((item.duration || 0) / 10) }).map((_, i) => (
                                <div key={i} style={{ position: 'absolute', left: i * 10 * zoom, borderLeft: '1px solid #555', paddingLeft: 2 }}>
                                    {formatDuration(i * 10)}
                                </div>
                            ))}
                        </div>

                        {/* Segments with joint handles */}
                        {(() => {
                            const sorted = [...segments].sort((a, b) => a.startTime - b.startTime);
                            return sorted.map((seg, idx) => {
                                const start = Number(seg.startTime) || 0;
                                const end = Number(seg.endTime) ?? start;
                                const width = Math.max(0, end - start) * zoom;
                                const nextSeg = sorted[idx + 1];
                                // Is the next segment adjacent (within 0.5s)?
                                const isAdjacentToNext = nextSeg && Math.abs(nextSeg.startTime - end) < 0.5;
                                return (
                                <React.Fragment key={seg.id}>
                                    <div
                                        className={`${styles.segment} ${selectedSegmentId === seg.id ? styles.selected : ''}`}
                                        style={{ left: start * zoom, width, backgroundColor: seg.color || '#3b82f6' }}
                                        onMouseDown={(e) => beginDrag(e, seg.id, 'move')}
                                        onClick={(e) => { e.stopPropagation(); setSelectedSegmentId(seg.id); }}
                                        title={`${seg.label}: ${formatDuration(start)} - ${formatDuration(end)}`}
                                    >
                                        <div className={`${styles.handle} ${styles.handleLeft}`} onMouseDown={(e) => beginDrag(e, seg.id, 'start')} />
                                        <div className={styles.segmentLabel}>{seg.label}</div>
                                        <div className={`${styles.handle} ${styles.handleRight}`} onMouseDown={(e) => beginDrag(e, seg.id, 'end')} />
                                    </div>
                                    {isAdjacentToNext && (
                                        <div
                                            className={styles.jointHandle}
                                            style={{ left: end * zoom - 6 }}
                                            onMouseDown={(e) => beginJointDrag(e, seg, nextSeg)}
                                            title="Drag to slide boundary between segments"
                                        />
                                    )}
                                </React.Fragment>
                                );
                            });
                        })()}

                        {/* Playhead */}
                        <div
                            className={styles.playhead}
                            style={{ left: currentTime * zoom }}
                        >
                            <div className={styles.playheadDragHandle} />
                        </div>
                    </div>
                </div>

                {/* Preview frame tooltip */}
                {previewFrame && (
                    <div 
                        className={styles.previewTooltip}
                        style={{ left: previewFrame.x }}
                    >
                        <img src={previewFrame.url} alt="Preview" className={styles.previewImage} />
                        <span className={styles.previewTime}>{formatDuration(previewFrame.time)}</span>
                    </div>
                )}

                {/* Hidden elements for preview frame generation */}
                <video
                    ref={previewVideoRef}
                    src={videoSrc || undefined}
                    style={{ display: 'none' }}
                    muted
                    preload="auto"
                    onError={(e) => console.error('[SegmentEditor] Preview video error:', e)}
                />
                <canvas ref={previewCanvasRef} style={{ display: 'none' }} />
            </div>
        </div>
    );
}
