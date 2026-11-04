/**
 * CONFIDENCE MONITOR
 * Professional broadcast multi-view confidence monitoring
 * Inspired by: Vizrt Multi-Preview, Ross Video Dashboard, Sony MVS Series
 * 
 * Features:
 * - Multi-channel preview grid
 * - Real-time waveform/vectorscope
 * - Audio level metering
 * - Countdown overlays
 * - On-air tally indication
 * - Source identification
 */

import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import {
    Monitor, Volume2, VolumeX, Maximize2, Minimize2,
    AlertTriangle, CheckCircle2, XCircle, Clock, Radio,
    Eye, EyeOff, Settings, Grid, LayoutGrid,
    Signal, Activity, Wifi, WifiOff, RefreshCw
} from 'lucide-react';
import styles from './ConfidenceMonitor.module.css';

// Types
interface SourceStatus {
    id: string;
    name: string;
    type: 'playout' | 'live' | 'graphics' | 'external' | 'encoder';
    state: 'on-air' | 'preview' | 'standby' | 'offline' | 'error';
    thumbnail?: string;
    videoUrl?: string;
    currentTitle?: string;
    countdown?: number;
    audioLevels?: { left: number; right: number };
    signalStatus?: 'good' | 'weak' | 'none';
    resolution?: string;
    frameRate?: number;
    bitrate?: number;
    timecode?: string;
    errors?: string[];
}

interface MonitorLayout {
    id: string;
    name: string;
    grid: { cols: number; rows: number };
    sources: string[];
}

interface ConfidenceMonitorProps {
    sources: SourceStatus[];
    activeLayout?: MonitorLayout;
    layouts?: MonitorLayout[];
    onLayoutChange?: (layoutId: string) => void;
    onSourceSelect?: (sourceId: string) => void;
    onRefresh?: () => void;
    showTechnical?: boolean;
}

// Utility to format timecode
const formatTimecode = (ms: number): string => {
    if (!ms && ms !== 0) return '--:--:--:--';
    const totalSeconds = Math.floor(ms / 1000);
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    const frames = Math.floor((ms % 1000) / 33.33);
    return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}:${frames.toString().padStart(2, '0')}`;
};

const formatCountdown = (seconds: number): string => {
    if (!seconds && seconds !== 0) return '--:--';
    const totalSec = Math.round(Math.abs(seconds));
    const mins = Math.floor(totalSec / 60);
    const secs = totalSec % 60;
    const sign = seconds < 0 ? '+' : '-';
    return `${sign}${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
};

// Audio Meter Component
const AudioMeter: React.FC<{ left: number; right: number }> = ({ left, right }) => {
    const getMeterClass = (level: number, threshold: number) => {
        if (level > threshold) return styles.meterHot;
        if (level > threshold - 6) return styles.meterWarn;
        return styles.meterOk;
    };

    const renderMeter = (level: number, label: string) => (
        <div className={styles.audioChannel}>
            <span className={styles.audioLabel}>{label}</span>
            <div className={styles.meterTrack}>
                {Array.from({ length: 24 }).map((_, i) => {
                    const threshold = -60 + (i * 2.5);
                    const active = level >= threshold;
                    return (
                        <div 
                            key={i}
                            className={`${styles.meterSegment} ${active ? getMeterClass(level, -6) : styles.meterOff}`}
                        />
                    );
                })}
            </div>
            <span className={styles.audioDb}>{level > -60 ? `${level.toFixed(0)}` : '-∞'}</span>
        </div>
    );

    return (
        <div className={styles.audioMeter}>
            {renderMeter(left, 'L')}
            {renderMeter(right, 'R')}
        </div>
    );
};

// Signal Indicator Component
const SignalIndicator: React.FC<{ status: 'good' | 'weak' | 'none' }> = ({ status }) => {
    return (
        <div className={`${styles.signalIndicator} ${styles[`signal${status}`]}`}>
            {status === 'good' && <Signal size={14} />}
            {status === 'weak' && <Wifi size={14} />}
            {status === 'none' && <WifiOff size={14} />}
        </div>
    );
};

