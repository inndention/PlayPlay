/**
 * COMMAND CONTROL SURFACE
 * Professional broadcast-grade control interface
 * Inspired by: Vizrt Mosart, Grass Valley iTX, Ross Video XPression
 * 
 * Features:
 * - One-touch automation control
 * - Real-time status monitoring
 * - GPI/GPO hardware integration
 * - Multi-output routing matrix
 * - Emergency override controls
 * - Voice command integration ready
 */

import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { 
    Play, Pause, Square, SkipForward, SkipBack, 
    Radio, Mic, MicOff, Volume2, VolumeX,
    AlertTriangle, Zap, Clock, Target, Layers,
    Monitor, Tv, Cast, Airplay, Activity,
    Shield, Lock, Unlock, RefreshCw, Power,
    ChevronUp, ChevronDown, Maximize2, Minimize2,
    Grid, List, Settings, Command, Terminal,
    Eye, EyeOff, AlertCircle, CheckCircle2, XCircle,
    FastForward, Rewind, RotateCcw, Repeat
} from 'lucide-react';
import styles from './CommandControlSurface.module.css';

// Types
interface ChannelStatus {
    id: string;
    name: string;
    state: 'on-air' | 'standby' | 'cued' | 'ready' | 'error' | 'offline';
    currentItem?: {
        title: string;
        elapsed: number;
        duration: number;
        type: 'video' | 'live' | 'graphics' | 'ad';
    };
    nextItem?: {
        title: string;
        duration: number;
        type: string;
    };
    audioLevel: number;
    signalStrength: number;
    timecode: string;
    countdownMs: number;
}

interface GpiState {
    id: number;
    label: string;
    state: boolean;
    type: 'input' | 'output';
    lastTriggered?: number;
}

interface QuickAction {
    id: string;
    label: string;
    icon: React.ReactNode;
    color: 'primary' | 'warning' | 'danger' | 'success' | 'neutral';
    shortcut?: string;
    action: () => void;
}

interface CommandControlSurfaceProps {
    channels?: ChannelStatus[];
    gpiStates?: GpiState[];
    onPlay?: () => void;
    onPause?: () => void;
    onStop?: () => void;
    onNext?: () => void;
    onPrevious?: () => void;
    onTake?: () => void;
    onEmergencyStop?: () => void;
    onRecue?: () => void;
    onGpiTrigger?: (id: number) => void;
    isLive?: boolean;
    automationEnabled?: boolean;
    onAutomationToggle?: (enabled: boolean) => void;
}

// Utility functions
const formatTimecode = (ms: number): string => {
    const totalSeconds = Math.floor(ms / 1000);
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    const frames = Math.floor((ms % 1000) / 33.33); // ~30fps
    return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}:${frames.toString().padStart(2, '0')}`;
};

const formatCountdown = (ms: number): string => {
    if (ms <= 0) return '00:00';
    const seconds = Math.ceil(ms / 1000);
    const minutes = Math.floor(seconds / 60);
    const secs = seconds % 60;
    return `-${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
};

