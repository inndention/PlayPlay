import React, { useState, useRef, useEffect, useCallback } from 'react';
import { PlaylistItem, MediaItem, CGElement } from '../lib/types';
import { CGOverlay } from './CGOverlay';
import { formatDuration } from '../lib/utils';
import { 
    Play, Pause, Square, SkipForward, SkipBack, Volume2, VolumeX,
    Repeat, Shuffle, Monitor, Tv, Radio, Layers, Maximize2, Settings,
    CornerDownRight, ArrowRight, Upload, Trash2
} from 'lucide-react';
import styles from './VideoRouter.module.css';
import type { CGLayerState } from './CGOverlay';

// DDR Playlist type
export interface DDRPlaylist {
    id: string;
    items: PlaylistItem[];
    currentIndex: number;
    isPlaying: boolean;
    isLooping: boolean;
    volume: number;
}

// Video bus sources
type VideoSource = 'program' | 'ddr1' | 'ddr2' | 'black';

interface VideoRouterProps {
    // Main program feed
    programMedia?: PlaylistItem;
    programPlaying: boolean;
    onProgramEnded: () => void;
    
    // CG elements for overlay
    cgElements: CGElement[];
    cgLayers?: CGLayerState[];
    
    // Initial seek time for program (time-accurate sync)
    initialSeekTime?: number | null;
    
    // Callback to external output
    onOutputState?: (state: {
        source: VideoSource;
        media?: PlaylistItem;
        isPlaying: boolean;
        cgElements: CGElement[];
    }) => void;
}

