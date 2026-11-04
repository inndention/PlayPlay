// ============================================
// ENHANCED BUILT-IN PLAYER ENGINE
// Hardware acceleration, upscaling, frame interpolation, deblocking
// ============================================

import React, { useEffect, useRef, useState, useCallback, useMemo } from 'react';
import { PlaylistItem } from '../lib/types';
import { generateSignal } from '../lib/signalGenerator';
import { resolveMediaUrlSync } from '../lib/fileResolver';
import { 
    videoEnhancementManager, 
    VideoEnhancementSettings, 
    HardwareCapabilities,
    getVideoEnhancementCSS,
    getUpscalingCSS,
} from '../lib/externalEngines';
import styles from './PlayerEngine.module.css';
import { clsx } from 'clsx';
import { Volume2, VolumeX, Settings, Zap, MonitorPlay, Headphones } from 'lucide-react';

interface PlayerEngineProps {
    currentMedia?: PlaylistItem;
    nextMedia?: PlaylistItem;
    isPlaying: boolean;
    onEnded: () => void;
    onTimeUpdate?: (time: number) => void;
    initialSeekTime?: number | null;
    onSeekComplete?: () => void;
    showOverlays?: boolean;
    defaultMuted?: boolean;
    enableEnhancements?: boolean; // Enable video enhancements
    onAudioLevelsChange?: (levels: { left: number; right: number; peak?: number }) => void;
    audioControlsSlot?: 'inline' | 'gutter';
}

// Video enhancement presets
const ENHANCEMENT_PRESETS: Record<string, Partial<VideoEnhancementSettings>> = {
    off: {
        upscaling: { enabled: false, algorithm: 'none', strength: 0 },
        frameInterpolation: { enabled: false, mode: 'none', targetFps: 60 },
        deblocking: { enabled: false, strength: 'light' },
        sharpening: { enabled: false, amount: 0 },
    },
    broadcast: {
        upscaling: { enabled: true, algorithm: 'lanczos', strength: 50 },
        frameInterpolation: { enabled: false, mode: 'none', targetFps: 60 },
        deblocking: { enabled: true, strength: 'light' },
        sharpening: { enabled: true, amount: 25 },
    },
    enhanced: {
        upscaling: { enabled: true, algorithm: 'sharp', strength: 75 },
        frameInterpolation: { enabled: true, mode: 'frame_blending', targetFps: 60 },
        deblocking: { enabled: true, strength: 'medium' },
        sharpening: { enabled: true, amount: 40 },
    },
    quality: {
        upscaling: { enabled: true, algorithm: 'bicubic', strength: 60 },
        frameInterpolation: { enabled: true, mode: 'motion_blur', targetFps: 60 },
        deblocking: { enabled: true, strength: 'heavy' },
        sharpening: { enabled: true, amount: 30 },
        colorCorrection: { enabled: true, brightness: 0, contrast: 5, saturation: 5, hue: 0 },
    },
};

