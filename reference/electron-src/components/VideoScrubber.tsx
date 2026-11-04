import React, { useState, useRef, useEffect, useCallback } from 'react';
import styles from './VideoScrubber.module.css';
import { 
    X, Play, Pause, SkipBack, SkipForward, Volume2, VolumeX,
    Rewind, FastForward, Save, Scissors, AudioWaveform
} from 'lucide-react';
import { MediaItem, Segment } from '../lib/types';

interface VideoScrubberProps {
    mediaItem: MediaItem;
    onClose: () => void;
    onCreateSegment?: (segment: Segment) => void;
    onSaveMarks?: (markIn: number | null, markOut: number | null) => void;
}

export const VideoScrubber: React.FC<VideoScrubberProps> = ({
    mediaItem,
    onClose,
    onCreateSegment,
    onSaveMarks
}) => {
    const videoRef = useRef<HTMLVideoElement>(null);
    const progressRef = useRef<HTMLDivElement>(null);
    const waveformRef = useRef<HTMLCanvasElement>(null);
    
    const [isPlaying, setIsPlaying] = useState(false);
    const [currentTime, setCurrentTime] = useState(0);
    const [duration, setDuration] = useState(0);
    const [volume, setVolume] = useState(1);
    const [isMuted, setIsMuted] = useState(false);
    const [markIn, setMarkIn] = useState<number | null>(mediaItem.inPoint ?? null);
    const [markOut, setMarkOut] = useState<number | null>(mediaItem.outPoint ?? null);
    const [isDragging, setIsDragging] = useState(false);
    const [showThumbnail, setShowThumbnail] = useState(false);
    const [thumbnailPosition, setThumbnailPosition] = useState(0);
    const [thumbnailTime, setThumbnailTime] = useState(0);
    const [showSegmentDialog, setShowSegmentDialog] = useState(false);
    const [segmentName, setSegmentName] = useState('');
    const [waveformData, setWaveformData] = useState<number[]>([]);

    // Format time to HH:MM:SS:FF format
    const formatTimecode = useCallback((timeInSeconds: number): string => {
        const hours = Math.floor(timeInSeconds / 3600);
        const minutes = Math.floor((timeInSeconds % 3600) / 60);
        const seconds = Math.floor(timeInSeconds % 60);
        const frames = Math.floor((timeInSeconds % 1) * 30); // Assuming 30fps
        
        return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}:${frames.toString().padStart(2, '0')}`;
    }, []);

    // Generate fake waveform data (in real implementation, this would analyze audio)
    const generateWaveform = useCallback(() => {
        const samples = 200;
        const data: number[] = [];
        for (let i = 0; i < samples; i++) {
            // Generate pseudo-random waveform that looks realistic
            const base = Math.sin(i * 0.1) * 0.3;
            const noise = Math.random() * 0.4;
            const spike = Math.random() > 0.95 ? Math.random() * 0.3 : 0;
            data.push(Math.min(1, Math.abs(base + noise + spike)));
        }
        setWaveformData(data);
    }, []);

    // Draw waveform
    const drawWaveform = useCallback(() => {
        const canvas = waveformRef.current;
        if (!canvas || waveformData.length === 0) return;

        const ctx = canvas.getContext('2d');
        if (!ctx) return;

        const width = canvas.width;
        const height = canvas.height;
        const barWidth = width / waveformData.length;
        const progress = duration > 0 ? currentTime / duration : 0;
        const progressX = progress * width;

        ctx.clearRect(0, 0, width, height);

        waveformData.forEach((value, i) => {
            const x = i * barWidth;
            const barHeight = value * height * 0.8;
            const y = (height - barHeight) / 2;

            // Color based on progress
            if (x < progressX) {
                ctx.fillStyle = '#3b82f6';
            } else {
                ctx.fillStyle = '#3f3f46';
            }

            // Highlight marked region
            if (markIn !== null && markOut !== null) {
                const markInX = (markIn / duration) * width;
                const markOutX = (markOut / duration) * width;
                if (x >= markInX && x <= markOutX) {
                    ctx.fillStyle = x < progressX ? '#22c55e' : 'rgba(34, 197, 94, 0.4)';
                }
            }

            ctx.fillRect(x, y, Math.max(1, barWidth - 1), barHeight);
        });
    }, [waveformData, currentTime, duration, markIn, markOut]);

    useEffect(() => {
        generateWaveform();
    }, [generateWaveform]);

    useEffect(() => {
        drawWaveform();
    }, [drawWaveform]);

    useEffect(() => {
        const canvas = waveformRef.current;
        if (canvas) {
            const resizeObserver = new ResizeObserver(() => {
                canvas.width = canvas.offsetWidth;
                canvas.height = canvas.offsetHeight;
                drawWaveform();
            });
            resizeObserver.observe(canvas);
            return () => resizeObserver.disconnect();
        }
    }, [drawWaveform]);

    // Video event handlers
    const handleLoadedMetadata = () => {
        if (videoRef.current) {
            setDuration(videoRef.current.duration);
        }
    };

    const handleTimeUpdate = () => {
        if (videoRef.current && !isDragging) {
            setCurrentTime(videoRef.current.currentTime);
        }
    };

    const handlePlay = () => setIsPlaying(true);
    const handlePause = () => setIsPlaying(false);
    const handleEnded = () => setIsPlaying(false);

    const togglePlay = () => {
        if (videoRef.current) {
            if (isPlaying) {
                videoRef.current.pause();
            } else {
                videoRef.current.play();
            }
        }
    };

    const toggleMute = () => {
        if (videoRef.current) {
            videoRef.current.muted = !isMuted;
            setIsMuted(!isMuted);
        }
    };

    const handleVolumeChange = (e: React.ChangeEvent<HTMLInputElement>) => {
        const newVolume = parseFloat(e.target.value);
        if (videoRef.current) {
            videoRef.current.volume = newVolume;
            setVolume(newVolume);
            setIsMuted(newVolume === 0);
        }
    };

    const seek = (time: number) => {
        if (videoRef.current) {
            const clampedTime = Math.max(0, Math.min(time, duration));
            videoRef.current.currentTime = clampedTime;
            setCurrentTime(clampedTime);
        }
    };

    const skipFrames = (frames: number) => {
        seek(currentTime + frames / 30); // Assuming 30fps
    };

    const handleProgressClick = (e: React.MouseEvent) => {
        if (progressRef.current) {
            const rect = progressRef.current.getBoundingClientRect();
            const x = e.clientX - rect.left;
            const percentage = x / rect.width;
            seek(percentage * duration);
        }
    };

    const handleProgressMouseMove = (e: React.MouseEvent) => {
        if (progressRef.current) {
            const rect = progressRef.current.getBoundingClientRect();
            const x = e.clientX - rect.left;
            const percentage = Math.max(0, Math.min(1, x / rect.width));
            setThumbnailPosition(x);
            setThumbnailTime(percentage * duration);
            setShowThumbnail(true);

            if (isDragging) {
                seek(percentage * duration);
            }
        }
    };

    const handleProgressMouseLeave = () => {
        setShowThumbnail(false);
        if (!isDragging) {
            setIsDragging(false);
        }
    };

    const handleProgressMouseDown = () => {
        setIsDragging(true);
    };

    const handleProgressMouseUp = () => {
        setIsDragging(false);
    };

    // Mark in/out functions
    const setMarkInPoint = () => {
        setMarkIn(currentTime);
    };

    const setMarkOutPoint = () => {
        setMarkOut(currentTime);
    };

    const clearMarks = () => {
        setMarkIn(null);
        setMarkOut(null);
    };

    const goToMarkIn = () => {
        if (markIn !== null) seek(markIn);
    };

    const goToMarkOut = () => {
        if (markOut !== null) seek(markOut);
    };

    const playMarkedRegion = () => {
        if (markIn !== null && videoRef.current) {
            seek(markIn);
            videoRef.current.play().catch((err) => console.error('[VideoScrubber] Play failed:', err));
        }
    };

    const openSegmentDialog = () => {
        if (markIn !== null && markOut !== null && markOut > markIn) {
            // Segment name will be set from label
            setShowSegmentDialog(true);
        }
    };

    const createSegment = () => {
        if (markIn !== null && markOut !== null && segmentName && onCreateSegment) {
            onCreateSegment({
                id: crypto.randomUUID(),
                label: segmentName,
                startTime: markIn,
                endTime: markOut,
                color: '#3b82f6'
            });
            setShowSegmentDialog(false);
            setSegmentName('');
        }
    };

    const saveMarks = () => {
        if (onSaveMarks) {
            onSaveMarks(markIn, markOut);
        }
        onClose();
    };

    // Keyboard shortcuts
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (showSegmentDialog) return;

            switch (e.key) {
                case ' ':
                    e.preventDefault();
                    togglePlay();
                    break;
                case 'ArrowLeft':
                    e.preventDefault();
                    skipFrames(e.shiftKey ? -30 : -1);
                    break;
                case 'ArrowRight':
                    e.preventDefault();
                    skipFrames(e.shiftKey ? 30 : 1);
                    break;
                case 'i':
                case 'I':
                    setMarkInPoint();
                    break;
                case 'o':
                case 'O':
                    setMarkOutPoint();
                    break;
                case 'c':
                case 'C':
                    clearMarks();
                    break;
                case 'Escape':
                    onClose();
                    break;
                case 'Home':
                    seek(0);
                    break;
                case 'End':
                    seek(duration);
                    break;
            }
        };

        window.addEventListener('keydown', handleKeyDown);
        return () => window.removeEventListener('keydown', handleKeyDown);
    }, [currentTime, duration, showSegmentDialog]);

    // Watch for mark out to stop playback
    useEffect(() => {
        if (markOut !== null && isPlaying && currentTime >= markOut) {
            videoRef.current?.pause();
        }
    }, [currentTime, markOut, isPlaying]);

    const progress = duration > 0 ? (currentTime / duration) * 100 : 0;

    return (
        <div className={styles.scrubberOverlay} onClick={onClose}>
            <div className={styles.scrubber} onClick={e => e.stopPropagation()}>
                {/* Header */}
                <div className={styles.header}>
                    <div className={styles.title}>
                        <AudioWaveform size={20} />
                        <h3>{mediaItem.title}</h3>
                    </div>
                    <button className={styles.closeBtn} onClick={onClose}>
                        <X size={20} />
                    </button>
                </div>

                {/* Video Container */}
                <div className={styles.videoContainer}>
                    <video
                        ref={videoRef}
                        className={styles.video}
                        src={mediaItem.filePath || mediaItem.src}
                        onLoadedMetadata={handleLoadedMetadata}
                        onTimeUpdate={handleTimeUpdate}
                        onPlay={handlePlay}
                        onPause={handlePause}
                        onEnded={handleEnded}
                    />
                    {!isPlaying && (
                        <div className={styles.playOverlay} onClick={togglePlay}>
                            <Play size={64} />
                        </div>
                    )}
                </div>

                {/* Waveform */}
                <div className={styles.waveformContainer}>
                    <canvas ref={waveformRef} className={styles.waveform} />
                    <div 
                        className={styles.waveformPlayhead}
                        style={{ left: `${progress}%` }}
                    />
                </div>

                {/* Progress Bar */}
                <div
                    ref={progressRef}
                    className={styles.progressContainer}
                    onClick={handleProgressClick}
                    onMouseMove={handleProgressMouseMove}
                    onMouseLeave={handleProgressMouseLeave}
                    onMouseDown={handleProgressMouseDown}
                    onMouseUp={handleProgressMouseUp}
                >
                    {/* Marked Region */}
                    {markIn !== null && markOut !== null && (
                        <div
                            className={styles.markedRegion}
                            style={{
                                left: `${(markIn / duration) * 100}%`,
                                right: `${100 - (markOut / duration) * 100}%`
                            }}
                        />
                    )}
                    
                    {/* Mark In/Out Points */}
                    {markIn !== null && (
                        <div
                            className={styles.markIn}
                            style={{ left: `${(markIn / duration) * 100}%` }}
                        />
                    )}
                    {markOut !== null && (
                        <div
                            className={styles.markOut}
                            style={{ left: `${(markOut / duration) * 100}%` }}
                        />
                    )}

                    <div className={styles.progressBar}>
                        <div 
                            className={styles.progressFill}
                            style={{ width: `${progress}%` }}
                        />
                    </div>
                    <div 
                        className={styles.progressHandle}
                        style={{ left: `${progress}%` }}
                    />

                    {/* Thumbnail Preview */}
                    {showThumbnail && (
                        <div 
                            className={styles.thumbnailPreview}
                            style={{ left: thumbnailPosition }}
                        >
                            <span className={styles.thumbnailTime}>
                                {formatTimecode(thumbnailTime)}
                            </span>
                        </div>
                    )}
                </div>

                {/* Timecode Display */}
                <div className={styles.timecode}>
                    <span className={styles.currentTime}>{formatTimecode(currentTime)}</span>
                    <span className={styles.separator}>/</span>
                    <span className={styles.totalTime}>{formatTimecode(duration)}</span>
                    
                    {markIn !== null && (
                        <span className={styles.markDisplay}>IN: {formatTimecode(markIn)}</span>
                    )}
                    {markOut !== null && (
                        <span className={styles.markDisplay}>OUT: {formatTimecode(markOut)}</span>
                    )}
                </div>

                {/* Controls */}
                <div className={styles.controls}>
                    <div className={styles.playbackControls}>
                        <button onClick={() => skipFrames(-30)} title="Back 1 second">
                            <Rewind size={16} />
                        </button>
                        <button onClick={() => skipFrames(-1)} title="Previous frame">
                            <SkipBack size={16} />
                        </button>
                        <button className={styles.playBtn} onClick={togglePlay}>
                            {isPlaying ? <Pause size={24} /> : <Play size={24} />}
                        </button>
                        <button onClick={() => skipFrames(1)} title="Next frame">
                            <SkipForward size={16} />
                        </button>
                        <button onClick={() => skipFrames(30)} title="Forward 1 second">
                            <FastForward size={16} />
                        </button>
                    </div>

                    <div className={styles.volumeControl}>
                        <button onClick={toggleMute}>
                            {isMuted ? <VolumeX size={18} /> : <Volume2 size={18} />}
                        </button>
                        <input
                            type="range"
                            className={styles.volumeSlider}
                            min="0"
                            max="1"
                            step="0.05"
                            value={isMuted ? 0 : volume}
                            onChange={handleVolumeChange}
                        />
                    </div>

                    <div className={styles.markControls}>
                        <button 
                            onClick={setMarkInPoint} 
                            title="Set Mark In (I)"
                            className={markIn !== null ? styles.active : ''}
                        >
                            I
                        </button>
                        <button 
                            onClick={setMarkOutPoint} 
                            title="Set Mark Out (O)"
                            className={markOut !== null ? styles.active : ''}
                        >
                            O
                        </button>
                        <button onClick={goToMarkIn} title="Go to Mark In" disabled={markIn === null}>
                            ⏮
                        </button>
                        <button onClick={goToMarkOut} title="Go to Mark Out" disabled={markOut === null}>
                            ⏭
                        </button>
                        <button onClick={clearMarks} title="Clear Marks (C)">
                            ✕
                        </button>
                        <button 
                            className={styles.createSegmentBtn}
                            onClick={openSegmentDialog}
                            disabled={markIn === null || markOut === null || markOut <= markIn}
                            title="Create Segment from Marks"
                        >
                            <Scissors size={14} />
                            Segment
                        </button>
                        <button 
                            className={styles.createSegmentBtn}
                            onClick={saveMarks}
                            title="Save Marks"
                        >
                            <Save size={14} />
                            Save
                        </button>
                    </div>
                </div>

                {/* Keyboard Shortcuts */}
                <div className={styles.shortcuts}>
                    <span><kbd>Space</kbd> Play/Pause</span>
                    <span><kbd>←</kbd><kbd>→</kbd> Frame</span>
                    <span><kbd>Shift</kbd>+<kbd>←</kbd><kbd>→</kbd> 1 sec</span>
                    <span><kbd>I</kbd> Mark In</span>
                    <span><kbd>O</kbd> Mark Out</span>
                    <span><kbd>C</kbd> Clear</span>
                </div>

                {/* Segment Dialog */}
                {showSegmentDialog && (
                    <div className={styles.dialogOverlay} onClick={() => setShowSegmentDialog(false)}>
                        <div className={styles.dialog} onClick={e => e.stopPropagation()}>
                            <h4>Create Segment</h4>
                            <p>
                                {formatTimecode(markIn!)} → {formatTimecode(markOut!)} 
                                ({formatTimecode(markOut! - markIn!)})
                            </p>
                            <input
                                type="text"
                                value={segmentName}
                                onChange={e => setSegmentName(e.target.value)}
                                placeholder="Segment name"
                                autoFocus
                                onKeyDown={e => {
                                    if (e.key === 'Enter') createSegment();
                                    if (e.key === 'Escape') setShowSegmentDialog(false);
                                }}
                            />
                            <div className={styles.dialogActions}>
                                <button onClick={() => setShowSegmentDialog(false)}>Cancel</button>
                                <button 
                                    className={styles.primaryBtn}
                                    onClick={createSegment}
                                    disabled={!segmentName.trim()}
                                >
                                    <Scissors size={14} />
                                    Create
                                </button>
                            </div>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
};

export default VideoScrubber;

