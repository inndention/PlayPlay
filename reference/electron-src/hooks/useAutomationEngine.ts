/**
 * React Hook for Time-Authoritative Automation Engine v3
 * 
 * The renderer is a SLAVE. It executes commands from the engine.
 * The engine is the BRAIN. It derives state from time + schedule.
 * 
 * v3 Improvements:
 * - Higher-frequency renderer state reporting (RAF-based)
 * - Health monitoring integration
 * - Better error recovery
 * - Reduced latency in state sync
 */

import { useEffect, useRef, useState, useCallback } from 'react';
import { 
    AutomationEngine,
    EngineConfig,
    getEngine,
    resetEngine,
} from '../lib/automationEngine';
import {
    DerivedState,
    RendererCommand,
    PolicyConfig,
    DEFAULT_POLICY,
} from '../lib/automationStateMachine';
import { TimerStats, TimerConfig } from '../lib/precisionTimer';
import { HealthSnapshot, HealthAlert } from '../lib/engineHealthMonitor';
import { WeeklySchedule, MediaItem } from '../lib/types';
import { loadWeeklySchedule } from '../lib/storage';

export interface UseAutomationEngineOptions {
    policy?: Partial<PolicyConfig>;
    timer?: Partial<TimerConfig>;
    autoStart?: boolean;
    enableHealthMonitor?: boolean;
}