export function PlayerEngine({
    currentMedia,
    nextMedia,
    isPlaying,
    onEnded,
    onTimeUpdate,
    initialSeekTime,
    onSeekComplete,
    showOverlays = true,
    defaultMuted = false,
    enableEnhancements = true,
    onAudioLevelsChange,
    audioControlsSlot = 'inline',
}: PlayerEngineProps) {
    const [progress, setProgress] = useState(0);
    const [currentTime, setCurrentTime] = useState(0);
    const [error, setError] = useState<string | null>(null);
    const [volume, setVolume] = useState(1);
    const [isMuted, setIsMuted] = useState(defaultMuted);
    const [audioLevel, setAudioLevel] = useState({ left: 0, right: 0, peak: 0 });
    const [meterNormalize, setMeterNormalize] = useState(false);
    const [showEnhancementPanel, setShowEnhancementPanel] = useState(false);
    const [enhancementSettings, setEnhancementSettings] = useState<VideoEnhancementSettings>(
        videoEnhancementManager.getSettings()
    );
    const [hwCapabilities, setHwCapabilities] = useState<HardwareCapabilities | null>(null);
    const [activePreset, setActivePreset] = useState<string>('broadcast');
    const [audioDevices, setAudioDevices] = useState<MediaDeviceInfo[]>([]);
    const [monitorDeviceId, setMonitorDeviceId] = useState('');
    const [lastMonitorDeviceId, setLastMonitorDeviceId] = useState('');
    
    const videoRef = useRef<HTMLVideoElement>(null);
    const monitorVideoRef = useRef<HTMLVideoElement>(null);
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const currentMediaIdRef = useRef<string | null>(null);
    const audioContextRef = useRef<AudioContext | null>(null);
    const analyserRef = useRef<AnalyserNode | null>(null);
    const leftAnalyserRef = useRef<AnalyserNode | null>(null);
    const rightAnalyserRef = useRef<AnalyserNode | null>(null);
    const splitterRef = useRef<ChannelSplitterNode | null>(null);
    const sourceNodeRef = useRef<MediaElementAudioSourceNode | null>(null);
    const gainNodeRef = useRef<GainNode | null>(null);
    const animationFrameRef = useRef<number | null>(null);
    const frameInterpolationRef = useRef<number | null>(null);

    // Value-mirror refs — let the media-load effect read live values without
    // re-running on every UI prop change. Without this, toggling volume would
    // call video.load() and interrupt playback.
    const isPlayingRef = useRef(isPlaying);
    const volumeRef = useRef(volume);
    const isMutedRef = useRef(isMuted);
    const monitorDeviceIdRef = useRef(monitorDeviceId);
    useEffect(() => { isPlayingRef.current = isPlaying; }, [isPlaying]);
    useEffect(() => { volumeRef.current = volume; }, [volume]);
    useEffect(() => { isMutedRef.current = isMuted; }, [isMuted]);
    useEffect(() => { monitorDeviceIdRef.current = monitorDeviceId; }, [monitorDeviceId]);

    // Load hardware capabilities
    useEffect(() => {
        const caps = videoEnhancementManager.getCapabilities();
        setHwCapabilities(caps);
        
        const unsubscribe = videoEnhancementManager.onSettingsChange(setEnhancementSettings);
        return unsubscribe;
    }, []);

    useEffect(() => {
        navigator.mediaDevices?.enumerateDevices?.()
            .then(devices => setAudioDevices(devices.filter(device => device.kind === 'audiooutput')))
            .catch(() => setAudioDevices([]));
    }, []);

    // Calculate video styles based on enhancement settings
    const videoStyles = useMemo(() => {
        if (!enableEnhancements) return {};
        
        const cssFilter = getVideoEnhancementCSS(enhancementSettings);
        const imageRendering = getUpscalingCSS(enhancementSettings.upscaling.algorithm);
        
        return {
            filter: cssFilter !== 'none' ? cssFilter : undefined,
            imageRendering: imageRendering !== 'auto' ? imageRendering as any : undefined,
            // Add motion blur effect for frame interpolation
            ...(enhancementSettings.frameInterpolation.enabled && 
                enhancementSettings.frameInterpolation.mode === 'motion_blur' && {
                '--motion-blur': '0.5px',
            }),
        };
    }, [enhancementSettings, enableEnhancements]);

    // Apply enhancement preset
    const applyPreset = useCallback((presetName: string) => {
        const preset = ENHANCEMENT_PRESETS[presetName];
        if (preset) {
            videoEnhancementManager.updateSettings(preset as VideoEnhancementSettings);
            setActivePreset(presetName);
        }
    }, []);

    // Hardware acceleration badge
    const hwAccelBadge = useMemo(() => {
        if (!hwCapabilities) return null;
        
        if (hwCapabilities.nvdec) return 'NVDEC';
        if (hwCapabilities.d3d11va) return 'D3D11VA';
        if (hwCapabilities.videotoolbox) return 'VT';
        if (hwCapabilities.vaapi) return 'VAAPI';
        if (hwCapabilities.webgpu) return 'WebGPU';
        if (hwCapabilities.webgl2) return 'WebGL2';
        return null;
    }, [hwCapabilities]);
    
    // Initialize audio context and analyzer
    const initAudioAnalyzer = useCallback(() => {
        const video = videoRef.current;
        if (!video) return;
        
        if (!audioContextRef.current) {
            audioContextRef.current = new AudioContext();
        }
        
        const audioContext = audioContextRef.current;
        
        if (!sourceNodeRef.current) {
            try {
                sourceNodeRef.current = audioContext.createMediaElementSource(video);
                analyserRef.current = audioContext.createAnalyser();
                analyserRef.current.fftSize = 256;
                analyserRef.current.smoothingTimeConstant = 0.3;
                leftAnalyserRef.current = audioContext.createAnalyser();
                rightAnalyserRef.current = audioContext.createAnalyser();
                leftAnalyserRef.current.fftSize = 256;
                rightAnalyserRef.current.fftSize = 256;
                leftAnalyserRef.current.smoothingTimeConstant = 0.3;
                rightAnalyserRef.current.smoothingTimeConstant = 0.3;
                splitterRef.current = audioContext.createChannelSplitter(2);
                
                gainNodeRef.current = audioContext.createGain();
                gainNodeRef.current.gain.value = isMuted ? 0 : volume;
                
                sourceNodeRef.current.connect(analyserRef.current);
                sourceNodeRef.current.connect(splitterRef.current);
                splitterRef.current.connect(leftAnalyserRef.current, 0);
                splitterRef.current.connect(rightAnalyserRef.current, 1);
                analyserRef.current.connect(gainNodeRef.current);
                gainNodeRef.current.connect(audioContext.destination);
                
                // Ensure video is not muted at the element level so Web Audio gets the signal
                video.muted = false;
            } catch (e) {
                console.log('Audio analyzer already connected');
            }
        }
        
        if (audioContext.state === 'suspended') {
            audioContext.resume();
        }
    }, []);
    
    // Update audio levels
    const updateAudioLevels = useCallback(() => {
        if (!analyserRef.current || !isPlaying) {
            setAudioLevel({ left: 0, right: 0, peak: 0 });
            onAudioLevelsChange?.({ left: 0, right: 0, peak: 0 });
            return;
        }
        const levelFromAnalyser = (analyser: AnalyserNode | null): number => {
            if (!analyser) return 0;
            const data = new Uint8Array(analyser.fftSize);
            analyser.getByteTimeDomainData(data);
            let sumSq = 0;
            for (let i = 0; i < data.length; i++) {
                const centered = (data[i] - 128) / 128;
                sumSq += centered * centered;
            }
            const rms = Math.sqrt(sumSq / data.length);
            return Math.min(1, rms * 3.2); // tuned for broadcast-style visual range
        };
        const leftLevelRaw = levelFromAnalyser(leftAnalyserRef.current);
        const rightLevelRaw = levelFromAnalyser(rightAnalyserRef.current);
        const monoFallback = levelFromAnalyser(analyserRef.current);
        const leftLevel = leftLevelRaw > 0.0001 ? leftLevelRaw : monoFallback;
        const rightLevel = rightLevelRaw > 0.0001 ? rightLevelRaw : monoFallback;
        const level = Math.max(leftLevel, rightLevel);
        
        setAudioLevel(prev => ({
            left: leftLevel,
            right: rightLevel,
            peak: Math.max(prev.peak * 0.95, level),
        }));
        onAudioLevelsChange?.({ left: leftLevel, right: rightLevel, peak: level });
        
        animationFrameRef.current = requestAnimationFrame(updateAudioLevels);
    }, [isPlaying, onAudioLevelsChange]);
    
    // Start/stop audio level monitoring
    useEffect(() => {
        if (isPlaying && showOverlays) {
            initAudioAnalyzer();
            animationFrameRef.current = requestAnimationFrame(updateAudioLevels);
        } else {
            if (animationFrameRef.current) {
                cancelAnimationFrame(animationFrameRef.current);
            }
        }
        
        return () => {
            if (animationFrameRef.current) {
                cancelAnimationFrame(animationFrameRef.current);
            }
        };
    }, [isPlaying, showOverlays, initAudioAnalyzer, updateAudioLevels]);

    // Keep trying to resume WebAudio while playing. This fixes "meter updates only after UI clicking"
    // when autoplay policy initially suspends the context and user interaction happens later.
    useEffect(() => {
        if (!isPlaying) return;
        const interval = window.setInterval(() => {
            const ctx = audioContextRef.current;
            if (ctx && ctx.state === 'suspended') {
                ctx.resume().catch(() => {
                    // ignore; next tick retries and succeeds after any user gesture
                });
            }
        }, 500);
        return () => window.clearInterval(interval);
    }, [isPlaying]);
    
    // Update video volume when volume/mute changes
    useEffect(() => {
        const video = videoRef.current;
        if (video) {
            if (gainNodeRef.current && audioContextRef.current) {
                // Control volume via Web Audio API so VU meter keeps getting signal
                gainNodeRef.current.gain.setTargetAtTime(
                    isMuted ? 0 : volume, 
                    audioContextRef.current.currentTime, 
                    0.05
                );
                // Ensure video is unmuted to feed the analyser
                video.muted = false;
            } else {
                // Fallback if audio context isn't running yet
                video.volume = volume;
                video.muted = isMuted;
            }
        }
        const monitor = monitorVideoRef.current;
        if (monitor) {
            monitor.volume = isMuted ? 0 : volume;
        }
    }, [volume, isMuted, currentMedia?.id]);

    // Load and play media when currentMedia changes
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;

        if (!currentMedia) {
            video.pause();
            video.src = '';
            video.srcObject = null;
            currentMediaIdRef.current = null;
            setError(null);
            return;
        }

        if (currentMedia.media.type === 'gap') {
            video.pause();
            if (isPlaying) {
                const gapTimer = setTimeout(() => {
                    onEnded();
                }, currentMedia.media.duration * 1000);
                return () => clearTimeout(gapTimer);
            }
            return;
        }

        // Start from any existing src on the media
        let src = currentMedia.media.src;
        
        if (currentMediaIdRef.current === currentMedia.id) {
            if (isPlaying) {
                video.play().catch(e => {
                    console.error('Play failed:', e);
                    setError('Playback failed');
                });
            } else {
                video.pause();
            }
            return;
        }

        console.log('Loading new media:', currentMedia.media.title, 'src:', src);
        setError(null);
        currentMediaIdRef.current = currentMedia.id;

        // Built-in test patterns / signal sources
        if (src && src.startsWith('signal:')) {
            const type = src.split(':')[1];
            console.log('Generating signal:', type);
            try {
                const stream = generateSignal(type);
                video.srcObject = stream;
                video.src = '';
                if (isPlayingRef.current) {
                    video.play().catch(e => console.error('Signal play failed:', e));
                }
            } catch (e) {
                console.error('Signal generation failed:', e);
                setError('Signal generation failed');
            }
            return;
        }

        // If we still don't have a usable src, try to resolve one via the universal resolver.
        // This protects against stale file paths and blocked targets (.db, media.db, asrun.db).
        if (!src || src === '') {
            const resolved = resolveMediaUrlSync(currentMedia.media);
            if (resolved) {
                src = resolved;
            }
        }

        if (!src || src === '') {
            console.error('No source for media:', currentMedia.media.title);
            console.log('Attempting fallback to test pattern...');
            try {
                const stream = generateSignal('bars');
                video.srcObject = stream;
                video.src = '';
                setError(`Missing: ${currentMedia.media.title}`);
                if (isPlayingRef.current) {
                    video.play().catch(e => console.error('Fallback play failed:', e));
                }
            } catch (e) {
                setError('No media source');
            }
            return;
        }

        video.srcObject = null;
        video.src = src;
        if (monitorVideoRef.current) {
            monitorVideoRef.current.srcObject = null;
            monitorVideoRef.current.src = src;
            monitorVideoRef.current.volume = isMutedRef.current ? 0 : volumeRef.current;
            monitorVideoRef.current.muted = !monitorDeviceIdRef.current;
        }

        const handleCanPlay = () => {
            const livePlaying = isPlayingRef.current;
            const liveVolume = volumeRef.current;
            const liveMuted = isMutedRef.current;
            const liveMonitor = monitorDeviceIdRef.current;

            console.log('Video can play, isPlaying:', livePlaying, 'initialSeekTime:', initialSeekTime);
            if (gainNodeRef.current && audioContextRef.current) {
                gainNodeRef.current.gain.setTargetAtTime(
                    liveMuted ? 0 : liveVolume,
                    audioContextRef.current.currentTime,
                    0.02
                );
            } else {
                video.volume = liveVolume;
                video.muted = liveMuted;
            }

            let seekPosition = 0;

            if (initialSeekTime !== null && initialSeekTime !== undefined && initialSeekTime > 0) {
                const timeIn = currentMedia.timeIn || 0;
                seekPosition = initialSeekTime + timeIn;
            } else if (currentMedia.timeIn && currentMedia.timeIn > 0) {
                seekPosition = currentMedia.timeIn;
            } else if (currentMedia.media.segments && currentMedia.media.segments.length > 0) {
                seekPosition = currentMedia.media.segments[0].startTime;
            }

            if (seekPosition > 0) {
                video.currentTime = seekPosition;
                const handleSeeked = () => {
                    video.removeEventListener('seeked', handleSeeked);
                    onSeekComplete?.();
                    if (isPlayingRef.current) {
                        video.play().catch(e => {
                            console.error('Auto-play after seek failed:', e);
                            setError('Auto-play blocked');
                        });
                        const monitor = monitorVideoRef.current;
                        if (monitor && monitorDeviceIdRef.current) {
                            monitor.currentTime = video.currentTime;
                            monitor.play().catch(() => {});
                        }
                    }
                };
                video.addEventListener('seeked', handleSeeked, { once: true });
            } else if (livePlaying) {
                video.play().catch(e => {
                    console.error('Auto-play failed:', e);
                    setError('Auto-play blocked');
                });
                if (monitorVideoRef.current && liveMonitor) {
                    monitorVideoRef.current.play().catch(() => {});
                }
            }
        };

        const handleError = (e: Event) => {
            const videoEl = e.target as HTMLVideoElement;
            const error = videoEl.error;
            console.error('Video error:', error?.code, error?.message, 'src:', src);
            setError(`Video error: ${error?.message || 'Unknown'}`);
        };

        video.addEventListener('canplay', handleCanPlay, { once: true });
        video.addEventListener('error', handleError, { once: true });
        video.load();

        return () => {
            video.removeEventListener('canplay', handleCanPlay);
            video.removeEventListener('error', handleError);
        };
        // CRITICAL: depend ONLY on media identity. Volume/play/mute changes must
        // not retrigger video.load() — they're applied by separate effects below.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [currentMedia?.id]);

    useEffect(() => {
        const monitor = monitorVideoRef.current;
        const video = videoRef.current;
        if (!monitor || !video) return;
        monitor.muted = !monitorDeviceId;
        const setSinkId = (monitor as HTMLMediaElement & { setSinkId?: (sinkId: string) => Promise<void> }).setSinkId;
        if (setSinkId && monitorDeviceId) {
            setSinkId.call(monitor, monitorDeviceId).catch(() => {});
        }
        if (monitorDeviceId && isPlaying) {
            monitor.currentTime = video.currentTime;
            monitor.play().catch(() => {});
        } else {
            monitor.pause();
        }
    }, [monitorDeviceId, isPlaying]);

    // Handle play/pause changes for current media
    useEffect(() => {
        const video = videoRef.current;
        if (!video || !currentMedia || currentMedia.media.type === 'gap') return;

        if (isPlaying && video.paused && video.readyState >= 3) {
            video.play().catch(e => console.error('Play failed:', e));
        } else if (!isPlaying && !video.paused) {
            video.pause();
        }
    }, [isPlaying]);

    // Apply volume/mute changes WITHOUT reloading media. The audio gain node has
    // a smooth ramp; touching it never interrupts the video element.
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;
        if (gainNodeRef.current && audioContextRef.current) {
            gainNodeRef.current.gain.setTargetAtTime(
                isMuted ? 0 : volume,
                audioContextRef.current.currentTime,
                0.02
            );
        } else {
            video.volume = volume;
            video.muted = isMuted;
        }
        const monitor = monitorVideoRef.current;
        if (monitor) {
            monitor.volume = isMuted ? 0 : volume;
        }
    }, [volume, isMuted]);

    // Time update handler
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;

        const handleTimeUpdate = () => {
            if (!currentMedia) return;
            
            let timeIn = currentMedia.timeIn;
            let timeOut = currentMedia.timeOut;
            
            if ((timeIn === undefined || timeOut === undefined) && currentMedia.media.segments && currentMedia.media.segments.length > 0) {
                const firstSegment = currentMedia.media.segments[0];
                timeIn = timeIn ?? firstSegment.startTime;
                timeOut = timeOut ?? firstSegment.endTime;
            }
            
            timeIn = timeIn || 0;
            timeOut = timeOut || currentMedia.media.duration;
            
            const effectiveDuration = Math.max(0.1, timeOut - timeIn);
            const currentPlayTime = Math.max(0, video.currentTime - timeIn);
            const pct = (currentPlayTime / effectiveDuration) * 100;
            
            setProgress(Math.max(0, Math.min(100, pct)));
            setCurrentTime(video.currentTime);
            
            onTimeUpdate?.(video.currentTime);

            if (timeOut && video.currentTime >= timeOut) {
                video.pause();
                onEnded();
            }
        };

        const handleEnded = () => {
            console.log('Video ended');
            onEnded();
        };

        video.addEventListener('timeupdate', handleTimeUpdate);
        video.addEventListener('ended', handleEnded);
        
        return () => {
            video.removeEventListener('timeupdate', handleTimeUpdate);
            video.removeEventListener('ended', handleEnded);
        };
    }, [currentMedia, onEnded, onTimeUpdate]);

    // Calculate remaining time
    const remaining = currentMedia ? (() => {
        let timeIn = currentMedia.timeIn;
        let timeOut = currentMedia.timeOut;
        
        if ((timeIn === undefined || timeOut === undefined) && currentMedia.media.segments && currentMedia.media.segments.length > 0) {
            const firstSegment = currentMedia.media.segments[0];
            timeIn = timeIn ?? firstSegment.startTime;
            timeOut = timeOut ?? firstSegment.endTime;
        }
        
        timeIn = timeIn || 0;
        timeOut = timeOut || currentMedia.media.duration;
        const effectiveDuration = timeOut - timeIn;
        const currentPlayTime = Math.max(0, currentTime - timeIn);
        return Math.max(0, effectiveDuration - currentPlayTime);
    })() : 0;

    const formatTime = (seconds: number) => {
        if (!isFinite(seconds) || isNaN(seconds)) return '00:00';
        const m = Math.floor(seconds / 60);
        const s = Math.floor(seconds % 60);
        return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    };

    const isGap = currentMedia?.media.type === 'gap';

    const handleVolumeChange = (e: React.ChangeEvent<HTMLInputElement>) => {
        const newVolume = parseFloat(e.target.value);
        setVolume(newVolume);
        if (newVolume > 0 && isMuted) {
            setIsMuted(false);
        }
    };
    
    const toggleMute = () => {
        setIsMuted(!isMuted);
    };
    
    const VolumeIcon = isMuted || volume === 0 ? VolumeX : Volume2;
    const audioInGutter = audioControlsSlot === 'gutter';
    const meterDisplay = useMemo(() => {
        const leftRaw = Math.max(0, audioLevel.left);
        const rightRaw = Math.max(0, audioLevel.right);
        const peakRaw = Math.max(0, audioLevel.peak);
        const fixedScale = 1.8;
        if (!meterNormalize) {
            return {
                left: Math.min(1, leftRaw * fixedScale),
                right: Math.min(1, rightRaw * fixedScale),
                peak: Math.min(1, peakRaw * fixedScale),
            };
        }
        const ref = Math.max(0.2, leftRaw, rightRaw, peakRaw);
        const s = 0.88 / ref;
        return {
            left: Math.min(1, leftRaw * s),
            right: Math.min(1, rightRaw * s),
            peak: Math.min(1, peakRaw * s),
        };
    }, [audioLevel.left, audioLevel.right, audioLevel.peak, meterNormalize]);

    const meterSegments = 16;
    const renderSegmentedMeter = (level: number, peak: number, channel: 'L' | 'R') => {
        const activeCount = Math.round(Math.max(0, Math.min(1, level)) * meterSegments);
        return (
            <div className={styles.meterChannelVertical}>
                <div className={styles.meterLabel}>{channel}</div>
                <div className={styles.meterBarVertical}>
                    <div className={styles.meterSegments}>
                        {Array.from({ length: meterSegments }).map((_, i) => {
                            const active = i < activeCount;
                            const zoneClass =
                                i >= Math.floor(meterSegments * 0.88)
                                    ? styles.segRed
                                    : i >= Math.floor(meterSegments * 0.72)
                                        ? styles.segYellow
                                        : styles.segGreen;
                            return (
                                <div
                                    key={`${channel}-${i}`}
                                    className={`${styles.meterSegment} ${active ? zoneClass : styles.segOff}`}
                                />
                            );
                        })}
                    </div>
                    <div className={styles.meterPeakVertical} style={{ bottom: `${Math.max(0, Math.min(1, peak)) * 100}%` }} />
                </div>
            </div>
        );
    };

    const audioControls = showOverlays ? (
        <div className={`${styles.audioControls} ${audioInGutter ? styles.audioControlsGutter : ''}`}>
            <div className={styles.audioMeterPro}>
                {renderSegmentedMeter(meterDisplay.left, meterDisplay.peak, 'L')}
                {renderSegmentedMeter(meterDisplay.right, meterDisplay.peak, 'R')}
                {!audioInGutter && (
                    <div className={styles.volumeBlock}>
                        <input
                            type="range"
                            min="0"
                            max="1"
                            step="0.01"
                            value={isMuted ? 0 : volume}
                            onChange={handleVolumeChange}
                            className={styles.volumeSliderVertical}
                            title={`Volume ${Math.round(volume * 100)}%`}
                        />
                    </div>
                )}
            </div>
            <div className={styles.audioBottomButtons}>
                <button
                    className={`${styles.muteBtn} ${isMuted ? styles.toggleOff : styles.toggleOn}`}
                    onClick={toggleMute}
                    title={isMuted ? 'Output muted' : 'Output enabled'}
                >
                    <VolumeIcon size={14} />
                </button>
                <button
                    className={`${styles.muteBtn} ${monitorDeviceId ? styles.toggleOn : styles.toggleOff}`}
                    onClick={() => {
                        if (monitorDeviceId) {
                            setLastMonitorDeviceId(monitorDeviceId);
                            setMonitorDeviceId('');
                            return;
                        }
                        const fallback = lastMonitorDeviceId || audioDevices[0]?.deviceId || '';
                        setMonitorDeviceId(fallback);
                    }}
                    title={monitorDeviceId ? 'Monitor enabled' : 'Monitor disabled'}
                    disabled={audioDevices.length === 0}
                >
                    <Headphones size={14} />
                </button>
                {!audioInGutter && (
                    <label className={styles.normalizeToggle}>
                        <input type="checkbox" checked={meterNormalize} onChange={(e) => setMeterNormalize(e.target.checked)} />
                        NORM
                    </label>
                )}
            </div>
            <div className={styles.monitorBlock}>
                {audioDevices.length > 0 ? (
                    <select
                        className={styles.monitorSelect}
                        value={monitorDeviceId}
                        onChange={(e) => {
                            const next = e.target.value;
                            setMonitorDeviceId(next);
                            if (next) setLastMonitorDeviceId(next);
                        }}
                        title="Separate monitor audio output"
                    >
                        <option value="">Monitor Off</option>
                        {audioDevices.map(device => (
                            <option key={device.deviceId} value={device.deviceId}>
                                {device.label || `Audio Output ${device.deviceId.slice(0, 6)}`}
                            </option>
                        ))}
                    </select>
                ) : (
                    <span className={styles.monitorUnavailable}>No outputs</span>
                )}
            </div>
        </div>
    ) : null;

    return (
        <div className={styles.container}>
            <video
                ref={videoRef}
                className={clsx(styles.video, !isGap && styles.active)}
                style={videoStyles}
                playsInline
                onError={(e) => console.error('[PlayerEngine] Video playback error:', e)}
            />
            <video
                ref={monitorVideoRef}
                className={styles.monitorAudioElement}
                muted={!monitorDeviceId}
                playsInline
                aria-hidden
            />

            {/* Gap screen */}
            {isGap && (
                <div className={styles.gapScreen}>
                    <div className={styles.gapPattern}></div>
                    {showOverlays && (
                        <>
                            <div className={styles.gapText}>GAP / FILL</div>
                            <div className={styles.gapDuration}>{formatTime(remaining)}</div>
                        </>
                    )}
                </div>
            )}

            {/* Standby screen */}
            {!currentMedia && showOverlays && (
                <div className={styles.idle}>
                    <div>STANDBY</div>
                    <div style={{ fontSize: '0.7rem', opacity: 0.6, marginTop: '0.5rem' }}>
                        Load schedule or add media
                    </div>
                </div>
            )}

            {/* Overlays */}
            {showOverlays && (
                <>
                    {error && (
                        <div className={styles.error}>
                            <div>⚠️ {error}</div>
                        </div>
                    )}

                    {/* Progress bar overlay */}
                    {currentMedia && !isGap && (
                        <div className={styles.progressOverlay}>
                            <div className={styles.progressBar}>
                                <div className={styles.progressFill} style={{ width: `${progress}%` }}></div>
                            </div>
                            <div className={styles.timeInfo}>
                                <span className={styles.elapsed}>{formatTime(currentTime)}</span>
                                <span className={styles.remaining}>-{formatTime(remaining)}</span>
                            </div>
                        </div>
                    )}

                    {/* Tally light */}
                    {isPlaying && <div className={styles.tallyLight}></div>}
                    
                    {/* Hardware Acceleration Badge */}
                    {hwAccelBadge && enableEnhancements && (
                        <div className={styles.hwAccelBadge}>
                            <Zap size={10} />
                            {hwAccelBadge}
                        </div>
                    )}

                    {/* Video Enhancement Toggle */}
                    {enableEnhancements && (
                        <button
                            className={styles.enhancementToggle}
                            onClick={() => setShowEnhancementPanel(!showEnhancementPanel)}
                            title="Video Enhancements"
                        >
                            <MonitorPlay size={14} />
                        </button>
                    )}

                    {/* Enhancement Panel */}
                    {showEnhancementPanel && enableEnhancements && (
                        <div className={styles.enhancementPanel}>
                            <div className={styles.enhancementHeader}>
                                <span>Video Enhancements</span>
                                <button onClick={() => setShowEnhancementPanel(false)}>×</button>
                            </div>
                            <div className={styles.presetButtons}>
                                {Object.keys(ENHANCEMENT_PRESETS).map((preset) => (
                                    <button
                                        key={preset}
                                        className={clsx(styles.presetBtn, activePreset === preset && styles.active)}
                                        onClick={() => applyPreset(preset)}
                                    >
                                        {preset.charAt(0).toUpperCase() + preset.slice(1)}
                                    </button>
                                ))}
                            </div>
                            <div className={styles.enhancementSettings}>
                                <label className={styles.settingRow}>
                                    <span>Upscaling</span>
                                    <select
                                        value={enhancementSettings.upscaling.algorithm}
                                        onChange={(e) => {
                                            videoEnhancementManager.updateSettings({
                                                upscaling: {
                                                    ...enhancementSettings.upscaling,
                                                    enabled: e.target.value !== 'none',
                                                    algorithm: e.target.value as any,
                                                },
                                            });
                                        }}
                                    >
                                        <option value="none">Off</option>
                                        <option value="bilinear">Bilinear</option>
                                        <option value="bicubic">Bicubic</option>
                                        <option value="lanczos">Lanczos</option>
                                        <option value="sharp">Sharp</option>
                                    </select>
                                </label>
                                <label className={styles.settingRow}>
                                    <span>Deblocking</span>
                                    <select
                                        value={enhancementSettings.deblocking.enabled ? enhancementSettings.deblocking.strength : 'off'}
                                        onChange={(e) => {
                                            videoEnhancementManager.updateSettings({
                                                deblocking: {
                                                    enabled: e.target.value !== 'off',
                                                    strength: e.target.value === 'off' ? 'light' : e.target.value as any,
                                                },
                                            });
                                        }}
                                    >
                                        <option value="off">Off</option>
                                        <option value="light">Light</option>
                                        <option value="medium">Medium</option>
                                        <option value="heavy">Heavy</option>
                                    </select>
                                </label>
                                <label className={styles.settingRow}>
                                    <span>Sharpening</span>
                                    <input
                                        type="range"
                                        min="0"
                                        max="100"
                                        value={enhancementSettings.sharpening.amount}
                                        onChange={(e) => {
                                            const amount = parseInt(e.target.value, 10);
                                            videoEnhancementManager.updateSettings({
                                                sharpening: {
                                                    enabled: amount > 0,
                                                    amount,
                                                },
                                            });
                                        }}
                                    />
                                    <span className={styles.settingValue}>
                                        {enhancementSettings.sharpening.amount}%
                                    </span>
                                </label>
                            </div>
                        </div>
                    )}
                    
                    {audioControlsSlot === 'inline' && audioControls}
                    
                    {/* Debug info */}
                    {currentMedia && (
                        <div style={{
                            position: 'absolute',
                            bottom: '40px',
                            left: '10px',
                            fontSize: '0.65rem',
                            color: 'rgba(255,255,255,0.5)',
                            background: 'rgba(0,0,0,0.5)',
                            padding: '4px 8px',
                            borderRadius: '4px',
                        }}>
                            {currentMedia.media.title} | {currentMedia.media.src?.substring(0, 30)}...
                        </div>
                    )}
                </>
            )}
            {audioControlsSlot === 'gutter' && audioControls}
        </div>
    );
}