export function VideoRouter({
    programMedia,
    programPlaying,
    onProgramEnded,
    cgElements,
    cgLayers,
    initialSeekTime,
    onOutputState,
}: VideoRouterProps) {
    // Video source selection
    const [onAirSource, setOnAirSource] = useState<VideoSource>('program');
    const [previewSource, setPreviewSource] = useState<VideoSource>('ddr1');
    
    // DDR playlists
    const [ddr1, setDdr1] = useState<DDRPlaylist>({
        id: 'ddr1',
        items: [],
        currentIndex: 0,
        isPlaying: false,
        isLooping: true,
        volume: 0.8,
    });
    const [ddr2, setDdr2] = useState<DDRPlaylist>({
        id: 'ddr2',
        items: [],
        currentIndex: 0,
        isPlaying: false,
        isLooping: true,
        volume: 0.8,
    });
    
    // Refs
    const programVideoRef = useRef<HTMLVideoElement>(null);
    const ddr1VideoRef = useRef<HTMLVideoElement>(null);
    const ddr2VideoRef = useRef<HTMLVideoElement>(null);
    const onAirVideoRef = useRef<HTMLVideoElement>(null);
    const previewVideoRef = useRef<HTMLVideoElement>(null);
    
    // DDR time tracking
    const [ddr1Time, setDdr1Time] = useState(0);
    const [ddr1Duration, setDdr1Duration] = useState(0);
    const [ddr2Time, setDdr2Time] = useState(0);
    const [ddr2Duration, setDdr2Duration] = useState(0);
    const [programTime, setProgramTime] = useState(0);
    const [programDuration, setProgramDuration] = useState(0);
    
    // Current DDR items
    const ddr1Current = ddr1.items[ddr1.currentIndex];
    const ddr2Current = ddr2.items[ddr2.currentIndex];

    // Program video handling
    useEffect(() => {
        const video = programVideoRef.current;
        if (!video) return;

        if (!programMedia) {
            video.pause();
            video.src = '';
            return;
        }

        const src = programMedia.media?.src;
        if (src && video.src !== src) {
            video.src = src;
            video.load();
        }

        if (programPlaying && video.paused && video.readyState >= 3) {
            video.play().catch(console.error);
        } else if (!programPlaying && !video.paused) {
            video.pause();
        }
    }, [programMedia, programPlaying]);

    // DDR1 video handling
    useEffect(() => {
        const video = ddr1VideoRef.current;
        if (!video || !ddr1Current) return;

        const src = ddr1Current.media?.src;
        if (src && video.src !== src) {
            video.src = src;
            video.load();
        }

        video.volume = ddr1.volume;
        if (ddr1.isPlaying && video.paused && video.readyState >= 3) {
            video.play().catch(console.error);
        } else if (!ddr1.isPlaying && !video.paused) {
            video.pause();
        }
    }, [ddr1Current, ddr1.isPlaying, ddr1.volume]);

    // DDR2 video handling
    useEffect(() => {
        const video = ddr2VideoRef.current;
        if (!video || !ddr2Current) return;

        const src = ddr2Current.media?.src;
        if (src && video.src !== src) {
            video.src = src;
            video.load();
        }

        video.volume = ddr2.volume;
        if (ddr2.isPlaying && video.paused && video.readyState >= 3) {
            video.play().catch(console.error);
        } else if (!ddr2.isPlaying && !video.paused) {
            video.pause();
        }
    }, [ddr2Current, ddr2.isPlaying, ddr2.volume]);

    // DDR1 ended handler
    const handleDdr1Ended = () => {
        if (ddr1.items.length === 0) return;
        
        if (ddr1.isLooping) {
            const nextIndex = (ddr1.currentIndex + 1) % ddr1.items.length;
            setDdr1(prev => ({ ...prev, currentIndex: nextIndex }));
        } else if (ddr1.currentIndex < ddr1.items.length - 1) {
            setDdr1(prev => ({ ...prev, currentIndex: prev.currentIndex + 1 }));
        } else {
            setDdr1(prev => ({ ...prev, isPlaying: false }));
        }
    };

    // DDR2 ended handler
    const handleDdr2Ended = () => {
        if (ddr2.items.length === 0) return;
        
        if (ddr2.isLooping) {
            const nextIndex = (ddr2.currentIndex + 1) % ddr2.items.length;
            setDdr2(prev => ({ ...prev, currentIndex: nextIndex }));
        } else if (ddr2.currentIndex < ddr2.items.length - 1) {
            setDdr2(prev => ({ ...prev, currentIndex: prev.currentIndex + 1 }));
        } else {
            setDdr2(prev => ({ ...prev, isPlaying: false }));
        }
    };

    // Take source to air
    const takeToAir = (source: VideoSource) => {
        setOnAirSource(source);
    };

    // Add media to DDR
    const addToDDR = (ddr: 'ddr1' | 'ddr2', item: PlaylistItem) => {
        if (ddr === 'ddr1') {
            setDdr1(prev => ({ ...prev, items: [...prev.items, item] }));
        } else {
            setDdr2(prev => ({ ...prev, items: [...prev.items, item] }));
        }
    };

    // Handle drop
    const handleDrop = (ddr: 'ddr1' | 'ddr2') => (e: React.DragEvent) => {
        e.preventDefault();
        try {
            const data = e.dataTransfer.getData('application/json');
            const media = JSON.parse(data) as MediaItem;
            const item: PlaylistItem = {
                id: `ddr_${Date.now()}`,
                mediaId: media.id,
                media,
            };
            addToDDR(ddr, item);
        } catch (err) {
            console.error('Drop failed:', err);
        }
    };

    // Get video element for source
    const getSourceVideo = (source: VideoSource): HTMLVideoElement | null => {
        switch (source) {
            case 'program': return programVideoRef.current;
            case 'ddr1': return ddr1VideoRef.current;
            case 'ddr2': return ddr2VideoRef.current;
            default: return null;
        }
    };

    // Notify output state
    useEffect(() => {
        if (onOutputState) {
            let media: PlaylistItem | undefined;
            let isPlaying = false;
            
            switch (onAirSource) {
                case 'program':
                    media = programMedia;
                    isPlaying = programPlaying;
                    break;
                case 'ddr1':
                    media = ddr1Current;
                    isPlaying = ddr1.isPlaying;
                    break;
                case 'ddr2':
                    media = ddr2Current;
                    isPlaying = ddr2.isPlaying;
                    break;
            }
            
            onOutputState({
                source: onAirSource,
                media,
                isPlaying,
                cgElements,
            });
        }
    }, [onAirSource, programMedia, programPlaying, ddr1Current, ddr1.isPlaying, ddr2Current, ddr2.isPlaying, cgElements]);

    return (
        <div className={styles.container}>
            {/* Hidden source videos */}
            <div className={styles.hiddenVideos}>
                <video
                    ref={programVideoRef}
                    onTimeUpdate={() => {
                        if (programVideoRef.current) {
                            setProgramTime(programVideoRef.current.currentTime);
                            setProgramDuration(programVideoRef.current.duration || 0);
                        }
                    }}
                    onEnded={onProgramEnded}
                    onError={(e) => console.error('[VideoRouter] Program video error:', e)}
                    muted
                    playsInline
                />
                <video
                    ref={ddr1VideoRef}
                    onTimeUpdate={() => {
                        if (ddr1VideoRef.current) {
                            setDdr1Time(ddr1VideoRef.current.currentTime);
                            setDdr1Duration(ddr1VideoRef.current.duration || 0);
                        }
                    }}
                    onEnded={handleDdr1Ended}
                    onError={(e) => console.error('[VideoRouter] DDR1 video error:', e)}
                    playsInline
                />
                <video
                    ref={ddr2VideoRef}
                    onTimeUpdate={() => {
                        if (ddr2VideoRef.current) {
                            setDdr2Time(ddr2VideoRef.current.currentTime);
                            setDdr2Duration(ddr2VideoRef.current.duration || 0);
                        }
                    }}
                    onEnded={handleDdr2Ended}
                    onError={(e) => console.error('[VideoRouter] DDR2 video error:', e)}
                    playsInline
                />
            </div>

            {/* Output Monitors */}
            <div className={styles.monitors}>
                {/* ON AIR Output */}
                <div className={styles.monitorSection}>
                    <div className={styles.monitorLabel}>
                        <Tv size={14} />
                        <span>ON AIR</span>
                        <span className={styles.tallyRed}>●</span>
                    </div>
                    <div className={styles.monitor}>
                        <OnAirMonitor
                            source={onAirSource}
                            programVideo={programVideoRef.current}
                            ddr1Video={ddr1VideoRef.current}
                            ddr2Video={ddr2VideoRef.current}
                            cgElements={cgElements}
                            cgLayers={cgLayers}
                        />
                    </div>
                    <div className={styles.monitorInfo}>
                        <span className={styles.sourceBadge} data-source={onAirSource}>
                            {onAirSource.toUpperCase()}
                        </span>
                        <span className={styles.timeCode}>
                            {onAirSource === 'program' && formatDuration(Math.floor(programTime))}
                            {onAirSource === 'ddr1' && formatDuration(Math.floor(ddr1Time))}
                            {onAirSource === 'ddr2' && formatDuration(Math.floor(ddr2Time))}
                        </span>
                    </div>
                </div>

                {/* DDR 1 */}
                <div className={styles.ddrSection}>
                    <div className={styles.monitorLabel}>
                        <Radio size={14} />
                        <span>DDR 1</span>
                        {ddr1.isPlaying && <span className={styles.tallyGreen}>●</span>}
                    </div>
                    <div 
                        className={styles.ddrMonitor}
                        onDragOver={(e) => e.preventDefault()}
                        onDrop={handleDrop('ddr1')}
                    >
                        <DDRMonitor
                            video={ddr1VideoRef.current}
                            currentItem={ddr1Current}
                            isEmpty={ddr1.items.length === 0}
                        />
                    </div>
                    <DDRControls
                        playlist={ddr1}
                        currentTime={ddr1Time}
                        duration={ddr1Duration}
                        onPlayPause={() => setDdr1(prev => ({ ...prev, isPlaying: !prev.isPlaying }))}
                        onStop={() => {
                            setDdr1(prev => ({ ...prev, isPlaying: false, currentIndex: 0 }));
                            if (ddr1VideoRef.current) ddr1VideoRef.current.currentTime = 0;
                        }}
                        onPrev={() => setDdr1(prev => ({ 
                            ...prev, 
                            currentIndex: Math.max(0, prev.currentIndex - 1) 
                        }))}
                        onNext={() => setDdr1(prev => ({ 
                            ...prev, 
                            currentIndex: Math.min(prev.items.length - 1, prev.currentIndex + 1) 
                        }))}
                        onLoop={() => setDdr1(prev => ({ ...prev, isLooping: !prev.isLooping }))}
                        onTakeToAir={() => takeToAir('ddr1')}
                        onClear={() => setDdr1(prev => ({ ...prev, items: [], currentIndex: 0, isPlaying: false }))}
                        isOnAir={onAirSource === 'ddr1'}
                    />
                    <div className={styles.ddrPlaylist}>
                        {ddr1.items.map((item, idx) => (
                            <div 
                                key={item.id} 
                                className={`${styles.playlistItem} ${idx === ddr1.currentIndex ? styles.active : ''}`}
                                onClick={() => setDdr1(prev => ({ ...prev, currentIndex: idx }))}
                            >
                                <span className={styles.itemNum}>{idx + 1}</span>
                                <span className={styles.itemTitle}>{item.media.title}</span>
                                <span className={styles.itemDur}>{formatDuration(item.media.duration)}</span>
                            </div>
                        ))}
                        {ddr1.items.length === 0 && (
                            <div className={styles.emptyDDR}>
                                <Upload size={16} />
                                <span>Drop media here</span>
                            </div>
                        )}
                    </div>
                </div>

                {/* DDR 2 */}
                <div className={styles.ddrSection}>
                    <div className={styles.monitorLabel}>
                        <Radio size={14} />
                        <span>DDR 2</span>
                        {ddr2.isPlaying && <span className={styles.tallyGreen}>●</span>}
                    </div>
                    <div 
                        className={styles.ddrMonitor}
                        onDragOver={(e) => e.preventDefault()}
                        onDrop={handleDrop('ddr2')}
                    >
                        <DDRMonitor
                            video={ddr2VideoRef.current}
                            currentItem={ddr2Current}
                            isEmpty={ddr2.items.length === 0}
                        />
                    </div>
                    <DDRControls
                        playlist={ddr2}
                        currentTime={ddr2Time}
                        duration={ddr2Duration}
                        onPlayPause={() => setDdr2(prev => ({ ...prev, isPlaying: !prev.isPlaying }))}
                        onStop={() => {
                            setDdr2(prev => ({ ...prev, isPlaying: false, currentIndex: 0 }));
                            if (ddr2VideoRef.current) ddr2VideoRef.current.currentTime = 0;
                        }}
                        onPrev={() => setDdr2(prev => ({ 
                            ...prev, 
                            currentIndex: Math.max(0, prev.currentIndex - 1) 
                        }))}
                        onNext={() => setDdr2(prev => ({ 
                            ...prev, 
                            currentIndex: Math.min(prev.items.length - 1, prev.currentIndex + 1) 
                        }))}
                        onLoop={() => setDdr2(prev => ({ ...prev, isLooping: !prev.isLooping }))}
                        onTakeToAir={() => takeToAir('ddr2')}
                        onClear={() => setDdr2(prev => ({ ...prev, items: [], currentIndex: 0, isPlaying: false }))}
                        isOnAir={onAirSource === 'ddr2'}
                    />
                    <div className={styles.ddrPlaylist}>
                        {ddr2.items.map((item, idx) => (
                            <div 
                                key={item.id} 
                                className={`${styles.playlistItem} ${idx === ddr2.currentIndex ? styles.active : ''}`}
                                onClick={() => setDdr2(prev => ({ ...prev, currentIndex: idx }))}
                            >
                                <span className={styles.itemNum}>{idx + 1}</span>
                                <span className={styles.itemTitle}>{item.media.title}</span>
                                <span className={styles.itemDur}>{formatDuration(item.media.duration)}</span>
                            </div>
                        ))}
                        {ddr2.items.length === 0 && (
                            <div className={styles.emptyDDR}>
                                <Upload size={16} />
                                <span>Drop media here</span>
                            </div>
                        )}
                    </div>
                </div>
            </div>

            {/* Source Selection Bus */}
            <div className={styles.bus}>
                <div className={styles.busLabel}>SOURCE SELECT</div>
                <button 
                    className={`${styles.busBtn} ${onAirSource === 'program' ? styles.onAir : ''}`}
                    onClick={() => takeToAir('program')}
                >
                    PGM
                </button>
                <button 
                    className={`${styles.busBtn} ${onAirSource === 'ddr1' ? styles.onAir : ''}`}
                    onClick={() => takeToAir('ddr1')}
                >
                    DDR1
                </button>
                <button 
                    className={`${styles.busBtn} ${onAirSource === 'ddr2' ? styles.onAir : ''}`}
                    onClick={() => takeToAir('ddr2')}
                >
                    DDR2
                </button>
                <button 
                    className={`${styles.busBtn} ${onAirSource === 'black' ? styles.onAir : ''}`}
                    onClick={() => takeToAir('black')}
                >
                    BLK
                </button>
            </div>
        </div>
    );
}