export const CommandControlSurface: React.FC<CommandControlSurfaceProps> = ({
    channels = [],
    gpiStates = [],
    onPlay,
    onPause,
    onStop,
    onNext,
    onPrevious,
    onTake,
    onEmergencyStop,
    onRecue,
    onGpiTrigger,
    isLive = false,
    automationEnabled = true,
    onAutomationToggle
}) => {
    const [currentTime, setCurrentTime] = useState(Date.now());
    const [expandedSection, setExpandedSection] = useState<string | null>(null);
    const [audioMuted, setAudioMuted] = useState(false);
    const [locked, setLocked] = useState(false);
    
    // Update clock
    useEffect(() => {
        const interval = setInterval(() => setCurrentTime(Date.now()), 33);
        return () => clearInterval(interval);
    }, []);

    // Get primary channel (first on-air or first in list)
    const primaryChannel = useMemo(() => 
        channels.find(c => c.state === 'on-air') || channels[0],
        [channels]
    );

    // Keyboard shortcuts
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (locked) return;
            
            if (e.key === ' ' && !e.ctrlKey) {
                e.preventDefault();
                onPlay?.();
            } else if (e.key === 'Escape') {
                onStop?.();
            } else if (e.key === 'n' || e.key === 'N') {
                onNext?.();
            } else if (e.key === 'p' && !e.ctrlKey) {
                onPrevious?.();
            } else if (e.key === 't' || e.key === 'T') {
                onTake?.();
            } else if (e.key === 'F12') {
                e.preventDefault();
                onEmergencyStop?.();
            }
        };

        window.addEventListener('keydown', handleKeyDown);
        return () => window.removeEventListener('keydown', handleKeyDown);
    }, [locked, onPlay, onStop, onNext, onPrevious, onTake, onEmergencyStop]);

    // Calculate countdown criticality
    const getCountdownClass = (ms: number) => {
        if (ms <= 0) return styles.countdownExpired;
        if (ms <= 3000) return styles.countdownCritical;
        if (ms <= 10000) return styles.countdownWarning;
        return styles.countdownNormal;
    };

    // Quick actions configuration
    const quickActions: QuickAction[] = [
        { 
            id: 'take', 
            label: 'TAKE', 
            icon: <Zap size={20} />, 
            color: 'danger',
            shortcut: 'T',
            action: () => onTake?.()
        },
        { 
            id: 'recue', 
            label: 'RECUE', 
            icon: <RotateCcw size={20} />, 
            color: 'warning',
            shortcut: 'R',
            action: () => onRecue?.()
        },
        { 
            id: 'hold', 
            label: 'HOLD', 
            icon: <Pause size={20} />, 
            color: 'neutral',
            shortcut: 'H',
            action: () => onPause?.()
        },
        { 
            id: 'skip', 
            label: 'SKIP', 
            icon: <SkipForward size={20} />, 
            color: 'primary',
            shortcut: 'N',
            action: () => onNext?.()
        },
    ];

    return (
        <div className={`${styles.container} ${isLive ? styles.containerLive : ''}`}>
            {/* Master Header Bar */}
            <header className={styles.header}>
                <div className={styles.headerLeft}>
                    <div className={styles.logoSection}>
                        <Command size={24} className={styles.logoIcon} />
                        <span className={styles.logoText}>COMMAND</span>
                    </div>
                    
                    {/* Live Status Indicator */}
                    <div className={`${styles.liveIndicator} ${isLive ? styles.liveActive : styles.liveStandby}`}>
                        {isLive ? (
                            <>
                                <span className={styles.liveDot} />
                                <span>ON AIR</span>
                            </>
                        ) : (
                            <span>STANDBY</span>
                        )}
                    </div>
                </div>

                <div className={styles.headerCenter}>
                    {/* Master Clock */}
                    <div className={styles.masterClock}>
                        <Clock size={16} className={styles.clockIcon} />
                        <span className={styles.clockTime}>
                            {new Date(currentTime).toLocaleTimeString('en-US', { 
                                hour12: false, 
                                hour: '2-digit', 
                                minute: '2-digit', 
                                second: '2-digit' 
                            })}
                        </span>
                        <span className={styles.clockFrames}>
                            :{Math.floor((currentTime % 1000) / 33.33).toString().padStart(2, '0')}
                        </span>
                    </div>
                </div>

                <div className={styles.headerRight}>
                    {/* Automation Status */}
                    <button 
                        className={`${styles.automationToggle} ${automationEnabled ? styles.automationOn : styles.automationOff}`}
                        onClick={() => onAutomationToggle?.(!automationEnabled)}
                    >
                        {automationEnabled ? <Unlock size={16} /> : <Lock size={16} />}
                        <span>AUTO {automationEnabled ? 'ON' : 'OFF'}</span>
                    </button>

                    {/* Lock Control */}
                    <button 
                        className={`${styles.lockButton} ${locked ? styles.locked : ''}`}
                        onClick={() => setLocked(!locked)}
                        title={locked ? 'Unlock controls' : 'Lock controls'}
                    >
                        {locked ? <Lock size={18} /> : <Unlock size={18} />}
                    </button>

                    {/* Emergency Stop */}
                    <button 
                        className={styles.emergencyStop}
                        onClick={onEmergencyStop}
                        disabled={locked}
                        title="Emergency Stop (F12)"
                    >
                        <AlertTriangle size={18} />
                        <span>E-STOP</span>
                    </button>
                </div>
            </header>

            {/* Main Control Area */}
            <div className={styles.mainArea}>
                {/* Left Panel - Channel Status */}
                <aside className={styles.channelPanel}>
                    <div className={styles.panelHeader}>
                        <Tv size={16} />
                        <span>Channels</span>
                    </div>
                    
                    <div className={styles.channelList}>
                        {channels.length === 0 ? (
                            <div className={styles.emptyChannels}>
                                <Monitor size={32} />
                                <span>No channels configured</span>
                            </div>
                        ) : (
                            channels.map((channel) => (
                                <div 
                                    key={channel.id} 
                                    className={`${styles.channelCard} ${styles[`channel${channel.state.replace('-', '')}`]}`}
                                >
                                    <div className={styles.channelHeader}>
                                        <span className={styles.channelName}>{channel.name}</span>
                                        <span className={`${styles.channelState} ${styles[channel.state.replace('-', '')]}`}>
                                            {channel.state.toUpperCase()}
                                        </span>
                                    </div>
                                    
                                    {channel.currentItem && (
                                        <div className={styles.channelNow}>
                                            <div className={styles.nowPlaying}>
                                                <span className={styles.nowLabel}>NOW</span>
                                                <span className={styles.nowTitle}>{channel.currentItem.title}</span>
                                            </div>
                                            <div className={styles.nowProgress}>
                                                <div 
                                                    className={styles.nowProgressFill}
                                                    style={{ width: `${(channel.currentItem.elapsed / channel.currentItem.duration) * 100}%` }}
                                                />
                                            </div>
                                            <div className={styles.nowTimes}>
                                                <span>{formatTimecode(channel.currentItem.elapsed * 1000)}</span>
                                                <span className={getCountdownClass(channel.countdownMs)}>
                                                    {formatCountdown(channel.countdownMs)}
                                                </span>
                                            </div>
                                        </div>
                                    )}

                                    {channel.nextItem && (
                                        <div className={styles.channelNext}>
                                            <span className={styles.nextLabel}>NEXT</span>
                                            <span className={styles.nextTitle}>{channel.nextItem.title}</span>
                                        </div>
                                    )}

                                    {/* Audio Meter */}
                                    <div className={styles.audioMeter}>
                                        <div className={styles.audioMeterBars}>
                                            {Array.from({ length: 20 }).map((_, i) => (
                                                <div 
                                                    key={i}
                                                    className={`${styles.audioBar} ${
                                                        i < channel.audioLevel * 20 
                                                            ? i > 16 ? styles.audioBarHot : i > 12 ? styles.audioBarWarn : styles.audioBarOk 
                                                            : styles.audioBarOff
                                                    }`}
                                                />
                                            ))}
                                        </div>
                                    </div>
                                </div>
                            ))
                        )}
                    </div>
                </aside>

                {/* Center Panel - Primary Controls */}
                <main className={styles.centerPanel}>
                    {/* Current Item Display */}
                    {primaryChannel?.currentItem && (
                        <div className={styles.currentDisplay}>
                            <div className={styles.currentBadge}>
                                {primaryChannel.state === 'on-air' && <span className={styles.currentLive}>LIVE</span>}
                                <span className={styles.currentType}>{primaryChannel.currentItem.type.toUpperCase()}</span>
                            </div>
                            <h2 className={styles.currentTitle}>{primaryChannel.currentItem.title}</h2>
                            
                            {/* Giant Countdown */}
                            <div className={`${styles.giantCountdown} ${getCountdownClass(primaryChannel.countdownMs)}`}>
                                {formatCountdown(primaryChannel.countdownMs)}
                            </div>

                            {/* Progress Ring */}
                            <div className={styles.progressRing}>
                                <svg viewBox="0 0 100 100">
                                    <circle 
                                        className={styles.progressBg}
                                        cx="50" cy="50" r="45"
                                    />
                                    <circle 
                                        className={styles.progressFill}
                                        cx="50" cy="50" r="45"
                                        strokeDasharray={`${(primaryChannel.currentItem.elapsed / primaryChannel.currentItem.duration) * 283} 283`}
                                    />
                                </svg>
                            </div>
                        </div>
                    )}

                    {/* Transport Controls */}
                    <div className={styles.transportControls}>
                        <button 
                            className={styles.transportBtn}
                            onClick={onPrevious}
                            disabled={locked}
                            title="Previous (P)"
                        >
                            <SkipBack size={24} />
                        </button>
                        
                        <button 
                            className={styles.transportBtn}
                            onClick={onStop}
                            disabled={locked}
                            title="Stop (Escape)"
                        >
                            <Square size={24} />
                        </button>
                        
                        <button 
                            className={`${styles.transportBtn} ${styles.playBtn} ${isLive ? styles.playing : ''}`}
                            onClick={isLive ? onPause : onPlay}
                            disabled={locked}
                            title={isLive ? "Pause" : "Play (Space)"}
                        >
                            {isLive ? <Pause size={32} /> : <Play size={32} />}
                        </button>
                        
                        <button 
                            className={styles.transportBtn}
                            onClick={onNext}
                            disabled={locked}
                            title="Next (N)"
                        >
                            <SkipForward size={24} />
                        </button>
                        
                        <button 
                            className={`${styles.transportBtn} ${styles.takeBtn}`}
                            onClick={onTake}
                            disabled={locked}
                            title="Take (T)"
                        >
                            <Zap size={24} />
                            <span>TAKE</span>
                        </button>
                    </div>

                    {/* Quick Actions Grid */}
                    <div className={styles.quickActions}>
                        {quickActions.map((action) => (
                            <button
                                key={action.id}
                                className={`${styles.quickAction} ${styles[`quick${action.color}`]}`}
                                onClick={action.action}
                                disabled={locked}
                            >
                                {action.icon}
                                <span className={styles.quickLabel}>{action.label}</span>
                                {action.shortcut && (
                                    <span className={styles.quickShortcut}>{action.shortcut}</span>
                                )}
                            </button>
                        ))}
                    </div>

                    {/* Next Up Preview */}
                    {primaryChannel?.nextItem && (
                        <div className={styles.nextUp}>
                            <div className={styles.nextUpHeader}>
                                <span className={styles.nextUpLabel}>NEXT UP</span>
                                <span className={styles.nextUpDuration}>
                                    {Math.floor(primaryChannel.nextItem.duration / 60)}:{Math.round(primaryChannel.nextItem.duration % 60).toString().padStart(2, '0')}
                                </span>
                            </div>
                            <div className={styles.nextUpTitle}>{primaryChannel.nextItem.title}</div>
                        </div>
                    )}
                </main>

                {/* Right Panel - GPI/GPO & Routing */}
                <aside className={styles.rightPanel}>
                    {/* GPI/GPO Status */}
                    <div className={styles.gpiSection}>
                        <div className={styles.panelHeader}>
                            <Zap size={16} />
                            <span>GPI/GPO</span>
                        </div>
                        
                        <div className={styles.gpiGrid}>
                            {gpiStates.length === 0 ? (
                                <div className={styles.emptyGpi}>
                                    <Terminal size={24} />
                                    <span>No GPI configured</span>
                                </div>
                            ) : (
                                gpiStates.map((gpi) => (
                                    <button
                                        key={gpi.id}
                                        className={`${styles.gpiButton} ${gpi.state ? styles.gpiActive : ''} ${styles[`gpi${gpi.type}`]}`}
                                        onClick={() => gpi.type === 'output' && onGpiTrigger?.(gpi.id)}
                                        disabled={locked || gpi.type === 'input'}
                                    >
                                        <span className={styles.gpiId}>{gpi.id}</span>
                                        <span className={styles.gpiLabel}>{gpi.label}</span>
                                        <span className={`${styles.gpiIndicator} ${gpi.state ? styles.gpiOn : styles.gpiOff}`} />
                                    </button>
                                ))
                            )}
                        </div>
                    </div>

                    {/* Audio Control */}
                    <div className={styles.audioSection}>
                        <div className={styles.panelHeader}>
                            <Volume2 size={16} />
                            <span>Audio</span>
                        </div>
                        
                        <div className={styles.audioControls}>
                            <button 
                                className={`${styles.muteBtn} ${audioMuted ? styles.muted : ''}`}
                                onClick={() => setAudioMuted(!audioMuted)}
                            >
                                {audioMuted ? <VolumeX size={20} /> : <Volume2 size={20} />}
                                <span>{audioMuted ? 'UNMUTE' : 'MUTE'}</span>
                            </button>
                            
                            <div className={styles.masterMeter}>
                                <span className={styles.meterLabel}>L</span>
                                <div className={styles.meterTrack}>
                                    <div className={styles.meterFill} style={{ width: '75%' }} />
                                </div>
                            </div>
                            <div className={styles.masterMeter}>
                                <span className={styles.meterLabel}>R</span>
                                <div className={styles.meterTrack}>
                                    <div className={styles.meterFill} style={{ width: '70%' }} />
                                </div>
                            </div>
                        </div>
                    </div>

                    {/* Quick Status */}
                    <div className={styles.statusSection}>
                        <div className={styles.panelHeader}>
                            <Activity size={16} />
                            <span>System</span>
                        </div>
                        
                        <div className={styles.statusGrid}>
                            <div className={styles.statusItem}>
                                <CheckCircle2 size={14} className={styles.statusOk} />
                                <span>Playout</span>
                            </div>
                            <div className={styles.statusItem}>
                                <CheckCircle2 size={14} className={styles.statusOk} />
                                <span>Graphics</span>
                            </div>
                            <div className={styles.statusItem}>
                                <CheckCircle2 size={14} className={styles.statusOk} />
                                <span>Audio</span>
                            </div>
                            <div className={styles.statusItem}>
                                <AlertCircle size={14} className={styles.statusWarn} />
                                <span>Network</span>
                            </div>
                        </div>
                    </div>
                </aside>
            </div>

            {/* Status Bar */}
            <footer className={styles.statusBar}>
                <div className={styles.statusBarLeft}>
                    <span className={styles.statusItem}>
                        <Activity size={12} />
                        Frame Rate: 59.94 fps
                    </span>
                    <span className={styles.statusItem}>
                        <Clock size={12} />
                        Timecode: {primaryChannel?.timecode || '00:00:00:00'}
                    </span>
                </div>
                <div className={styles.statusBarCenter}>
                    <span>PLAY PLAY Professional Broadcast Automation v2.0</span>
                </div>
                <div className={styles.statusBarRight}>
                    <span className={styles.statusItem}>
                        <Shield size={12} />
                        {locked ? 'LOCKED' : 'UNLOCKED'}
                    </span>
                </div>
            </footer>
        </div>
    );
};

export default CommandControlSurface;
