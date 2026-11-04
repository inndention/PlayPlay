/**
 * BROADCAST CLOCK
 * Professional broadcast-grade precision timing display
 * Inspired by: Evertz, ESE, Wohler
 * 
 * Features:
 * - Frame-accurate timecode display
 * - Multiple timezone support
 * - Countdown/countup timers
 * - Cue point indicators
 * - NTP sync status
 * - On-air duration tracking
 */

import React, { useState, useEffect, useMemo, useCallback } from 'react';
import {
    Clock, Radio, Timer, Target, Pause, Play,
    RotateCcw, AlertCircle, CheckCircle2, Wifi
} from 'lucide-react';
import styles from './BroadcastClock.module.css';

// Types
interface TimerConfig {
    id: string;
    label: string;
    type: 'countdown' | 'countup' | 'segment';
    targetTime?: number; // Unix timestamp for countdown target
    startTime?: number; // Unix timestamp for countup start
    duration?: number; // Duration in seconds for segment timer
    warningThreshold?: number; // Seconds before warning state
    criticalThreshold?: number; // Seconds before critical state
    autoReset?: boolean;
    color?: string;
}

interface BroadcastClockProps {
    timers?: TimerConfig[];
    frameRate?: number;
    timezone?: string;
    secondaryTimezone?: string;
    isOnAir?: boolean;
    onAirStartTime?: number;
    showFrames?: boolean;
    showDate?: boolean;
    ntpSynced?: boolean;
    onTimerComplete?: (timerId: string) => void;
    onTimerStart?: (timerId: string) => void;
    onTimerReset?: (timerId: string) => void;
}

// Format helpers
const formatTime = (date: Date, showFrames: boolean, frameRate: number): string => {
    const hours = date.getHours().toString().padStart(2, '0');
    const minutes = date.getMinutes().toString().padStart(2, '0');
    const seconds = date.getSeconds().toString().padStart(2, '0');
    
    if (showFrames) {
        const ms = date.getMilliseconds();
        const frames = Math.floor((ms / 1000) * frameRate).toString().padStart(2, '0');
        return `${hours}:${minutes}:${seconds}:${frames}`;
    }
    
    return `${hours}:${minutes}:${seconds}`;
};