export function useAutomationEngine(options: UseAutomationEngineOptions = {}) {
    const { policy, timer, autoStart = false, enableHealthMonitor = true } = options;
    
    const engineRef = useRef<AutomationEngine | null>(null);
    const videoRef = useRef<HTMLVideoElement | null>(null);
    const rafRef = useRef<number | null>(null);
    const lastReportTime = useRef<number>(0);
    
    const [isRunning, setIsRunning] = useState(false);
    const [derivedState, setDerivedState] = useState<DerivedState | null>(null);
    const [errors, setErrors] = useState<Error[]>([]);
    const [health, setHealth] = useState<HealthSnapshot | null>(null);
    const [timerStats, setTimerStats] = useState<TimerStats | null>(null);
    
    // Command execution - the renderer obeys
    const executeCommand = useCallback((cmd: RendererCommand) => {
        const video = videoRef.current;
        
        console.log('[Renderer] Executing:', cmd.type, cmd);
        
        switch (cmd.type) {
            case 'load':
                if (video && cmd.mediaSrc) {
                    // Set source
                    video.src = cmd.mediaSrc;
                    video.load();
                    
                    // Seek after load
                    const handleLoaded = () => {
                        if (cmd.seekTo !== undefined && cmd.seekTo > 0) {
                            video.currentTime = cmd.seekTo / 1000;
                        }
                        video.removeEventListener('loadedmetadata', handleLoaded);
                    };
                    video.addEventListener('loadedmetadata', handleLoaded);
                }
                break;
                
            case 'seek':
                if (video && cmd.seekTo !== undefined) {
                    video.currentTime = cmd.seekTo / 1000;
                }
                break;
                
            case 'play':
                video?.play().catch(e => {
                    console.error('[Renderer] Play failed:', e);
                    // Auto-mute and retry
                    if (video) {
                        video.muted = true;
                        video.play().catch(e2 => {
                            console.error('[Renderer] Muted play also failed:', e2);
                        });
                    }
                });
                break;
                
            case 'pause':
                video?.pause();
                break;
                
            case 'stop':
                if (video) {
                    video.pause();
                    video.removeAttribute('src');
                    video.load();
                }
                break;
                
            case 'fade':
                if (video) {
                    const duration = cmd.fadeDuration ?? 500;
                    video.style.transition = `opacity ${duration}ms ease`;
                    video.style.opacity = cmd.fadeDirection === 'out' ? '0' : '1';
                    // Remove inline transition once done so future hard-cuts aren't animated
                    const onEnd = () => {
                        video.style.transition = '';
                        video.removeEventListener('transitionend', onEnd);
                    };
                    video.addEventListener('transitionend', onEnd, { once: true });
                }
                break;
        }
        
        // Report back to engine
        if (video && engineRef.current) {
            const mediaId = cmd.mediaId || video.src;
            const position = video.currentTime * 1000;
            engineRef.current.reportRendererState(mediaId, position);
        }
    }, []);
    
    // State change handler
    const handleStateChange = useCallback((state: DerivedState) => {
        setDerivedState(state);
    }, []);
    
    // Error handler
    const handleError = useCallback((error: Error) => {
        setErrors(prev => [...prev.slice(-9), error]);
    }, []);
    
    // Initialize engine with new config
    useEffect(() => {
        const config: EngineConfig = {
            policy,
            timer,
            enableHealthMonitor,
        };
        engineRef.current = getEngine(config);
        
        return () => {
            // Don't reset on unmount - engine might be shared
        };
    }, [policy, timer, enableHealthMonitor]);
    
    // Report renderer state using requestAnimationFrame for precision
    // This is much more reliable than timeupdate events or setInterval
    useEffect(() => {
        const video = videoRef.current;
        if (!video || !engineRef.current) return;
        
        const reportState = () => {
            if (!engineRef.current || !video.src) return;
            
            const now = performance.now();
            // Throttle to ~60Hz (match display refresh)
            if (now - lastReportTime.current >= 16) {
                engineRef.current.reportRendererState(
                    video.src,
                    video.currentTime * 1000
                );
                lastReportTime.current = now;
            }
        };
        
        // Use RAF for smooth, consistent reporting
        let active = true;
        const rafLoop = () => {
            if (!active) return;
            reportState();
            rafRef.current = requestAnimationFrame(rafLoop);
        };

        if (isRunning) {
            rafRef.current = requestAnimationFrame(rafLoop);
        }

        // Also listen to timeupdate as backup
        video.addEventListener('timeupdate', reportState);

        return () => {
            active = false; // stop the closure regardless of isRunning state
            video.removeEventListener('timeupdate', reportState);
            if (rafRef.current !== null) {
                cancelAnimationFrame(rafRef.current);
                rafRef.current = null;
            }
        };
    }, [isRunning]);
    
    // API
    const start = useCallback(async () => {
        if (!engineRef.current || isRunning) return;
        
        // Load current schedule
        const schedule = await loadWeeklySchedule();
        engineRef.current.loadSchedule(schedule);
        
        // Start engine
        engineRef.current.start({
            onCommand: executeCommand,
            onStateChange: handleStateChange,
            onError: handleError,
        });
        
        setIsRunning(true);
    }, [isRunning, executeCommand, handleStateChange, handleError]);
    
    const stop = useCallback(() => {
        if (!engineRef.current || !isRunning) return;

        // Cancel the RAF reporter before the engine stops
        if (rafRef.current !== null) {
            cancelAnimationFrame(rafRef.current);
            rafRef.current = null;
        }

        engineRef.current.stop();
        setIsRunning(false);
        setDerivedState(null);
    }, [isRunning]);
    
    const attachVideo = useCallback((video: HTMLVideoElement | null) => {
        videoRef.current = video;
    }, []);
    
    const reloadSchedule = useCallback(async () => {
        if (!engineRef.current) return;
        const schedule = await loadWeeklySchedule();
        engineRef.current.loadSchedule(schedule);
    }, []);
    
    const setFillPool = useCallback((media: MediaItem[]) => {
        engineRef.current?.setFillPool(media);
    }, []);
    
    const setSlate = useCallback((media: MediaItem) => {
        engineRef.current?.setSlate(media);
    }, []);
    
    const insertOverride = useCallback((media: MediaItem, startTime: number, endTime: number) => {
        engineRef.current?.insertOverride(media, startTime, endTime);
    }, []);
    
    const lockEvent = useCallback((eventId: string, untilTime: number) => {
        engineRef.current?.lockEvent(eventId, untilTime);
    }, []);
    
    const unlockEvent = useCallback((eventId: string) => {
        engineRef.current?.unlockEvent(eventId);
    }, []);
    
    const clearOverrides = useCallback(() => {
        engineRef.current?.clearAllOverrides();
    }, []);
    
    // Poll health and timer stats
    useEffect(() => {
        if (!isRunning || !enableHealthMonitor) return;
        
        const pollHealth = () => {
            if (engineRef.current) {
                const h = engineRef.current.getHealth();
                if (h) setHealth(h);
                
                const s = engineRef.current.getTimerStats();
                setTimerStats(s);
            }
        };
        
        // Poll at 1Hz for health updates
        const interval = setInterval(pollHealth, 1000);
        pollHealth(); // Initial poll
        
        return () => clearInterval(interval);
    }, [isRunning, enableHealthMonitor]);
    
    // Get immediate stats
    const getHealth = useCallback(() => {
        return engineRef.current?.getHealth() ?? null;
    }, []);
    
    const getTimerStats = useCallback(() => {
        return engineRef.current?.getTimerStats() ?? null;
    }, []);
    
    return {
        // State
        isRunning,
        derivedState,
        errors,
        
        // Health & Monitoring (v3)
        health,
        timerStats,
        getHealth,
        getTimerStats,
        
        // Lifecycle
        start,
        stop,
        attachVideo,
        
        // Schedule
        reloadSchedule,
        setFillPool,
        setSlate,
        
        // Control
        insertOverride,
        lockEvent,
        unlockEvent,
        clearOverrides,
    };
}