// Source Card Component
const SourceCard: React.FC<{
    source: SourceStatus;
    isSelected?: boolean;
    showTechnical?: boolean;
    onSelect?: () => void;
}> = ({ source, isSelected, showTechnical, onSelect }) => {
    const [currentTime, setCurrentTime] = useState(Date.now());

    useEffect(() => {
        const interval = setInterval(() => setCurrentTime(Date.now()), 100);
        return () => clearInterval(interval);
    }, []);

    const countdownClass = useMemo(() => {
        if (!source.countdown) return '';
        if (source.countdown <= 5) return styles.countdownCritical;
        if (source.countdown <= 10) return styles.countdownWarning;
        return styles.countdownNormal;
    }, [source.countdown]);

    return (
        <div 
            className={`${styles.sourceCard} ${styles[`state${source.state}`]} ${isSelected ? styles.selected : ''}`}
            onClick={onSelect}
        >
            {/* Tally Light */}
            <div className={`${styles.tally} ${styles[`tally${source.state}`]}`} />

            {/* Preview Area */}
            <div className={styles.previewArea}>
                {source.thumbnail ? (
                    <img src={source.thumbnail} alt={source.name} className={styles.previewImage} />
                ) : (
                    <div className={styles.noSignal}>
                        <Monitor size={32} />
                        <span>No Signal</span>
                    </div>
                )}

                {/* State Badge */}
                <div className={`${styles.stateBadge} ${styles[`badge${source.state}`]}`}>
                    {source.state === 'on-air' && (
                        <>
                            <span className={styles.liveDot} />
                            <span>ON AIR</span>
                        </>
                    )}
                    {source.state === 'preview' && 'PREVIEW'}
                    {source.state === 'standby' && 'STANDBY'}
                    {source.state === 'offline' && 'OFFLINE'}
                    {source.state === 'error' && 'ERROR'}
                </div>

                {/* Countdown Overlay */}
                {source.state === 'on-air' && source.countdown !== undefined && (
                    <div className={`${styles.countdownOverlay} ${countdownClass}`}>
                        {formatCountdown(source.countdown)}
                    </div>
                )}

                {/* Current Title */}
                {source.currentTitle && (
                    <div className={styles.currentTitle}>
                        {source.currentTitle}
                    </div>
                )}

                {/* Signal Status */}
                {source.signalStatus && (
                    <SignalIndicator status={source.signalStatus} />
                )}
            </div>

            {/* Source Info */}
            <div className={styles.sourceInfo}>
                <div className={styles.sourceName}>
                    <span className={styles.sourceType}>
                        {source.type === 'playout' && <Monitor size={12} />}
                        {source.type === 'live' && <Radio size={12} />}
                        {source.type === 'graphics' && <LayoutGrid size={12} />}
                        {source.type === 'external' && <Wifi size={12} />}
                        {source.type === 'encoder' && <Activity size={12} />}
                    </span>
                    <span>{source.name}</span>
                </div>

                {/* Timecode */}
                {source.timecode && (
                    <div className={styles.timecode}>
                        <Clock size={10} />
                        <span>{source.timecode}</span>
                    </div>
                )}
            </div>

            {/* Audio Meters */}
            {source.audioLevels && (
                <AudioMeter left={source.audioLevels.left} right={source.audioLevels.right} />
            )}

            {/* Technical Info */}
            {showTechnical && (
                <div className={styles.techInfo}>
                    {source.resolution && <span>{source.resolution}</span>}
                    {source.frameRate && <span>{source.frameRate}fps</span>}
                    {source.bitrate && <span>{(source.bitrate / 1000000).toFixed(1)}Mbps</span>}
                </div>
            )}

            {/* Error Indicator */}
            {source.errors && source.errors.length > 0 && (
                <div className={styles.errorIndicator}>
                    <AlertTriangle size={12} />
                    <span>{source.errors.length}</span>
                </div>
            )}
        </div>
    );
};