const formatDuration = (seconds: number, showSign: boolean = false): string => {
    const sign = seconds < 0 ? '-' : (showSign ? '+' : '');
    const abs = Math.abs(seconds);
    const h = Math.floor(abs / 3600);
    const m = Math.floor((abs % 3600) / 60);
    const s = Math.floor(abs % 60);
    
    if (h > 0) {
        return `${sign}${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    }
    return `${sign}${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
};

const formatCountdown = (seconds: number): string => {
    const sign = seconds > 0 ? '-' : '+';
    const abs = Math.abs(seconds);
    const m = Math.floor(abs / 60);
    const s = Math.floor(abs % 60);
    return `${sign}${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
};

// Timer Component
const TimerDisplay: React.FC<{
    timer: TimerConfig;
    currentTime: number;
    onComplete?: () => void;
    onStart?: () => void;
    onReset?: () => void;
}> = ({ timer, currentTime, onComplete, onStart, onReset }) => {
    const [isRunning, setIsRunning] = useState(false);
    const [manualStartTime, setManualStartTime] = useState<number | null>(null);
    
    const value = useMemo(() => {
        if (timer.type === 'countdown' && timer.targetTime) {
            return Math.ceil((timer.targetTime - currentTime) / 1000);
        }
        if (timer.type === 'countup') {
            const start = manualStartTime || timer.startTime || currentTime;
            return Math.floor((currentTime - start) / 1000);
        }
        if (timer.type === 'segment' && timer.duration) {
            const start = manualStartTime || timer.startTime || currentTime;
            const elapsed = Math.floor((currentTime - start) / 1000);
            return timer.duration - elapsed;
        }
        return 0;
    }, [timer, currentTime, manualStartTime]);

    const state = useMemo(() => {
        if (timer.type === 'countdown' || timer.type === 'segment') {
            if (value <= 0) return 'expired';
            if (timer.criticalThreshold && value <= timer.criticalThreshold) return 'critical';
            if (timer.warningThreshold && value <= timer.warningThreshold) return 'warning';
        }
        return 'normal';
    }, [value, timer]);

    useEffect(() => {
        if (state === 'expired' && isRunning) {
            setIsRunning(false);
            onComplete?.();
        }
    }, [state, isRunning, onComplete]);

    const handleToggle = () => {
        if (!isRunning) {
            setManualStartTime(currentTime);
            setIsRunning(true);
            onStart?.();
        } else {
            setIsRunning(false);
        }
    };

    const handleReset = () => {
        setIsRunning(false);
        setManualStartTime(null);
        onReset?.();
    };

    return (
        <div className={`${styles.timerCard} ${styles[`timer${state}`]}`}>
            <div className={styles.timerHeader}>
                <span className={styles.timerLabel}>{timer.label}</span>
                <span className={styles.timerType}>
                    {timer.type === 'countdown' && <Timer size={12} />}
                    {timer.type === 'countup' && <Clock size={12} />}
                    {timer.type === 'segment' && <Target size={12} />}
                </span>
            </div>
            
            <div className={`${styles.timerValue} ${styles[`value${state}`]}`}>
                {timer.type === 'countdown' || timer.type === 'segment' 
                    ? formatCountdown(value)
                    : formatDuration(value)
                }
            </div>

            <div className={styles.timerControls}>
                <button 
                    className={`${styles.timerBtn} ${isRunning ? styles.running : ''}`}
                    onClick={handleToggle}
                >
                    {isRunning ? <Pause size={14} /> : <Play size={14} />}
                </button>
                <button className={styles.timerBtn} onClick={handleReset}>
                    <RotateCcw size={14} />
                </button>
            </div>
        </div>
    );
};

// Main Component
export const BroadcastClock: React.FC<BroadcastClockProps> = ({
    timers = [],
    frameRate = 29.97,
    timezone = 'local',
    secondaryTimezone,
    isOnAir = false,
    onAirStartTime,
    showFrames = true,
    showDate = true,
    ntpSynced = true,
    onTimerComplete,
    onTimerStart,
    onTimerReset,
}) => {
    const [currentTime, setCurrentTime] = useState(Date.now());

    // High-frequency update for frame accuracy
    useEffect(() => {
        const interval = setInterval(() => {
            setCurrentTime(Date.now());
        }, showFrames ? 33 : 100); // ~30fps for frame display, 10fps otherwise
        
        return () => clearInterval(interval);
    }, [showFrames]);

    const now = useMemo(() => new Date(currentTime), [currentTime]);
    
    // Calculate on-air duration
    const onAirDuration = useMemo(() => {
        if (!isOnAir || !onAirStartTime) return 0;
        return Math.floor((currentTime - onAirStartTime) / 1000);
    }, [isOnAir, onAirStartTime, currentTime]);

    // Get secondary timezone time
    const secondaryTime = useMemo(() => {
        if (!secondaryTimezone) return null;
        try {
            return new Date(now.toLocaleString('en-US', { timeZone: secondaryTimezone }));
        } catch {
            return null;
        }
    }, [now, secondaryTimezone]);

    return (
        <div className={styles.container}>
            {/* Main Clock Display */}
            <div className={styles.mainClock}>
                {/* On-Air Indicator */}
                {isOnAir && (
                    <div className={styles.onAirBadge}>
                        <span className={styles.onAirDot} />
                        <span>ON AIR</span>
                    </div>
                )}

                {/* Primary Time */}
                <div className={styles.primaryTime}>
                    <span className={styles.timeDigits}>
                        {formatTime(now, showFrames, frameRate)}
                    </span>
                </div>

                {/* Date Display */}
                {showDate && (
                    <div className={styles.dateDisplay}>
                        {now.toLocaleDateString('en-US', { 
                            weekday: 'long', 
                            year: 'numeric', 
                            month: 'long', 
                            day: 'numeric' 
                        })}
                    </div>
                )}

                {/* Secondary Timezone */}
                {secondaryTime && (
                    <div className={styles.secondaryTime}>
                        <span className={styles.tzLabel}>{secondaryTimezone}</span>
                        <span className={styles.tzTime}>
                            {formatTime(secondaryTime, false, frameRate)}
                        </span>
                    </div>
                )}

                {/* Sync Status */}
                <div className={`${styles.syncStatus} ${ntpSynced ? styles.synced : styles.notSynced}`}>
                    {ntpSynced ? <CheckCircle2 size={12} /> : <AlertCircle size={12} />}
                    <span>{ntpSynced ? 'NTP Synced' : 'Not Synced'}</span>
                </div>
            </div>

            {/* On-Air Duration */}
            {isOnAir && (
                <div className={styles.onAirDuration}>
                    <Radio size={14} className={styles.onAirIcon} />
                    <span className={styles.durationLabel}>On-Air Duration</span>
                    <span className={styles.durationValue}>{formatDuration(onAirDuration)}</span>
                </div>
            )}

            {/* Timers Grid */}
            {timers.length > 0 && (
                <div className={styles.timersGrid}>
                    {timers.map(timer => (
                        <TimerDisplay
                            key={timer.id}
                            timer={timer}
                            currentTime={currentTime}
                            onComplete={() => onTimerComplete?.(timer.id)}
                            onStart={() => onTimerStart?.(timer.id)}
                            onReset={() => onTimerReset?.(timer.id)}
                        />
                    ))}
                </div>
            )}

            {/* Frame Rate Indicator */}
            <div className={styles.frameRateIndicator}>
                <span>{frameRate} fps</span>
            </div>
        </div>
    );
};

export default BroadcastClock;