// On-Air Monitor - renders the selected source with CG
function OnAirMonitor({ 
    source, 
    programVideo, 
    ddr1Video, 
    ddr2Video,
    cgElements,
    cgLayers
}: {
    source: VideoSource;
    programVideo: HTMLVideoElement | null;
    ddr1Video: HTMLVideoElement | null;
    ddr2Video: HTMLVideoElement | null;
    cgElements: CGElement[];
    cgLayers?: CGLayerState[];
}) {
    const canvasRef = useRef<HTMLCanvasElement>(null);
    
    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas) return;
        
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        
        let animationId: number;
        
        const render = () => {
            ctx.fillStyle = '#000';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            
            let video: HTMLVideoElement | null = null;
            switch (source) {
                case 'program': video = programVideo; break;
                case 'ddr1': video = ddr1Video; break;
                case 'ddr2': video = ddr2Video; break;
            }
            
            if (video && video.readyState >= 2) {
                // Draw video maintaining aspect ratio
                const videoAspect = video.videoWidth / video.videoHeight || 16/9;
                const canvasAspect = canvas.width / canvas.height;
                
                let drawWidth = canvas.width;
                let drawHeight = canvas.height;
                let offsetX = 0;
                let offsetY = 0;
                
                if (videoAspect > canvasAspect) {
                    drawHeight = canvas.width / videoAspect;
                    offsetY = (canvas.height - drawHeight) / 2;
                } else {
                    drawWidth = canvas.height * videoAspect;
                    offsetX = (canvas.width - drawWidth) / 2;
                }
                
                ctx.drawImage(video, offsetX, offsetY, drawWidth, drawHeight);
            }
            
            animationId = requestAnimationFrame(render);
        };
        
        render();
        return () => cancelAnimationFrame(animationId);
    }, [source, programVideo, ddr1Video, ddr2Video]);
    
    return (
        <div className={styles.onAirMonitor}>
            <canvas ref={canvasRef} width={320} height={180} className={styles.canvas} />
            <CGOverlay elements={cgElements} cgLayers={cgLayers} />
        </div>
    );
}

