/**
 * TalentMonitor - Professional Green Room / Talent Display
 * 
 * An informative remote screen for hosts and show guests packed with:
 * - Current camera indicator with visual arrows
 * - Real-time clock with precision timing
 * - Time to top of hour countdown
 * - Next scheduled file/segment
 * - Camera queuing with countdowns
 * - Current CG data display
 * - Time till next event
 * - Flashing on-air indicators
 * - Volume meters
 * - Video previews with live/preview selection
 * - Countdown till commercial break
 * - Producer-named segments for guests
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './TalentMonitor.module.css';
import {
    Camera, Clock, Play, Pause, Volume2, VolumeX, Tv, Radio,
    ChevronRight, ChevronLeft, ChevronUp, ChevronDown, AlertCircle,
    Users, Mic, MicOff, Monitor, Video, VideoOff, Eye, Settings,
    Zap, Timer, SkipForward, Layers, MessageSquare, Bell, RefreshCw,
    ArrowRight, ArrowLeft, ArrowUp, ArrowDown, Circle, Square,
    Maximize, Minimize, X, Check, Edit2, Save, User, CheckCircle
} from 'lucide-react';

interface CameraSource {
    id: string;
    name: string;
    label: string;
    type: 'camera' | 'input' | 'remote' | 'screen';
    isLive: boolean;
    isPreview: boolean;
    isQueued: boolean;
    queuePosition?: number;
    inputSource?: string;
    thumbnail?: string;
}

interface CGData {
    id: string;
    type: 'lower-third' | 'fullscreen' | 'ticker' | 'bug';
    title?: string;
    subtitle?: string;
    text?: string;
    isActive: boolean;
}

interface ScheduledSegment {
    id: string;
    name: string;
    type: 'media' | 'live' | 'break' | 'segment';
    duration: number;
    startTime: Date;
    guestName?: string;
    notes?: string;
}

interface LiveStreamStats {
    viewers: number;
    peakViewers: number;
    reactions: number;
    comments: number;
    shares: number;
    platform?: 'facebook' | 'youtube' | 'both';
}

interface Comment {
    id: string;
    author: string;
    message: string;
    timestamp: Date;
    platform: 'facebook' | 'youtube';
}

interface ProducerCountdown {
    seconds: number;
    message?: string;
    scheduled?: boolean;
    scheduledTime?: Date;
}

interface TalentMonitorProps {
    cameras?: CameraSource[];
    cgData?: CGData[];
    upcomingSegments?: ScheduledSegment[];
    currentSegment?: ScheduledSegment;
    onCameraSelect?: (cameraId: string) => void;
    onCameraQueue?: (cameraId: string) => void;
    isLive?: boolean;
    showName?: string;
    hostName?: string;
    producerNotes?: string;
    audioLevels?: { left: number; right: number };
    liveStats?: LiveStreamStats;
    comments?: Comment[];
    showComments?: boolean;
    producerCountdown?: ProducerCountdown | null;
    recordingSegments?: { id: string; startTime: Date; endTime?: Date; status: 'recording' | 'completed' }[];
}

export const TalentMonitor: React.FC<TalentMonitorProps> = ({
    cameras = [],
    cgData = [],
    upcomingSegments = [],
    currentSegment,
    onCameraSelect,
    onCameraQueue,
    isLive = false,
    showName = 'Live Show',
    hostName = 'Host',
    producerNotes = '',
    audioLevels = { left: 0, right: 0 },
    liveStats,
    comments = [],
    showComments = false,
    producerCountdown,
    recordingSegments = []
}) => {
    // State
    const [currentTime, setCurrentTime] = useState(new Date());
    const [timeToTopHour, setTimeToTopHour] = useState('');
    const [timeToNextEvent, setTimeToNextEvent] = useState('');
    const [timeToBreak, setTimeToBreak] = useState('');
    const [liveCamera, setLiveCamera] = useState<CameraSource | null>(null);
    const [previewCamera, setPreviewCamera] = useState<CameraSource | null>(null);
    const [cameraQueue, setCameraQueue] = useState<CameraSource[]>([]);
    const [showProducerPanel, setShowProducerPanel] = useState(false);
    const [guestNames, setGuestNames] = useState<Record<string, string>>({});
    const [editingGuest, setEditingGuest] = useState<string | null>(null);
    const [guestNameInput, setGuestNameInput] = useState('');
    const [flashOnAir, setFlashOnAir] = useState(false);
    const [audioMuted, setAudioMuted] = useState(false);
    const [fullscreenPreview, setFullscreenPreview] = useState<string | null>(null);
    const [countdownValue, setCountdownValue] = useState<number | null>(null);
    const [countdownMessage, setCountdownMessage] = useState<string>('');
    const countdownIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

    const activeCameras = cameras;
    const activeCG = cgData;
    const activeSegments = upcomingSegments;

    // Update time every 100ms for precision
    useEffect(() => {
        const timer = setInterval(() => {
            const now = new Date();
            setCurrentTime(now);
            
            // Calculate time to top of hour
            const nextHour = new Date(now);
            nextHour.setHours(nextHour.getHours() + 1, 0, 0, 0);
            const diffToHour = nextHour.getTime() - now.getTime();
            const minToHour = Math.floor(diffToHour / 60000);
            const secToHour = Math.floor((diffToHour % 60000) / 1000);
            setTimeToTopHour(`${minToHour}:${secToHour.toString().padStart(2, '0')}`);
            
            // Calculate time to next segment
            const nextSeg = activeSegments.find(s => new Date(s.startTime) > now);
            if (nextSeg) {
                const diffToSeg = new Date(nextSeg.startTime).getTime() - now.getTime();
                const minToSeg = Math.floor(diffToSeg / 60000);
                const secToSeg = Math.floor((diffToSeg % 60000) / 1000);
                setTimeToNextEvent(`${minToSeg}:${secToSeg.toString().padStart(2, '0')}`);
            }
            
            // Calculate time to next break
            const nextBreak = activeSegments.find(s => s.type === 'break' && new Date(s.startTime) > now);
            if (nextBreak) {
                const diffToBreak = new Date(nextBreak.startTime).getTime() - now.getTime();
                const minToBreak = Math.floor(diffToBreak / 60000);
                const secToBreak = Math.floor((diffToBreak % 60000) / 1000);
                setTimeToBreak(`${minToBreak}:${secToBreak.toString().padStart(2, '0')}`);
            }
        }, 100);
        
        return () => clearInterval(timer);
    }, [activeSegments]);

    // Flash on-air indicator
    useEffect(() => {
        if (isLive) {
            const flashTimer = setInterval(() => {
                setFlashOnAir(prev => !prev);
            }, 500);
            return () => clearInterval(flashTimer);
        } else {
            setFlashOnAir(false);
        }
    }, [isLive]);

    // Handle producer countdown
    useEffect(() => {
        if (producerCountdown) {
            if (producerCountdown.scheduled && producerCountdown.scheduledTime) {
                // Scheduled countdown
                const now = new Date();
                const scheduledTime = new Date(producerCountdown.scheduledTime);
                if (now >= scheduledTime) {
                    // Start countdown
                    setCountdownValue(producerCountdown.seconds);
                    setCountdownMessage(producerCountdown.message || '');
                }
            } else {
                // Instant countdown
                setCountdownValue(producerCountdown.seconds);
                setCountdownMessage(producerCountdown.message || '');
            }
        } else {
            setCountdownValue(null);
            setCountdownMessage('');
        }
    }, [producerCountdown]);

    // Countdown timer
    useEffect(() => {
        if (countdownValue !== null && countdownValue > 0) {
            if (countdownIntervalRef.current) {
                clearInterval(countdownIntervalRef.current);
            }
            countdownIntervalRef.current = setInterval(() => {
                setCountdownValue(prev => {
                    if (prev === null || prev <= 1) {
                        if (countdownIntervalRef.current) {
                            clearInterval(countdownIntervalRef.current);
                        }
                        return null;
                    }
                    return prev - 1;
                });
            }, 1000);
        } else {
            if (countdownIntervalRef.current) {
                clearInterval(countdownIntervalRef.current);
                countdownIntervalRef.current = null;
            }
        }

        return () => {
            if (countdownIntervalRef.current) {
                clearInterval(countdownIntervalRef.current);
            }
        };
    }, [countdownValue]);

    // Update live/preview cameras
    useEffect(() => {
        const live = activeCameras.find(c => c.isLive);
        const preview = activeCameras.find(c => c.isPreview);
        const queued = activeCameras.filter(c => c.isQueued).sort((a, b) => (a.queuePosition || 0) - (b.queuePosition || 0));
        
        setLiveCamera(live || null);
        setPreviewCamera(preview || null);
        setCameraQueue(queued);
    }, [activeCameras]);

    // Format time
    const formatTime = (date: Date) => {
        return date.toLocaleTimeString('en-US', { 
            hour: '2-digit', 
            minute: '2-digit', 
            second: '2-digit',
            hour12: false 
        });
    };

    const formatTimeWithMs = (date: Date) => {
        const ms = date.getMilliseconds().toString().padStart(3, '0').slice(0, 2);
        return `${formatTime(date)}.${ms}`;
    };

    // Handle camera selection
    const handleCameraClick = (camera: CameraSource) => {
        onCameraSelect?.(camera.id);
    };

    // Handle camera queue
    const handleQueueCamera = (camera: CameraSource) => {
        onCameraQueue?.(camera.id);
    };

    // Save guest name
    const saveGuestName = (segmentId: string) => {
        if (guestNameInput.trim()) {
            setGuestNames(prev => ({ ...prev, [segmentId]: guestNameInput.trim() }));
        }
        setEditingGuest(null);
        setGuestNameInput('');
    };

    // Get camera direction arrow
    const getCameraArrow = (camera: CameraSource) => {
        if (!camera.isLive) return null;
        
        // Determine camera position based on label
        const label = camera.label.toLowerCase();
        if (label.includes('left') || label.includes('guest')) return <ArrowLeft size={24} />;
        if (label.includes('right') || label.includes('host')) return <ArrowRight size={24} />;
        if (label.includes('wide') || label.includes('two')) return <ArrowUp size={24} />;
        return <Circle size={24} />;
    };

    // Render volume meter
    const renderVolumeMeter = (level: number, label: string) => {
        const segments = 20;
        const activeSegments = Math.round((level / 100) * segments);
        
        return (
            <div className={styles.volumeMeter}>
                <span className={styles.volumeLabel}>{label}</span>
                <div className={styles.volumeBar}>
                    {Array.from({ length: segments }).map((_, i) => (
                        <div 
                            key={i}
                            className={`${styles.volumeSegment} ${i < activeSegments ? styles.active : ''} ${i >= segments - 3 ? styles.peak : ''}`}
                        />
                    ))}
                </div>
                <span className={styles.volumeValue}>{level}dB</span>
            </div>
        );
    };

    // Current segment info
    const currentSeg = currentSegment || activeSegments[0];
    const nextSeg = activeSegments[1];

    return (
        <div className={`${styles.monitor} ${isLive ? styles.onAir : ''}`}>
            {/* Top Bar - On Air Status */}
            <div className={styles.topBar}>
                <div className={`${styles.onAirIndicator} ${flashOnAir ? styles.flash : ''}`}>
                    {isLive ? (
                        <>
                            <Circle className={styles.liveIcon} size={12} fill="currentColor" />
                            ON AIR
                        </>
                    ) : (
                        <>
                            <Square size={12} />
                            STANDBY
                        </>
                    )}
                </div>
                
                <div className={styles.showInfo}>
                    <span className={styles.showName}>{showName}</span>
                    <span className={styles.hostName}>Host: {hostName}</span>
                </div>
                
                <div className={styles.topActions}>
                    <button 
                        className={styles.iconBtn}
                        onClick={() => setShowProducerPanel(!showProducerPanel)}
                        title="Producer Panel"
                    >
                        <Settings size={18} />
                    </button>
                    <button 
                        className={`${styles.iconBtn} ${audioMuted ? styles.muted : ''}`}
                        onClick={() => setAudioMuted(!audioMuted)}
                        title={audioMuted ? 'Unmute' : 'Mute'}
                    >
                        {audioMuted ? <VolumeX size={18} /> : <Volume2 size={18} />}
                    </button>
                </div>
            </div>

            {/* Main Content */}
            <div className={styles.mainContent}>
                {/* Left Column - Clock and Timers */}
                <div className={styles.leftColumn}>
                    {/* Main Clock */}
                    <div className={styles.clockSection}>
                        <div className={styles.mainClock}>
                            <span className={styles.clockTime}>{formatTimeWithMs(currentTime)}</span>
                        </div>
                        <div className={styles.clockDate}>
                            {currentTime.toLocaleDateString('en-US', { 
                                weekday: 'long', 
                                month: 'long', 
                                day: 'numeric' 
                            })}
                        </div>
                    </div>

                    {/* Countdown Timers */}
                    <div className={styles.timers}>
                        <div className={styles.timerCard}>
                            <div className={styles.timerLabel}>
                                <Timer size={14} />
                                TO TOP OF HOUR
                            </div>
                            <div className={styles.timerValue}>{timeToTopHour}</div>
                        </div>
                        
                        <div className={styles.timerCard}>
                            <div className={styles.timerLabel}>
                                <SkipForward size={14} />
                                TO NEXT SEGMENT
                            </div>
                            <div className={styles.timerValue}>{timeToNextEvent || '--:--'}</div>
                        </div>
                        
                        <div className={`${styles.timerCard} ${styles.breakTimer}`}>
                            <div className={styles.timerLabel}>
                                <Pause size={14} />
                                TO BREAK
                            </div>
                            <div className={styles.timerValue}>{timeToBreak || '--:--'}</div>
                        </div>
                    </div>

                    {/* Volume Meters */}
                    <div className={styles.audioSection}>
                        <div className={styles.sectionHeader}>
                            <Volume2 size={14} />
                            AUDIO LEVELS
                        </div>
                        {renderVolumeMeter(audioLevels.left || 65, 'L')}
                        {renderVolumeMeter(audioLevels.right || 62, 'R')}
                    </div>

                    {/* Live Stream Stats */}
                    {liveStats && isLive && (
                        <div className={styles.liveStatsSection}>
                            <div className={styles.sectionHeader}>
                                <Radio size={14} />
                                LIVE STATS
                            </div>
                            <div className={styles.statsGrid}>
                                <div className={styles.statItem}>
                                    <span className={styles.statLabel}>Viewers</span>
                                    <span className={styles.statValue}>{liveStats.viewers.toLocaleString()}</span>
                                </div>
                                <div className={styles.statItem}>
                                    <span className={styles.statLabel}>Peak</span>
                                    <span className={styles.statValue}>{liveStats.peakViewers.toLocaleString()}</span>
                                </div>
                                <div className={styles.statItem}>
                                    <span className={styles.statLabel}>Reactions</span>
                                    <span className={styles.statValue}>{liveStats.reactions.toLocaleString()}</span>
                                </div>
                                <div className={styles.statItem}>
                                    <span className={styles.statLabel}>Comments</span>
                                    <span className={styles.statValue}>{liveStats.comments.toLocaleString()}</span>
                                </div>
                            </div>
                        </div>
                    )}

                    {/* Producer Countdown */}
                    {countdownValue !== null && (
                        <div className={styles.producerCountdown}>
                            <div className={styles.countdownHeader}>
                                <Zap size={16} />
                                PRODUCER COUNTDOWN
                            </div>
                            <div className={styles.countdownValue}>{countdownValue}</div>
                            {countdownMessage && (
                                <div className={styles.countdownMessage}>{countdownMessage}</div>
                            )}
                        </div>
                    )}

                    {/* Recording Segments */}
                    {recordingSegments.length > 0 && (
                        <div className={styles.recordingSection}>
                            <div className={styles.sectionHeader}>
                                <Video size={14} />
                                RECORDING
                            </div>
                            <div className={styles.segmentsList}>
                                {recordingSegments.map(segment => (
                                    <div key={segment.id} className={styles.segmentItem}>
                                        <div className={styles.segmentStatus}>
                                            {segment.status === 'recording' ? (
                                                <Circle size={8} fill="var(--danger)" />
                                            ) : (
                                                <CheckCircle size={8} fill="var(--success)" />
                                            )}
                                        </div>
                                        <div className={styles.segmentInfo}>
                                            <span className={styles.segmentTime}>
                                                {segment.startTime.toLocaleTimeString()}
                                            </span>
                                            {segment.endTime && (
                                                <span className={styles.segmentDuration}>
                                                    {Math.round((segment.endTime.getTime() - segment.startTime.getTime()) / 1000)}s
                                                </span>
                                            )}
                                        </div>
                                    </div>
                                ))}
                            </div>
                        </div>
                    )}
                </div>

                {/* Center - Camera Previews */}
                <div className={styles.centerColumn}>
                    {/* Live/Preview Monitors */}
                    <div className={styles.monitors}>
                        <div className={`${styles.monitorFrame} ${styles.liveMonitor}`}>
                            <div className={styles.monitorHeader}>
                                <span className={styles.liveTag}>
                                    <Circle size={8} fill="currentColor" />
                                    LIVE
                                </span>
                                <span>{liveCamera?.label || 'No Source'}</span>
                            </div>
                            <div className={styles.monitorScreen}>
                                {liveCamera?.thumbnail ? (
                                    <img src={liveCamera.thumbnail} alt="Live" />
                                ) : (
                                    <div className={styles.monitorPlaceholder}>
                                        <Camera size={48} />
                                        <span>{liveCamera?.name || 'No Live Source'}</span>
                                    </div>
                                )}
                                
                                {/* Camera direction arrow */}
                                {liveCamera && (
                                    <div className={styles.cameraArrow}>
                                        {getCameraArrow(liveCamera)}
                                    </div>
                                )}
                            </div>
                            <div className={styles.monitorInfo}>
                                <span>Source: {liveCamera?.name || 'N/A'}</span>
                            </div>
                        </div>

                        <div className={`${styles.monitorFrame} ${styles.previewMonitor}`}>
                            <div className={styles.monitorHeader}>
                                <span className={styles.previewTag}>
                                    <Eye size={10} />
                                    PREVIEW
                                </span>
                                <span>{previewCamera?.label || 'No Preview'}</span>
                            </div>
                            <div className={styles.monitorScreen}>
                                {previewCamera?.thumbnail ? (
                                    <img src={previewCamera.thumbnail} alt="Preview" />
                                ) : (
                                    <div className={styles.monitorPlaceholder}>
                                        <Eye size={48} />
                                        <span>{previewCamera?.name || 'No Preview'}</span>
                                    </div>
                                )}
                            </div>
                            <div className={styles.monitorInfo}>
                                <span>Next: {previewCamera?.name || 'N/A'}</span>
                            </div>
                        </div>
                    </div>

                    {/* Camera Queue */}
                    <div className={styles.cameraQueue}>
                        <div className={styles.sectionHeader}>
                            <Layers size={14} />
                            CAMERA QUEUE
                        </div>
                        <div className={styles.queueList}>
                            {cameraQueue.map((cam, index) => (
                                <div key={cam.id} className={styles.queueItem}>
                                    <span className={styles.queueNumber}>{index + 1}</span>
                                    <span className={styles.queueName}>{cam.label}</span>
                                    <span className={styles.queueSource}>{cam.name}</span>
                                </div>
                            ))}
                            {cameraQueue.length === 0 && (
                                <div className={styles.emptyQueue}>No cameras queued</div>
                            )}
                        </div>
                    </div>

                    {/* Camera Grid */}
                    <div className={styles.cameraGrid}>
                        {activeCameras.slice(0, 6).map(cam => (
                            <button 
                                key={cam.id}
                                className={`${styles.cameraBtn} ${cam.isLive ? styles.live : ''} ${cam.isPreview ? styles.preview : ''} ${cam.isQueued ? styles.queued : ''}`}
                                onClick={() => handleCameraClick(cam)}
                                onDoubleClick={() => handleQueueCamera(cam)}
                            >
                                <div className={styles.cameraThumbnail}>
                                    {cam.thumbnail ? (
                                        <img src={cam.thumbnail} alt={cam.name} />
                                    ) : (
                                        <Camera size={24} />
                                    )}
                                </div>
                                <div className={styles.cameraLabel}>
                                    <span>{cam.label}</span>
                                    {cam.isQueued && <span className={styles.queueBadge}>{cam.queuePosition}</span>}
                                </div>
                            </button>
                        ))}
                    </div>
                </div>

                {/* Right Column - Segment Info and CG */}
                <div className={styles.rightColumn}>
                    {/* Current Segment */}
                    <div className={styles.segmentSection}>
                        <div className={styles.sectionHeader}>
                            <Play size={14} />
                            NOW PLAYING
                        </div>
                        {currentSeg && (
                            <div className={styles.currentSegment}>
                                <div className={styles.segmentName}>{currentSeg.name}</div>
                                {currentSeg.guestName && (
                                    <div className={styles.segmentGuest}>
                                        <User size={12} />
                                        {guestNames[currentSeg.id] || currentSeg.guestName}
                                    </div>
                                )}
                                <div className={styles.segmentDuration}>
                                    Duration: {Math.floor(currentSeg.duration / 60)}:{Math.round(currentSeg.duration % 60).toString().padStart(2, '0')}
                                </div>
                            </div>
                        )}
                    </div>

                    {/* Up Next */}
                    <div className={styles.segmentSection}>
                        <div className={styles.sectionHeader}>
                            <SkipForward size={14} />
                            UP NEXT
                        </div>
                        <div className={styles.upNextList}>
                            {activeSegments.slice(1, 4).map((seg, index) => (
                                <div 
                                    key={seg.id} 
                                    className={`${styles.upNextItem} ${seg.type === 'break' ? styles.isBreak : ''}`}
                                >
                                    <span className={styles.upNextIndex}>{index + 1}</span>
                                    <div className={styles.upNextInfo}>
                                        <span className={styles.upNextName}>
                                            {seg.name}
                                            {seg.type === 'break' && <span className={styles.breakBadge}>BREAK</span>}
                                        </span>
                                        {seg.guestName && (
                                            <span className={styles.upNextGuest}>
                                                {guestNames[seg.id] || seg.guestName}
                                                {showProducerPanel && (
                                                    <button 
                                                        className={styles.editGuestBtn}
                                                        onClick={() => {
                                                            setEditingGuest(seg.id);
                                                            setGuestNameInput(guestNames[seg.id] || seg.guestName || '');
                                                        }}
                                                    >
                                                        <Edit2 size={10} />
                                                    </button>
                                                )}
                                            </span>
                                        )}
                                    </div>
                                    <span className={styles.upNextDuration}>
                                        {Math.floor(seg.duration / 60)}:{Math.round(seg.duration % 60).toString().padStart(2, '0')}
                                    </span>
                                </div>
                            ))}
                        </div>
                    </div>

                    {/* Current CG */}
                    <div className={styles.cgSection}>
                        <div className={styles.sectionHeader}>
                            <Layers size={14} />
                            ACTIVE GRAPHICS
                        </div>
                        <div className={styles.cgList}>
                            {activeCG.filter(cg => cg.isActive).map(cg => (
                                <div key={cg.id} className={`${styles.cgItem} ${styles[cg.type]}`}>
                                    <div className={styles.cgType}>{cg.type.toUpperCase()}</div>
                                    {cg.type === 'lower-third' && (
                                        <div className={styles.cgContent}>
                                            <div className={styles.cgTitle}>{cg.title}</div>
                                            {cg.subtitle && <div className={styles.cgSubtitle}>{cg.subtitle}</div>}
                                        </div>
                                    )}
                                    {cg.type === 'ticker' && (
                                        <div className={styles.cgContent}>
                                            <div className={styles.cgTicker}>{cg.text}</div>
                                        </div>
                                    )}
                                    {cg.type === 'bug' && (
                                        <div className={styles.cgBug}>{cg.text}</div>
                                    )}
                                </div>
                            ))}
                            {activeCG.filter(cg => cg.isActive).length === 0 && (
                                <div className={styles.noCG}>No active graphics</div>
                            )}
                        </div>
                    </div>

                    {/* Producer Notes */}
                    {(producerNotes || showProducerPanel) && (
                        <div className={styles.notesSection}>
                            <div className={styles.sectionHeader}>
                                <MessageSquare size={14} />
                                PRODUCER NOTES
                            </div>
                            <div className={styles.notesContent}>
                                {producerNotes || 'No notes for current segment.'}
                            </div>
                        </div>
                    )}
                </div>
            </div>

            {/* Bottom Status Bar */}
            <div className={styles.bottomBar}>
                <div className={styles.statusItem}>
                    <Camera size={14} />
                    <span>{activeCameras.length} Sources</span>
                </div>
                <div className={styles.statusItem}>
                    <Layers size={14} />
                    <span>{activeCG.filter(c => c.isActive).length} Graphics</span>
                </div>
                <div className={styles.statusItem}>
                    <Radio size={14} />
                    <span>Recording: {isLive ? 'Active' : 'Stopped'}</span>
                </div>
                <div className={styles.statusItem}>
                    <Users size={14} />
                    <span>Viewers: --</span>
                </div>
            </div>

            {/* Guest Name Edit Modal */}
            {editingGuest && (
                <div className={styles.modal} onClick={() => setEditingGuest(null)}>
                    <div className={styles.modalContent} onClick={e => e.stopPropagation()}>
                        <h3>Edit Guest Display Name</h3>
                        <input
                            type="text"
                            value={guestNameInput}
                            onChange={(e) => setGuestNameInput(e.target.value)}
                            placeholder="Enter guest name as shown..."
                            autoFocus
                        />
                        <div className={styles.modalActions}>
                            <button onClick={() => setEditingGuest(null)}>Cancel</button>
                            <button onClick={() => saveGuestName(editingGuest)}>
                                <Save size={14} /> Save
                            </button>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
};

export default TalentMonitor;