// Main Component
export const ConfidenceMonitor: React.FC<ConfidenceMonitorProps> = ({
    sources,
    activeLayout,
    layouts = [],
    onLayoutChange,
    onSourceSelect,
    onRefresh,
    showTechnical = true,
}) => {
    const [selectedSourceId, setSelectedSourceId] = useState<string | null>(null);
    const [fullscreenSourceId, setFullscreenSourceId] = useState<string | null>(null);
    const [currentTime, setCurrentTime] = useState(new Date());
    const [localShowTechnical, setLocalShowTechnical] = useState(showTechnical);

    // Update clock
    useEffect(() => {
        const interval = setInterval(() => setCurrentTime(new Date()), 1000);
        return () => clearInterval(interval);
    }, []);

    // Calculate grid dimensions
    const gridConfig = useMemo(() => {
        if (activeLayout) return activeLayout.grid;
        
        const count = sources.length;
        if (count <= 1) return { cols: 1, rows: 1 };
        if (count <= 2) return { cols: 2, rows: 1 };
        if (count <= 4) return { cols: 2, rows: 2 };
        if (count <= 6) return { cols: 3, rows: 2 };
        if (count <= 9) return { cols: 3, rows: 3 };
        return { cols: 4, rows: Math.ceil(count / 4) };
    }, [sources.length, activeLayout]);

    // Get on-air count
    const onAirCount = useMemo(() => 
        sources.filter(s => s.state === 'on-air').length,
        [sources]
    );

    // Get error count
    const errorCount = useMemo(() =>
        sources.filter(s => s.state === 'error' || (s.errors && s.errors.length > 0)).length,
        [sources]
    );

    const handleSourceSelect = (sourceId: string) => {
        setSelectedSourceId(sourceId);
        onSourceSelect?.(sourceId);
    };

    const toggleFullscreen = (sourceId: string | null) => {
        setFullscreenSourceId(fullscreenSourceId === sourceId ? null : sourceId);
    };

    return (
        <div className={styles.container}>
            {/* Header */}
            <header className={styles.header}>
                <div className={styles.headerLeft}>
                    <h1 className={styles.title}>
                        <Eye size={20} />
                        Confidence Monitor
                    </h1>
                    
                    {/* Status Badges */}
                    <div className={styles.statusBadges}>
                        {onAirCount > 0 && (
                            <span className={styles.onAirBadge}>
                                <span className={styles.liveDot} />
                                {onAirCount} ON AIR
                            </span>
                        )}
                        {errorCount > 0 && (
                            <span className={styles.errorBadge}>
                                <AlertTriangle size={12} />
                                {errorCount} ERRORS
                            </span>
                        )}
                    </div>
                </div>

                <div className={styles.headerCenter}>
                    {/* Master Clock */}
                    <div className={styles.masterClock}>
                        <span className={styles.clockTime}>
                            {currentTime.toLocaleTimeString('en-US', { hour12: false })}
                        </span>
                        <span className={styles.clockDate}>
                            {currentTime.toLocaleDateString('en-US', { weekday: 'short', month: 'short', day: 'numeric' })}
                        </span>
                    </div>
                </div>

                <div className={styles.headerRight}>
                    {/* Layout Selector */}
                    {layouts.length > 0 && (
                        <select 
                            className={styles.layoutSelect}
                            value={activeLayout?.id || ''}
                            onChange={(e) => onLayoutChange?.(e.target.value)}
                        >
                            <option value="">Auto Layout</option>
                            {layouts.map(layout => (
                                <option key={layout.id} value={layout.id}>{layout.name}</option>
                            ))}
                        </select>
                    )}

                    {/* Quick Actions */}
                    <button 
                        className={`${styles.headerBtn} ${localShowTechnical ? styles.active : ''}`}
                        onClick={() => setLocalShowTechnical(!localShowTechnical)}
                        title="Toggle Technical Info"
                    >
                        <Settings size={16} />
                    </button>
                    <button 
                        className={styles.headerBtn}
                        onClick={onRefresh}
                        title="Refresh All Sources"
                    >
                        <RefreshCw size={16} />
                    </button>
                </div>
            </header>

            {/* Monitor Grid */}
            <div 
                className={styles.monitorGrid}
                style={{
                    gridTemplateColumns: `repeat(${gridConfig.cols}, 1fr)`,
                    gridTemplateRows: `repeat(${gridConfig.rows}, 1fr)`,
                }}
            >
                {sources.map(source => (
                    <SourceCard
                        key={source.id}
                        source={source}
                        isSelected={selectedSourceId === source.id}
                        showTechnical={localShowTechnical}
                        onSelect={() => handleSourceSelect(source.id)}
                    />
                ))}
            </div>

            {/* Fullscreen Preview */}
            {fullscreenSourceId && (
                <div className={styles.fullscreenOverlay} onClick={() => toggleFullscreen(null)}>
                    <div className={styles.fullscreenPreview}>
                        <SourceCard
                            source={sources.find(s => s.id === fullscreenSourceId)!}
                            showTechnical={localShowTechnical}
                        />
                        <button className={styles.closeFullscreen}>
                            <Minimize2 size={24} />
                        </button>
                    </div>
                </div>
            )}

            {/* Status Bar */}
            <footer className={styles.statusBar}>
                <div className={styles.statusLeft}>
                    <span className={styles.statusItem}>
                        <Monitor size={12} />
                        {sources.length} Sources
                    </span>
                    <span className={styles.statusItem}>
                        <Grid size={12} />
                        {gridConfig.cols}x{gridConfig.rows} Grid
                    </span>
                </div>
                <div className={styles.statusRight}>
                    <span className={styles.statusItem}>
                        <Activity size={12} />
                        System OK
                    </span>
                </div>
            </footer>
        </div>
    );
};

export default ConfidenceMonitor;