// DDR Monitor component
function DDRMonitor({ 
    video, 
    currentItem,
    isEmpty 
}: { 
    video: HTMLVideoElement | null;
    currentItem?: PlaylistItem;
    isEmpty: boolean;
}) {
    const canvasRef = useRef<HTMLCanvasElement>(null);
    
    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas) return;
        
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        
        let animationId: number;
        
        const render = () => {
            ctx.fillStyle = '#111';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            
            if (video && video.readyState >= 2 && currentItem) {
                const videoAspect = video.videoWidth / video.videoHeight || 16/9;
                const canvasAspect = canvas.width / canvas.height;
                
                let drawWidth = canvas.width;
                let drawHeight = canvas.height;
                let offsetX = 0;
                let offsetY = 0;
                
                if (videoAspect > canvasAspect) {
                    drawHeight = canvas.width / videoAspect;
                    offsetY = (canvas.height - drawHeight) / 2;
                } else {
                    drawWidth = canvas.height * videoAspect;
                    offsetX = (canvas.width - drawWidth) / 2;
                }
                
                ctx.drawImage(video, offsetX, offsetY, drawWidth, drawHeight);
            } else if (isEmpty) {
                ctx.fillStyle = '#333';
                ctx.font = '12px sans-serif';
                ctx.textAlign = 'center';
                ctx.fillText('Empty', canvas.width / 2, canvas.height / 2);
            }
            
            animationId = requestAnimationFrame(render);
        };
        
        render();
        return () => cancelAnimationFrame(animationId);
    }, [video, currentItem, isEmpty]);
    
    return <canvas ref={canvasRef} width={192} height={108} className={styles.canvas} />;
}

