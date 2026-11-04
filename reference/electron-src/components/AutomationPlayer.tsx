/**
 * AUTOMATION PLAYER - The Renderer (Slave)
 * 
 * This component is NOT the brain. It executes commands from the engine.
 * The engine tells it what to play, where to seek, when to fade.
 * It reports its state back to the engine for drift detection.
 */

import React, { useEffect, useRef, useState, useCallback } from 'react';
import { useAutomationEngine } from '../hooks/useAutomationEngine';
import { CGOverlay } from './CGOverlay';
import { CGElement, MediaItem } from '../lib/types';
import { 
    Play, Square, AlertTriangle, Clock, RefreshCw, 
    Zap, Eye, EyeOff, AlertCircle, Activity, Heart, Gauge, Timer
} from 'lucide-react';
import styles from './AutomationPlayer.module.css';

interface AutomationPlayerProps {
    cgElements?: CGElement[];
    fillPool?: MediaItem[];
    slateMedia?: MediaItem;
    onStateChange?: (state: any) => void;
}

export function AutomationPlayer({
    cgElements = [],
    fillPool = [],
    slateMedia,
    onStateChange,
}: AutomationPlayerProps) {
    const videoRef = useRef<HTMLVideoElement>(null);
    const [showDebug, setShowDebug] = useState(true);
    const [wallClock, setWallClock] = useState(new Date());
    
    const {
        isRunning,
        derivedState,
        errors,
        health,
        timerStats,
        start,
        stop,
        attachVideo,
        setFillPool,
        setSlate,
        reloadSchedule,
        insertOverride,
        lockEvent,
        clearOverrides,
    } = useAutomationEngine({ enableHealthMonitor: true });
    
    // Attach video element
    useEffect(() => {
        if (videoRef.current) {
            attachVideo(videoRef.current);
        }
    }, [attachVideo]);
    
    // Set fill content
    useEffect(() => {
        setFillPool(fillPool);
    }, [fillPool, setFillPool]);
    
    useEffect(() => {
        if (slateMedia) {
            setSlate(slateMedia);
        }
    }, [slateMedia, setSlate]);
    
    // Wall clock: update at 10Hz so centiseconds shown in formatClock() stay accurate
    useEffect(() => {
        const interval = setInterval(() => {
            setWallClock(new Date());
        }, 100);
        return () => clearInterval(interval);
    }, []);
    
    // Notify parent of state changes
    useEffect(() => {
        onStateChange?.(derivedState);
    }, [derivedState, onStateChange]);
    
    // Format time display
    const formatMs = (ms: number) => {
        if (!isFinite(ms) || isNaN(ms)) return '--:--:--';
        const totalSeconds = Math.floor(ms / 1000);
        const hours = Math.floor(totalSeconds / 3600);
        const minutes = Math.floor((totalSeconds % 3600) / 60);
        const seconds = totalSeconds % 60;
        const millis = Math.floor((ms % 1000) / 10);
        return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}.${millis.toString().padStart(2, '0')}`;
    };
    
    const formatClock = (date: Date) => {
        return date.toLocaleTimeString('en-US', { 
            hour12: false, 
            hour: '2-digit', 
            minute: '2-digit', 
            second: '2-digit' 
        }) + '.' + Math.floor(date.getMilliseconds() / 10).toString().padStart(2, '0');
    };
    
    // Playback state indicator color
    const getStateColor = () => {
        if (!derivedState) return '#666';
        switch (derivedState.playbackState) {
            case 'on_air': return '#22c55e';
            case 'filling': return '#f59e0b';
            case 'drifted': return '#ef4444';
            case 'transitioning': return '#3b82f6';
            case 'gap': return '#dc2626';
            default: return '#666';
        }
    };

    return (
        <div className={styles.container}>
            {/* Video Renderer - THE SLAVE */}
            <div className={styles.videoContainer}>
                <video
                    ref={videoRef}
                    className={styles.video}
                    playsInline
                />
                
                {/* CG Layer */}
                <CGOverlay elements={cgElements} />
                
                {/* State Indicator Overlay */}
                {derivedState?.gapDetected && (
                    <div className={styles.gapWarning}>
                        <AlertCircle size={32} />
                        <span>GAP DETECTED</span>
                        <span className={styles.gapSubtext}>No content available</span>
                    </div>
                )}
                
                {derivedState?.fillActive && (
                    <div className={styles.fillIndicator}>
                        <AlertTriangle size={20} />
                        <span>FILL</span>
                    </div>
                )}
                
                {/* On-Air Tally */}
                {isRunning && derivedState?.playbackState === 'on_air' && (
                    <div className={styles.tally}>
                        <span className={styles.tallyDot}>●</span>
                        <span>ON AIR</span>
                    </div>
                )}
                
                {/* Drift Warning */}
                {derivedState?.playbackState === 'drifted' && (
                    <div className={styles.driftOverlay}>
                        <RefreshCw size={16} className={styles.driftIcon} />
                        <span>CORRECTING</span>
                    </div>
                )}
            </div>
            
            {/* Master Clock */}
            <div className={styles.masterClock}>
                <Clock size={14} />
                <span className={styles.clockTime}>{formatClock(wallClock)}</span>
                <span className={styles.clockLabel}>WALL</span>
            </div>
            
            {/* Engine Status Bar */}
            <div className={styles.statusBar}>
                <div className={styles.statusSection}>
                    <div 
                        className={styles.stateIndicator}
                        style={{ background: getStateColor() }}
                    >
                        <Activity size={12} />
                        <span>{derivedState?.playbackState?.toUpperCase() || 'STANDBY'}</span>
                    </div>
                    
                    <div className={`${styles.engineBadge} ${isRunning ? styles.running : ''}`}>
                        <Zap size={12} />
                        <span>{isRunning ? `${Math.round(timerStats?.totalTicks && timerStats.uptime > 0 ? (timerStats.totalTicks / (timerStats.uptime / 1000)) : 0)}Hz` : 'OFF'}</span>
                    </div>
                </div>
                
                <div className={styles.statusSection}>
                    <div className={styles.positionDisplay}>
                        <span className={styles.posLabel}>POS</span>
                        <span className={styles.posValue}>{formatMs(derivedState?.expectedPosition || 0)}</span>
                    </div>
                    
                    <div className={styles.driftDisplay}>
                        <span className={styles.driftLabel}>DRIFT</span>
                        <span className={`${styles.driftValue} ${(derivedState?.drift || 0) > 100 ? styles.driftBad : ''}`}>
                            {derivedState?.drift !== Infinity 
                                ? `${Math.round(derivedState?.drift || 0)}ms` 
                                : 'WRONG MEDIA'}
                        </span>
                    </div>
                </div>
                
                <div className={styles.statusSection}>
                    <div className={styles.nextDisplay}>
                        <span className={styles.nextLabel}>NEXT IN</span>
                        <span className={styles.nextValue}>
                            {derivedState?.timeToNext && derivedState.timeToNext < Infinity
                                ? formatMs(derivedState.timeToNext)
                                : '---'}
                        </span>
                    </div>
                </div>
            </div>
            
            {/* Controls */}
            <div className={styles.controls}>
                <button 
                    className={`${styles.controlBtn} ${styles.primary} ${isRunning ? styles.running : ''}`}
                    onClick={isRunning ? stop : start}
                >
                    {isRunning ? <Square size={16} /> : <Play size={16} />}
                    <span>{isRunning ? 'STOP' : 'START'}</span>
                </button>
                
                <button 
                    className={styles.controlBtn}
                    onClick={reloadSchedule}
                    disabled={!isRunning}
                >
                    <RefreshCw size={14} />
                    <span>Reload</span>
                </button>
                
                <button 
                    className={styles.controlBtn}
                    onClick={clearOverrides}
                    disabled={!isRunning}
                >
                    <AlertTriangle size={14} />
                    <span>Clear Overrides</span>
                </button>
                
                <div className={styles.spacer} />
                
                <button 
                    className={`${styles.controlBtn} ${showDebug ? styles.active : ''}`}
                    onClick={() => setShowDebug(!showDebug)}
                >
                    {showDebug ? <EyeOff size={14} /> : <Eye size={14} />}
                    <span>Debug</span>
                </button>
            </div>
            
            {/* Debug Panel */}
            {showDebug && (
                <div className={styles.debugPanel}>
                    <div className={styles.debugHeader}>
                        <span>ENGINE STATE</span>
                        <span className={styles.debugTimestamp}>
                            {derivedState?.timestamp ? new Date(derivedState.timestamp).toISOString() : '---'}
                        </span>
                    </div>
                    
                    <div className={styles.debugGrid}>
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Mode</div>
                            <div className={styles.debugCardValue}>{derivedState?.mode || 'standby'}</div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Playback State</div>
                            <div className={styles.debugCardValue} style={{ color: getStateColor() }}>
                                {derivedState?.playbackState || 'standby'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Active Event</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.activeEventId || 'none'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Correction</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.correctionAction || 'none'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Expected Pos</div>
                            <div className={styles.debugCardValue}>
                                {formatMs(derivedState?.expectedPosition || 0)}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Actual Pos</div>
                            <div className={styles.debugCardValue}>
                                {formatMs(derivedState?.actualPosition || 0)}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Drift</div>
                            <div className={`${styles.debugCardValue} ${(derivedState?.drift || 0) > 100 ? styles.warning : ''}`}>
                                {derivedState?.drift !== Infinity 
                                    ? `${Math.round(derivedState?.drift || 0)}ms` 
                                    : '∞'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Prerolling</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.isPrerolling ? 'YES' : 'NO'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Next Event</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.nextEventId || 'none'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Time to Next</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.timeToNext && derivedState.timeToNext < Infinity
                                    ? formatMs(derivedState.timeToNext)
                                    : '∞'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Fill Active</div>
                            <div className={styles.debugCardValue}>
                                {derivedState?.fillActive ? 'YES' : 'NO'}
                            </div>
                        </div>
                        
                        <div className={styles.debugCard}>
                            <div className={styles.debugCardTitle}>Gap (BUG)</div>
                            <div className={`${styles.debugCardValue} ${derivedState?.gapDetected ? styles.error : ''}`}>
                                {derivedState?.gapDetected ? 'YES!' : 'NO'}
                            </div>
                        </div>
                    </div>
                    
                    {/* Transition Decision */}
                    {derivedState?.transitionDecision && (
                        <div className={styles.transitionInfo}>
                            <span className={styles.transitionLabel}>Transition:</span>
                            <span className={styles.transitionAction}>{derivedState.transitionDecision.action}</span>
                            <span className={styles.transitionReason}>{derivedState.transitionDecision.reason}</span>
                        </div>
                    )}
                    
                    {/* Health Monitor (v3) */}
                    {health && (
                        <div className={styles.healthSection}>
                            <div className={styles.healthHeader}>
                                <Heart size={12} />
                                <span>ENGINE HEALTH</span>
                                <span 
                                    className={styles.healthBadge}
                                    data-level={health.level}
                                >
                                    {health.score}%
                                </span>
                            </div>
                            <div className={styles.healthGrid}>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Tick Rate</span>
                                    <span className={styles.healthValue}>{health.metrics.tickRate.toFixed(1)} Hz</span>
                                </div>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Avg Jitter</span>
                                    <span className={`${styles.healthValue} ${health.metrics.averageTickJitter > 20 ? styles.warning : ''}`}>
                                        {health.metrics.averageTickJitter.toFixed(1)}ms
                                    </span>
                                </div>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Max Jitter</span>
                                    <span className={`${styles.healthValue} ${health.metrics.maxTickJitter > 50 ? styles.warning : ''}`}>
                                        {health.metrics.maxTickJitter.toFixed(1)}ms
                                    </span>
                                </div>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Missed Ticks</span>
                                    <span className={`${styles.healthValue} ${health.metrics.missedTicks > 0 ? styles.warning : ''}`}>
                                        {health.metrics.missedTicks}
                                    </span>
                                </div>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Drift Events</span>
                                    <span className={styles.healthValue}>{health.metrics.driftEvents}</span>
                                </div>
                                <div className={styles.healthItem}>
                                    <span className={styles.healthLabel}>Gap Events</span>
                                    <span className={`${styles.healthValue} ${health.metrics.gapEvents > 0 ? styles.error : ''}`}>
                                        {health.metrics.gapEvents}
                                    </span>
                                </div>
                            </div>
                            {health.recommendations.length > 0 && (
                                <div className={styles.recommendations}>
                                    {health.recommendations.slice(0, 2).map((rec, i) => (
                                        <div key={i} className={styles.recommendation}>
                                            <AlertTriangle size={10} />
                                            <span>{rec}</span>
                                        </div>
                                    ))}
                                </div>
                            )}
                        </div>
                    )}
                    
                    {/* Timer Stats */}
                    {timerStats && (
                        <div className={styles.timerStats}>
                            <div className={styles.timerHeader}>
                                <Timer size={12} />
                                <span>PRECISION TIMER</span>
                                <span className={`${styles.timerHealthy} ${timerStats.isHealthy ? styles.healthy : styles.unhealthy}`}>
                                    {timerStats.isHealthy ? '●' : '○'}
                                </span>
                            </div>
                            <div className={styles.timerGrid}>
                                <span>Ticks: {timerStats.totalTicks}</span>
                                <span>Missed: {timerStats.missedTicks}</span>
                                <span>Score: {timerStats.healthScore}%</span>
                                <span>Uptime: {(timerStats.uptime / 1000).toFixed(0)}s</span>
                            </div>
                        </div>
                    )}
                    
                    {/* Recent Errors */}
                    {errors.length > 0 && (
                        <div className={styles.errorLog}>
                            <div className={styles.errorTitle}>Recent Errors</div>
                            {errors.slice(-3).map((err, i) => (
                                <div key={i} className={styles.errorItem}>{err.message}</div>
                            ))}
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}
