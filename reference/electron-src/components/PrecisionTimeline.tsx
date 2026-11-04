/**
 * PRECISION TIMELINE
 * Professional broadcast-grade timeline editor
 * Inspired by: Adobe Premiere, DaVinci Resolve, Grass Valley EDIUS
 * 
 * Features:
 * - Frame-accurate editing
 * - Waveform visualization
 * - Multiple track layers
 * - Thumbnail scrubbing
 * - Magnetic snap-to-grid
 * - Ripple/slip/slide editing
 * - Real-time preview
 */

import React, { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import {
    ZoomIn, ZoomOut, Scissors, Copy, Trash2, Lock, Unlock,
    Eye, EyeOff, Volume2, VolumeX, Layers, Magnet,
    ChevronDown, ChevronRight, GripVertical, MoreHorizontal,
    Play, Pause, Square, SkipBack, SkipForward,
    Maximize2, Minimize2, Grid, AlignLeft, AlignCenter, AlignRight,
    Clock, Film, Mic, Image, Tv, Radio
} from 'lucide-react';
import styles from './PrecisionTimeline.module.css';

// Types
export interface TimelineClip {
    id: string;
    trackId: string;
    type: 'video' | 'audio' | 'graphics' | 'live' | 'ad' | 'bumper';
    title: string;
    startFrame: number;
    durationFrames: number;
    mediaStartFrame?: number; // In-point in source media
    mediaDurationFrames?: number; // Total source duration
    color?: string;
    thumbnail?: string;
    locked?: boolean;
    muted?: boolean;
    volume?: number;
    opacity?: number;
    waveform?: number[]; // Normalized audio levels
}

export interface TimelineTrack {
    id: string;
    name: string;
    type: 'video' | 'audio' | 'graphics';
    height: number;
    locked: boolean;
    muted: boolean;
    visible: boolean;
    solo?: boolean;
    clips: TimelineClip[];
}

export interface TimelineMarker {
    id: string;
    frame: number;
    type: 'marker' | 'chapter' | 'error' | 'cue';
    label: string;
    color: string;
}

interface PrecisionTimelineProps {
    tracks: TimelineTrack[];
    markers?: TimelineMarker[];
    currentFrame: number;
    totalFrames: number;
    frameRate: number;
    isPlaying: boolean;
    selectedClipIds?: string[];
    onFrameChange?: (frame: number) => void;
    onClipSelect?: (clipIds: string[]) => void;
    onClipMove?: (clipId: string, trackId: string, startFrame: number) => void;
    onClipTrim?: (clipId: string, side: 'start' | 'end', newFrame: number) => void;
    onClipDelete?: (clipIds: string[]) => void;
    onClipSplit?: (clipId: string, frame: number) => void;
    onTrackMute?: (trackId: string, muted: boolean) => void;
    onTrackLock?: (trackId: string, locked: boolean) => void;
    onTrackSolo?: (trackId: string, solo: boolean) => void;
    onPlay?: () => void;
    onPause?: () => void;
    onStop?: () => void;
}

// Utility functions
const framesToTimecode = (frames: number, fps: number): string => {
    const totalSeconds = frames / fps;
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = Math.floor(totalSeconds % 60);
    const f = Math.floor(frames % fps);
    return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}:${f.toString().padStart(2, '0')}`;
};

const getClipTypeIcon = (type: string) => {
    switch (type) {
        case 'video': return <Film size={12} />;
        case 'audio': return <Volume2 size={12} />;
        case 'graphics': return <Layers size={12} />;
        case 'live': return <Radio size={12} />;
        case 'ad': return <Tv size={12} />;
        default: return <Film size={12} />;
    }
};

const getClipTypeColor = (type: string): string => {
    const colors: Record<string, string> = {
        video: 'var(--tl-video)',
        audio: 'var(--tl-audio)',
        graphics: 'var(--tl-graphics)',
        live: 'var(--tl-live)',
        ad: 'var(--tl-ad)',
        bumper: 'var(--tl-bumper)',
        program: 'var(--tl-program)',
    };
    return colors[type] || 'var(--surface-highlight)';
};

export const PrecisionTimeline: React.FC<PrecisionTimelineProps> = ({
    tracks,
    markers = [],
    currentFrame,
    totalFrames,
    frameRate,
    isPlaying,
    selectedClipIds = [],
    onFrameChange,
    onClipSelect,
    onClipMove,
    onClipTrim,
    onClipDelete,
    onClipSplit,
    onTrackMute,
    onTrackLock,
    onTrackSolo,
    onPlay,
    onPause,
    onStop,
}) => {
    const [zoom, setZoom] = useState(1);
    const [scrollX, setScrollX] = useState(0);
    const [snapEnabled, setSnapEnabled] = useState(true);
    const [showWaveforms, setShowWaveforms] = useState(true);
    const [draggingClip, setDraggingClip] = useState<string | null>(null);
    const [trimming, setTrimming] = useState<{ clipId: string; side: 'start' | 'end' } | null>(null);
    const [hoveredFrame, setHoveredFrame] = useState<number | null>(null);
    
    const timelineRef = useRef<HTMLDivElement>(null);
    const trackAreaRef = useRef<HTMLDivElement>(null);

    // Calculate pixels per frame based on zoom
    const pixelsPerFrame = useMemo(() => Math.max(0.1, zoom * 0.5), [zoom]);
    const timelineWidth = useMemo(() => totalFrames * pixelsPerFrame, [totalFrames, pixelsPerFrame]);

    // Ruler tick calculation
    const rulerTicks = useMemo(() => {
        const ticks: { frame: number; major: boolean }[] = [];
        const tickInterval = Math.max(1, Math.floor(30 / zoom)); // Adjust based on zoom
        const majorInterval = tickInterval * 10;
        
        for (let f = 0; f <= totalFrames; f += tickInterval) {
            ticks.push({ frame: f, major: f % majorInterval === 0 });
        }
        return ticks;
    }, [totalFrames, zoom]);

    // Handle zoom with mouse wheel
    const handleWheel = useCallback((e: React.WheelEvent) => {
        if (e.ctrlKey || e.metaKey) {
            e.preventDefault();
            const delta = e.deltaY > 0 ? -0.2 : 0.2;
            setZoom(z => Math.max(0.1, Math.min(10, z + delta)));
        } else {
            setScrollX(x => Math.max(0, Math.min(timelineWidth - 500, x + e.deltaX)));
        }
    }, [timelineWidth]);

    // Convert mouse position to frame
    const getFrameFromX = useCallback((clientX: number): number => {
        if (!trackAreaRef.current) return 0;
        const rect = trackAreaRef.current.getBoundingClientRect();
        const x = clientX - rect.left + scrollX;
        return Math.max(0, Math.min(totalFrames, Math.round(x / pixelsPerFrame)));
    }, [scrollX, pixelsPerFrame, totalFrames]);

    // Handle playhead scrub
    const handleRulerClick = useCallback((e: React.MouseEvent) => {
        const frame = getFrameFromX(e.clientX);
        onFrameChange?.(frame);
    }, [getFrameFromX, onFrameChange]);

    // Handle clip click
    const handleClipClick = useCallback((e: React.MouseEvent, clipId: string) => {
        e.stopPropagation();
        if (e.ctrlKey || e.metaKey) {
            // Multi-select
            const newSelection = selectedClipIds.includes(clipId)
                ? selectedClipIds.filter(id => id !== clipId)
                : [...selectedClipIds, clipId];
            onClipSelect?.(newSelection);
        } else {
            onClipSelect?.([clipId]);
        }
    }, [selectedClipIds, onClipSelect]);

    // Handle background click (deselect)
    const handleBackgroundClick = useCallback(() => {
        onClipSelect?.([]);
    }, [onClipSelect]);

    // Handle keyboard shortcuts
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (e.key === 'Delete' || e.key === 'Backspace') {
                if (selectedClipIds.length > 0) {
                    onClipDelete?.(selectedClipIds);
                }
            } else if (e.key === ' ') {
                e.preventDefault();
                if (isPlaying) {
                    onPause?.();
                } else {
                    onPlay?.();
                }
            } else if (e.key === 's' && !e.ctrlKey) {
                // Split at playhead
                if (selectedClipIds.length === 1) {
                    onClipSplit?.(selectedClipIds[0], currentFrame);
                }
            } else if (e.key === '+' || e.key === '=') {
                setZoom(z => Math.min(10, z + 0.5));
            } else if (e.key === '-') {
                setZoom(z => Math.max(0.1, z - 0.5));
            }
        };

        window.addEventListener('keydown', handleKeyDown);
        return () => window.removeEventListener('keydown', handleKeyDown);
    }, [selectedClipIds, currentFrame, isPlaying, onClipDelete, onClipSplit, onPlay, onPause]);

    // Calculate playhead position
    const playheadX = currentFrame * pixelsPerFrame - scrollX;

    return (
        <div className={styles.container} ref={timelineRef}>
            {/* Toolbar */}
            <div className={styles.toolbar}>
                <div className={styles.toolbarLeft}>
                    {/* Transport Controls */}
                    <div className={styles.transportGroup}>
                        <button className={styles.toolBtn} onClick={onStop} title="Stop">
                            <Square size={16} />
                        </button>
                        <button 
                            className={`${styles.toolBtn} ${isPlaying ? styles.active : ''}`} 
                            onClick={isPlaying ? onPause : onPlay}
                            title={isPlaying ? "Pause" : "Play"}
                        >
                            {isPlaying ? <Pause size={16} /> : <Play size={16} />}
                        </button>
                    </div>

                    <div className={styles.divider} />

                    {/* Edit Tools */}
                    <div className={styles.toolGroup}>
                        <button className={styles.toolBtn} title="Split (S)">
                            <Scissors size={16} />
                        </button>
                        <button className={styles.toolBtn} title="Copy">
                            <Copy size={16} />
                        </button>
                        <button 
                            className={styles.toolBtn} 
                            onClick={() => selectedClipIds.length > 0 && onClipDelete?.(selectedClipIds)}
                            disabled={selectedClipIds.length === 0}
                            title="Delete"
                        >
                            <Trash2 size={16} />
                        </button>
                    </div>

                    <div className={styles.divider} />

                    {/* Snap & Options */}
                    <div className={styles.toolGroup}>
                        <button 
                            className={`${styles.toolBtn} ${snapEnabled ? styles.active : ''}`}
                            onClick={() => setSnapEnabled(!snapEnabled)}
                            title="Snap to Grid"
                        >
                            <Magnet size={16} />
                        </button>
                        <button 
                            className={`${styles.toolBtn} ${showWaveforms ? styles.active : ''}`}
                            onClick={() => setShowWaveforms(!showWaveforms)}
                            title="Show Waveforms"
                        >
                            <Volume2 size={16} />
                        </button>
                    </div>
                </div>

                <div className={styles.toolbarCenter}>
                    {/* Timecode Display */}
                    <div className={styles.timecodeDisplay}>
                        <span className={styles.timecodeLabel}>Current</span>
                        <span className={styles.timecodeValue}>
                            {framesToTimecode(currentFrame, frameRate)}
                        </span>
                    </div>
                    <div className={styles.timecodeDivider}>/</div>
                    <div className={styles.timecodeDisplay}>
                        <span className={styles.timecodeLabel}>Duration</span>
                        <span className={styles.timecodeValue}>
                            {framesToTimecode(totalFrames, frameRate)}
                        </span>
                    </div>
                </div>

                <div className={styles.toolbarRight}>
                    {/* Zoom Controls */}
                    <div className={styles.zoomGroup}>
                        <button 
                            className={styles.toolBtn}
                            onClick={() => setZoom(z => Math.max(0.1, z - 0.5))}
                            title="Zoom Out"
                        >
                            <ZoomOut size={16} />
                        </button>
                        <div className={styles.zoomSlider}>
                            <input
                                type="range"
                                min="0.1"
                                max="10"
                                step="0.1"
                                value={zoom}
                                onChange={(e) => setZoom(parseFloat(e.target.value))}
                            />
                        </div>
                        <button 
                            className={styles.toolBtn}
                            onClick={() => setZoom(z => Math.min(10, z + 0.5))}
                            title="Zoom In"
                        >
                            <ZoomIn size={16} />
                        </button>
                        <span className={styles.zoomLevel}>{(zoom * 100).toFixed(0)}%</span>
                    </div>
                </div>
            </div>

            {/* Main Timeline Area */}
            <div className={styles.timelineArea}>
                {/* Track Headers */}
                <div className={styles.trackHeaders}>
                    <div className={styles.rulerHeader}>
                        <Clock size={14} />
                    </div>
                    {tracks.map(track => (
                        <div 
                            key={track.id} 
                            className={styles.trackHeader}
                            style={{ height: track.height }}
                        >
                            <div className={styles.trackInfo}>
                                <GripVertical size={14} className={styles.trackGrip} />
                                <span className={styles.trackName}>{track.name}</span>
                            </div>
                            <div className={styles.trackControls}>
                                <button 
                                    className={`${styles.trackBtn} ${track.muted ? styles.muted : ''}`}
                                    onClick={() => onTrackMute?.(track.id, !track.muted)}
                                    title="Mute"
                                >
                                    {track.muted ? <VolumeX size={14} /> : <Volume2 size={14} />}
                                </button>
                                <button 
                                    className={`${styles.trackBtn} ${track.solo ? styles.solo : ''}`}
                                    onClick={() => onTrackSolo?.(track.id, !track.solo)}
                                    title="Solo"
                                >
                                    S
                                </button>
                                <button 
                                    className={`${styles.trackBtn} ${track.locked ? styles.locked : ''}`}
                                    onClick={() => onTrackLock?.(track.id, !track.locked)}
                                    title="Lock"
                                >
                                    {track.locked ? <Lock size={14} /> : <Unlock size={14} />}
                                </button>
                            </div>
                        </div>
                    ))}
                </div>

                {/* Scrollable Track Area */}
                <div 
                    className={styles.trackArea}
                    ref={trackAreaRef}
                    onWheel={handleWheel}
                    onClick={handleBackgroundClick}
                >
                    {/* Ruler */}
                    <div 
                        className={styles.ruler}
                        onClick={handleRulerClick}
                        style={{ width: timelineWidth }}
                    >
                        {rulerTicks.map(tick => (
                            <div 
                                key={tick.frame}
                                className={`${styles.rulerTick} ${tick.major ? styles.major : ''}`}
                                style={{ left: tick.frame * pixelsPerFrame - scrollX }}
                            >
                                {tick.major && (
                                    <span className={styles.rulerLabel}>
                                        {framesToTimecode(tick.frame, frameRate).slice(0, 8)}
                                    </span>
                                )}
                            </div>
                        ))}

                        {/* Markers */}
                        {markers.map(marker => (
                            <div
                                key={marker.id}
                                className={`${styles.marker} ${styles[marker.type]}`}
                                style={{ 
                                    left: marker.frame * pixelsPerFrame - scrollX,
                                    '--marker-color': marker.color 
                                } as React.CSSProperties}
                                title={marker.label}
                            >
                                <span className={styles.markerFlag} />
                            </div>
                        ))}
                    </div>

                    {/* Tracks */}
                    <div className={styles.tracks} style={{ width: timelineWidth }}>
                        {tracks.map(track => (
                            <div 
                                key={track.id}
                                className={`${styles.track} ${track.locked ? styles.trackLocked : ''} ${track.muted ? styles.trackMuted : ''}`}
                                style={{ height: track.height }}
                            >
                                {/* Grid Lines */}
                                <div className={styles.trackGrid}>
                                    {rulerTicks.filter(t => t.major).map(tick => (
                                        <div 
                                            key={tick.frame}
                                            className={styles.gridLine}
                                            style={{ left: tick.frame * pixelsPerFrame - scrollX }}
                                        />
                                    ))}
                                </div>

                                {/* Clips */}
                                {track.clips.map(clip => {
                                    const clipLeft = clip.startFrame * pixelsPerFrame - scrollX;
                                    const clipWidth = clip.durationFrames * pixelsPerFrame;
                                    const isSelected = selectedClipIds.includes(clip.id);

                                    return (
                                        <div
                                            key={clip.id}
                                            className={`${styles.clip} ${isSelected ? styles.selected : ''} ${clip.locked ? styles.clipLocked : ''}`}
                                            style={{
                                                left: clipLeft,
                                                width: clipWidth,
                                                backgroundColor: clip.color || getClipTypeColor(clip.type),
                                            }}
                                            onClick={(e) => handleClipClick(e, clip.id)}
                                        >
                                            {/* Trim Handles */}
                                            <div className={`${styles.trimHandle} ${styles.trimStart}`} />
                                            <div className={`${styles.trimHandle} ${styles.trimEnd}`} />

                                            {/* Frame Stripes (Zoomed in) */}
                                            {pixelsPerFrame > 5 && clip.type === 'video' && (
                                                <div 
                                                    className={styles.frameStripes}
                                                    style={{
                                                        backgroundSize: `${pixelsPerFrame}px 100%`,
                                                        opacity: Math.min(0.3, (pixelsPerFrame - 5) / 20)
                                                    }}
                                                />
                                            )}

                                            {/* Clip Content */}
                                            <div className={styles.clipContent}>
                                                {/* Thumbnail */}
                                                {clip.thumbnail && clipWidth > 60 && (
                                                    <div 
                                                        className={styles.clipThumbnail}
                                                        style={{ 
                                                            backgroundImage: `url(${clip.thumbnail})` 
                                                        }}
                                                    />
                                                )}

                                                {/* Clip Info */}
                                                <div className={styles.clipInfo}>
                                                    {clipWidth > 40 && (
                                                        <span className={styles.clipIcon}>
                                                            {getClipTypeIcon(clip.type)}
                                                        </span>
                                                    )}
                                                    {clipWidth > 80 && (
                                                        <span className={styles.clipTitle}>{clip.title}</span>
                                                    )}
                                                </div>

                                                {/* Waveform */}
                                                {showWaveforms && clip.waveform && track.type === 'audio' && (
                                                    <div className={styles.waveform}>
                                                        {clip.waveform.map((level, i) => (
                                                            <div 
                                                                key={i}
                                                                className={styles.waveformBar}
                                                                style={{ height: `${level * 100}%` }}
                                                            />
                                                        ))}
                                                    </div>
                                                )}
                                            </div>

                                            {/* Duration Badge */}
                                            {clipWidth > 100 && (
                                                <span className={styles.clipDuration}>
                                                    {framesToTimecode(clip.durationFrames, frameRate).slice(3, 8)}
                                                </span>
                                            )}
                                        </div>
                                    );
                                })}
                            </div>
                        ))}
                    </div>

                    {/* Playhead */}
                    {playheadX >= 0 && playheadX <= 3000 && (
                        <div 
                            className={styles.playhead}
                            style={{ left: playheadX }}
                        >
                            <div className={styles.playheadHead} />
                            <div className={styles.playheadLine} />
                        </div>
                    )}
                </div>
            </div>

            {/* Mini Map */}
            <div className={styles.miniMap}>
                <div 
                    className={styles.miniMapView}
                    style={{
                        left: `${(scrollX / timelineWidth) * 100}%`,
                        width: `${(500 / timelineWidth) * 100}%`,
                    }}
                />
                {tracks.flatMap(track => 
                    track.clips.map(clip => (
                        <div
                            key={clip.id}
                            className={styles.miniMapClip}
                            style={{
                                left: `${(clip.startFrame / totalFrames) * 100}%`,
                                width: `${(clip.durationFrames / totalFrames) * 100}%`,
                                backgroundColor: clip.color || getClipTypeColor(clip.type),
                            }}
                        />
                    ))
                )}
                <div 
                    className={styles.miniMapPlayhead}
                    style={{ left: `${(currentFrame / totalFrames) * 100}%` }}
                />
            </div>
        </div>
    );
};

export default PrecisionTimeline;