// DDR Controls
function DDRControls({
    playlist,
    currentTime,
    duration,
    onPlayPause,
    onStop,
    onPrev,
    onNext,
    onLoop,
    onTakeToAir,
    onClear,
    isOnAir,
}: {
    playlist: DDRPlaylist;
    currentTime: number;
    duration: number;
    onPlayPause: () => void;
    onStop: () => void;
    onPrev: () => void;
    onNext: () => void;
    onLoop: () => void;
    onTakeToAir: () => void;
    onClear: () => void;
    isOnAir: boolean;
}) {
    const progress = duration > 0 ? (currentTime / duration) * 100 : 0;
    
    return (
        <div className={styles.ddrControls}>
            <div className={styles.progressBar}>
                <div className={styles.progressFill} style={{ width: `${progress}%` }} />
            </div>
            <div className={styles.controlRow}>
                <button className={styles.ctrlBtn} onClick={onPrev} disabled={playlist.items.length === 0}>
                    <SkipBack size={12} />
                </button>
                <button 
                    className={`${styles.ctrlBtn} ${styles.playBtn} ${playlist.isPlaying ? styles.playing : ''}`}
                    onClick={onPlayPause}
                    disabled={playlist.items.length === 0}
                >
                    {playlist.isPlaying ? <Pause size={14} /> : <Play size={14} />}
                </button>
                <button className={styles.ctrlBtn} onClick={onStop} disabled={playlist.items.length === 0}>
                    <Square size={12} />
                </button>
                <button className={styles.ctrlBtn} onClick={onNext} disabled={playlist.items.length === 0}>
                    <SkipForward size={12} />
                </button>
                <button 
                    className={`${styles.ctrlBtn} ${playlist.isLooping ? styles.active : ''}`}
                    onClick={onLoop}
                >
                    <Repeat size={12} />
                </button>
                <button 
                    className={`${styles.takeBtn} ${isOnAir ? styles.onAir : ''}`}
                    onClick={onTakeToAir}
                >
                    <ArrowRight size={12} /> TAKE
                </button>
                <button className={styles.clearBtn} onClick={onClear} title="Clear playlist">
                    <Trash2 size={12} />
                </button>
            </div>
            <div className={styles.timeDisplay}>
                {formatDuration(Math.floor(currentTime))} / {formatDuration(Math.floor(duration))}
            </div>
        </div>
    );
}

