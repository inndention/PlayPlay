import React, { useEffect, useState, useRef, useCallback } from 'react';
import { CGOverlay } from '../components/CGOverlay';
import { PlaylistItem, CGElement } from '../lib/types';
import { waitForElectronAPI, clearEnvironmentCache, resolveMediaUrl } from '../lib/fileResolver';
import { persistentLogger } from '../lib/persistentLogger';
import { logError } from '../lib/errorBus';
import styles from './OutputPage.module.css';

// CG Layer State (from CGLivePanel)
interface CGLayerState {
    id: string;
    type: 'element' | 'template';
    element?: CGElement;
    templateId?: string;
    templateUrl?: string;
    values?: Record<string, string>;
    isVisible: boolean;
    transition: 'cut' | 'fade' | 'wipe_left' | 'wipe_right' | 'push_up' | 'push_down';
    transitionDuration: number;
    layer: number;
}

interface SyncMessage {
    type: 'UPDATE_STATE';
    payload: {
        currentMedia: PlaylistItem | undefined;
        nextMedia: PlaylistItem | undefined;
        isPlaying: boolean;
        cgElements: CGElement[];
        cgLayers?: CGLayerState[];
        currentTime?: number;
        seekTime?: number;
        timestamp?: number;
    };
}

/** Program channel id for this output surface: hash `#/output?ch=…`, hash `#/output/ch-…`, or `?channel=` / `?ch=` on the location search. */
function parseOutputChannelFromLocation(): string | null {
    try {
        const hash = window.location.hash || '';
        const pathInHash = hash.replace(/^#/, '').split('?')[0] || '';
        const seg = pathInHash.match(/^\/output\/([\w-]+)\/?$/);
        if (seg) return seg[1];
        const qIdx = hash.indexOf('?');
        if (qIdx >= 0) {
            const hp = new URLSearchParams(hash.slice(qIdx + 1));
            const c = hp.get('ch');
            if (c) return c;
        }
        const pathParams = new URLSearchParams(window.location.search);
        return pathParams.get('ch') || pathParams.get('channel');
    } catch {
        return null;
    }
}

export function OutputPage() {
    const urlParams = new URLSearchParams(window.location.search);
    const obsChannelId = parseOutputChannelFromLocation();
    /** CG / debug routing: same channel as browser API when present, else legacy `?channel=` or CH1. */
    const channelId = obsChannelId || urlParams.get('channel') || 'ch-1';
    const layerId = urlParams.get('layer') ? parseInt(urlParams.get('layer')!, 10) : undefined;

    // Detect Electron (may load late in output window); clear fileResolver cache when API becomes available
    const [isElectron, setIsElectron] = useState(!!(typeof window !== 'undefined' && (window as any).electronAPI));
    useEffect(() => {
        waitForElectronAPI(3000).then((ok) => {
            if (ok) {
                clearEnvironmentCache();
                setIsElectron(true);
            }
        });
    }, []);

    const [state, setState] = useState<{
        currentMedia: PlaylistItem | undefined;
        nextMedia: PlaylistItem | undefined;
        isPlaying: boolean;
        cgElements: CGElement[];
        cgLayers: CGLayerState[];
        currentTime: number;
        seekTime: number;
        timestamp: number;
    }>({
        currentMedia: undefined,
        nextMedia: undefined,
        isPlaying: false,
        cgElements: [],
        cgLayers: [],
        currentTime: 0,
        seekTime: 0,
        timestamp: 0,
    });
    const [debugInfo, setDebugInfo] = useState(`Initializing... Channel: ${channelId}${layerId ? ` Layer: ${layerId}` : ''}`);
    const videoRef = useRef<HTMLVideoElement>(null);
    const currentMediaIdRef = useRef<string | null>(null);
    const lastHardSeekAtRef = useRef<number>(0);
    const wakeLockRef = useRef<WakeLockSentinel | null>(null);
    const rafRef = useRef<number | null>(null);
    const audioContextRef = useRef<AudioContext | null>(null);
    
    // Load channel-specific CG layer if specified
    useEffect(() => {
        if (layerId) {
            const loadLayerContent = async () => {
                try {
                    const { getAllChannelCGConfigs } = await import('../lib/obsCGController');
                    const configs = await getAllChannelCGConfigs();
                    const config = configs[channelId];
                    const layer = config?.layers.find(l => l.layerId === layerId);
                    
                    if (layer?.content) {
                        setState(prev => ({
                            ...prev,
                            cgElements: [layer.content!],
                        }));
                    }
                } catch (error) {
                    logError('[OutputPage] Error loading layer content', error);
                }
            };
            loadLayerContent();
        }
    }, [channelId, layerId]);

    // ============================================
    // ANTI-THROTTLING TRICKS FOR OBS CAPTURE
    // ============================================

    // 1. Wake Lock API - Prevent screen sleep
    const requestWakeLock = useCallback(async () => {
        try {
            if ('wakeLock' in navigator) {
                wakeLockRef.current = await (navigator as any).wakeLock.request('screen');
                persistentLogger.debug('[Output] Wake lock acquired', undefined, 'OutputPage');
                
                wakeLockRef.current?.addEventListener('release', () => {
                    persistentLogger.debug('[Output] Wake lock released, re-acquiring...', undefined, 'OutputPage');
                    requestWakeLock();
                });
            }
        } catch (err) {
            persistentLogger.debug('[Output] Wake lock not available: ' + String(err), undefined, 'OutputPage');
        }
    }, []);

    // 2. Keep-alive RAF loop - Prevents browser from throttling
    useEffect(() => {
        let frameCount = 0;
        const keepAlive = () => {
            frameCount++;
            // Force a tiny layout recalculation to keep renderer active
            if (frameCount % 60 === 0) {
                document.body.offsetHeight; // Force reflow
            }
            rafRef.current = requestAnimationFrame(keepAlive);
        };
        
        rafRef.current = requestAnimationFrame(keepAlive);
        persistentLogger.debug('[Output] Keep-alive RAF loop started', undefined, 'OutputPage');
        
        return () => {
            if (rafRef.current) {
                cancelAnimationFrame(rafRef.current);
            }
        };
    }, []);

    // 3. Audio Context trick - Keeps audio processing active
    useEffect(() => {
        const createAudioContext = () => {
            if (!audioContextRef.current) {
                audioContextRef.current = new AudioContext();
                // Create a silent oscillator to keep context alive
                const oscillator = audioContextRef.current.createOscillator();
                const gainNode = audioContextRef.current.createGain();
                gainNode.gain.value = 0; // Silent
                oscillator.connect(gainNode);
                gainNode.connect(audioContextRef.current.destination);
                oscillator.start();
                persistentLogger.debug('[Output] Silent audio context created', undefined, 'OutputPage');
            }
        };
        
        createAudioContext();
        
        return () => {
            if (audioContextRef.current) {
                audioContextRef.current.close();
            }
        };
    }, []);

    // 4. Visibility change handler - Keep playing when window loses focus
    useEffect(() => {
        const handleVisibilityChange = () => {
            const video = videoRef.current;
            if (!video) return;
            
            persistentLogger.debug('[Output] Visibility changed: ' + document.visibilityState, undefined, 'OutputPage');
            
            if (document.visibilityState === 'hidden') {
                // Window hidden (OBS still captures) - ensure playback continues
                if (state.isPlaying && video.paused) {
                    video.play().catch((e) => persistentLogger.debug('Resume play failed: ' + String(e), undefined, 'OutputPage'));
                }
            }
            
            // Re-acquire wake lock if page becomes visible again
            if (document.visibilityState === 'visible') {
                requestWakeLock();
            }
        };
        
        document.addEventListener('visibilitychange', handleVisibilityChange);
        return () => document.removeEventListener('visibilitychange', handleVisibilityChange);
    }, [state.isPlaying, requestWakeLock]);

    // 5. Prevent page from being discarded
    useEffect(() => {
        const handleFreeze = (e: Event) => {
            persistentLogger.debug('[Output] Page freeze event - preventing', undefined, 'OutputPage');
            e.preventDefault();
        };
        
        const handleResume = () => {
            persistentLogger.debug('[Output] Page resumed', undefined, 'OutputPage');
            // Re-sync video
            const video = videoRef.current;
            if (video && state.isPlaying && video.paused) {
                video.play().catch((e) => logError('Play failed', e));
            }
        };
        
        document.addEventListener('freeze', handleFreeze);
        document.addEventListener('resume', handleResume);
        
        return () => {
            document.removeEventListener('freeze', handleFreeze);
            document.removeEventListener('resume', handleResume);
        };
    }, [state.isPlaying]);

    // 6. Web Locks API - Prevent tab from being discarded
    useEffect(() => {
        let lockHeld = false;
        
        const acquireLock = async () => {
            if ('locks' in navigator && !lockHeld) {
                try {
                    await (navigator as any).locks.request('output-window-lock', { mode: 'exclusive' }, async () => {
                        lockHeld = true;
                        persistentLogger.debug('[Output] Web Lock acquired', undefined, 'OutputPage');
                        // Hold the lock forever
                        await new Promise(() => {}); // Never resolves
                    });
                } catch (e) {
                    persistentLogger.debug('[Output] Web Lock not available', undefined, 'OutputPage');
                }
            }
        };
        
        acquireLock();
    }, []);

    // 7. Request wake lock on mount
    useEffect(() => {
        requestWakeLock();
        
        return () => {
            if (wakeLockRef.current) {
                wakeLockRef.current.release();
            }
        };
    }, [requestWakeLock]);

    // ============================================
    // STANDARD OUTPUT PAGE LOGIC
    // ============================================

    // Per-channel scoping for OBS browser sources — /output/<id> (hash `#/output?ch=`),
    // /output2 … /output16, or `?ch=` on the hash. Unscoped `/api/state` stays the
    // legacy aggregate mirror when no channel is selected.

    const getApiBase = () => {
        if (window.location.protocol === 'http:' || window.location.protocol === 'https:') {
            if (window.location.port && window.location.port !== '5173') {
                return window.location.origin;
            }
        }
        return 'http://localhost:8080';
    };
    const API_BASE = getApiBase();
    const API_URL = obsChannelId
        ? `${API_BASE}/api/state/${encodeURIComponent(obsChannelId)}`
        : `${API_BASE}/api/state`;
    const MEDIA_ENDED_URL = obsChannelId
        ? `${API_BASE}/api/state/${encodeURIComponent(obsChannelId)}/media-ended`
        : null;

    useEffect(() => {
        setDebugInfo(`Mounted. Electron: ${isElectron}`);
    }, [isElectron]);

    // Load initial state from HTTP API
    useEffect(() => {
        const fetchInitialState = async () => {
            try {
                const response = await fetch(API_URL);
                if (response.ok) {
                    const parsed = await response.json();
                    persistentLogger.debug('[Output] Loaded initial state from API', parsed, 'OutputPage');
                    setState(prev => ({
                        ...prev,
                        currentMedia: parsed.currentMedia,
                        nextMedia: parsed.nextMedia,
                        isPlaying: parsed.isPlaying || false,
                        cgElements: parsed.cgElements || [],
                        cgLayers: parsed.cgLayers || [],
                        currentTime: parsed.currentTime || 0,
                        seekTime: parsed.seekTime || 0,
                        timestamp: parsed.timestamp || Date.now(),
                    }));
                    if (parsed.currentMedia) {
                        setDebugInfo(`Loaded: ${parsed.currentMedia?.media?.title || 'media'}`);
                    }
                }
            } catch (e) {
                logError('[Output] Failed to load state from API', e);
            }
        };
        fetchInitialState();
    }, []);

    // Poll HTTP API for state updates (browser source mode only; Electron windows get IPC updates)
    useEffect(() => {
        if (isElectron) return;
        let lastUpdate = 0;
        let isMounted = true;
        let lastStateHash = '';
        
        const pollState = async () => {
            if (!isMounted) return;
            
            try {
                const response = await fetch(API_URL);
                if (response.ok) {
                    const parsed = await response.json();
                    // Only update if timestamp changed
                    if (parsed.timestamp && parsed.timestamp !== lastUpdate) {
                        lastUpdate = parsed.timestamp;

                        // Identity hash deliberately excludes currentTime — it changes
                        // on every heartbeat and would force a full re-render at 2Hz.
                        const stateHash = JSON.stringify({
                            currentMediaId: parsed.currentMedia?.id,
                            nextMediaId: parsed.nextMedia?.id,
                            isPlaying: parsed.isPlaying,
                            cgElementsCount: parsed.cgElements?.length || 0,
                            cgLayersCount: parsed.cgLayers?.length || 0,
                        });

                        if (stateHash !== lastStateHash) {
                            // Identity changed — full state replace
                            lastStateHash = stateHash;
                            setState({
                                currentMedia: parsed.currentMedia,
                                nextMedia: parsed.nextMedia,
                                isPlaying: parsed.isPlaying || false,
                                cgElements: parsed.cgElements || [],
                                cgLayers: parsed.cgLayers || [],
                                currentTime: parsed.currentTime || 0,
                                seekTime: parsed.seekTime || 0,
                                timestamp: parsed.timestamp || Date.now(),
                            });
                            if (parsed.currentMedia) {
                                setDebugInfo(`Playing: ${parsed.currentMedia?.media?.title || 'media'}`);
                            }
                        } else {
                            // Time-only update — patch currentTime without re-mounting media
                            setState(prev => ({
                                ...prev,
                                currentTime: parsed.currentTime ?? prev.currentTime,
                                timestamp: parsed.timestamp || Date.now(),
                            }));
                        }
                    }
                }
            } catch (e) {
                // Ignore fetch errors (server might not be ready)
            }
        };
        
        // Poll at 2Hz (500ms) - reduced from 10Hz to prevent excessive re-renders
        const pollInterval = setInterval(pollState, 500);
        persistentLogger.debug('[Output] Started HTTP API polling for state sync', undefined, 'OutputPage');
        
        return () => {
            isMounted = false;
            clearInterval(pollInterval);
        };
    }, [isElectron]);

    useEffect(() => {
        persistentLogger.debug('OutputPage setting up state listener, isElectron: ' + isElectron, undefined, 'OutputPage');
        if (isElectron) {
            if (!(window as any).electronAPI) {
                setDebugInfo('ERROR: electronAPI not available!');
                return;
            }
            if (!(window as any).electronAPI.onPlayoutStateUpdate) {
                setDebugInfo('ERROR: onPlayoutStateUpdate not available!');
                return;
            }
            setDebugInfo('Waiting for state updates...');
            
            const unsubscribe = (window as any).electronAPI.onPlayoutStateUpdate((newState: any) => {
                persistentLogger.debug('OutputPage received state update', newState, 'OutputPage');
                if (newState) {
                    setState({
                        currentMedia: newState.currentMedia,
                        nextMedia: newState.nextMedia,
                        isPlaying: newState.isPlaying || false,
                        cgElements: newState.cgElements || [],
                        cgLayers: newState.cgLayers || [],
                        currentTime: newState.currentTime || 0,
                        seekTime: newState.seekTime || 0,
                        timestamp: newState.timestamp || Date.now(),
                    });
                    setDebugInfo(`Received: ${newState.currentMedia?.media?.title || 'no media'} | Playing: ${newState.isPlaying} | Time: ${(newState.currentTime || 0).toFixed(1)}s`);
                }
            });

            // Now that the listener is attached, ask main for current state.
            // Closes the race where the main process fires the initial sync before
            // this useEffect runs.
            if ((window as any).electronAPI.requestPlayoutState) {
                (window as any).electronAPI.requestPlayoutState();
            }

            return () => unsubscribe();
        } else {
            // BroadcastChannel for same-browser tabs
            const channel = new BroadcastChannel('playout_sync');
            channel.onmessage = (event) => {
                const msg = event.data as SyncMessage;
                if (msg.type === 'UPDATE_STATE') {
                    setState({
                        currentMedia: msg.payload.currentMedia,
                        nextMedia: msg.payload.nextMedia,
                        isPlaying: msg.payload.isPlaying,
                        cgElements: msg.payload.cgElements || [],
                        cgLayers: msg.payload.cgLayers || [],
                        currentTime: msg.payload.currentTime || 0,
                        seekTime: msg.payload.seekTime || 0,
                        timestamp: msg.payload.timestamp || Date.now(),
                    });
                }
            };
            return () => channel.close();
        }
    }, []);

    const handleMediaEnded = () => {
        persistentLogger.debug('OutputPage: media ended', { obsChannelId }, 'OutputPage');

        // Channel-scoped path: POST to per-channel media-ended endpoint.
        // Works for OBS browser sources that have no Electron / BroadcastChannel reach.
        if (MEDIA_ENDED_URL) {
            fetch(MEDIA_ENDED_URL, { method: 'POST' }).catch(e =>
                persistentLogger.warn('media-ended POST failed', e, 'OutputPage')
            );
            return;
        }

        if (isElectron) {
            (window as any).electronAPI.notifyMediaEnded();
        } else {
            const channel = new BroadcastChannel('playout_sync');
            channel.postMessage({ type: 'MEDIA_ENDED' });
            channel.close();
        }
    };

    // Direct video control with proper seeking
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;

        const currentMedia = state.currentMedia;
        
        if (!currentMedia) {
            video.pause();
            video.src = '';
            currentMediaIdRef.current = null;
            return;
        }

        const seekTime = state.currentTime || state.seekTime || (currentMedia.timeIn || 0);
        const media = currentMedia.media;
        
        persistentLogger.debug('OutputPage: Loading media', { 
            title: media?.title,
            isPlaying: state.isPlaying,
            currentId: currentMedia.id,
            prevId: currentMediaIdRef.current,
            seekTime,
        });

        // Only reload if media changed; resolve URL so file paths become playable (avoids DEMUXER_ERROR_COULD_NOT_OPEN)
        if (currentMediaIdRef.current !== currentMedia.id) {
            currentMediaIdRef.current = currentMedia.id;
            
            if (media) {
                let cancelled = false;
                resolveMediaUrl(media).then((resolvedSrc) => {
                    if (cancelled || !video) return;
                    if (!resolvedSrc || resolvedSrc.startsWith('indexeddb:')) {
                        if (!resolvedSrc.startsWith('indexeddb:')) setDebugInfo('Video: no playable source');
                        return;
                    }
                    video.src = resolvedSrc;
                    video.load();
                    video.onloadedmetadata = () => {
                        if (cancelled) return;
                        persistentLogger.debug('OutputPage: metadata loaded, seeking to ' + seekTime, undefined, 'OutputPage');
                        if (seekTime > 0) video.currentTime = seekTime;
                    };
                    video.onseeked = () => {
                        if (cancelled) return;
                        persistentLogger.debug('OutputPage: seeked to ' + video.currentTime, undefined, 'OutputPage');
                        if (state.isPlaying) {
                            video.play().catch(e => {
                                logError('Play after seek failed', e);
                                video.muted = true;
                                video.play().catch((e2) => logError('Muted play failed', e2));
                            });
                        }
                    };
                    video.oncanplay = () => {
                        if (cancelled) return;
                        persistentLogger.debug('OutputPage: video can play', undefined, 'OutputPage');
                        if (seekTime === 0 && state.isPlaying) video.play().catch((e) => logError('Play failed', e));
                    };
                    video.onerror = () => {
                        if (cancelled) return;
                        logError('OutputPage: video error', video.error);
                        setDebugInfo(`Video error: ${video.error?.message || 'unknown'}`);
                    };
                }).catch((err) => {
                    if (!cancelled) {
                        logError('OutputPage: resolveMediaUrl failed', err);
                        setDebugInfo('Video: failed to resolve URL');
                    }
                });
                return () => { cancelled = true; };
            }
        }

        // Handle play/pause
        if (state.isPlaying && video.paused && video.readyState >= 3) {
            video.play().catch((e) => logError('Play failed', e));
        } else if (!state.isPlaying && !video.paused) {
            video.pause();
        }
    }, [state.currentMedia, state.isPlaying, state.seekTime]);

    // Drift correction loop: extrapolate from synced playout timestamp instead of hard-seeking each state tick.
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;
        if (!state.currentMedia || !state.isPlaying) return;
        if (!state.timestamp) return;

        const interval = window.setInterval(() => {
            const media = state.currentMedia;
            if (!media || !videoRef.current || videoRef.current.readyState < 2) return;
            const now = Date.now();
            const base = Number(state.currentTime || state.seekTime || 0);
            const expected = base + Math.max(0, (now - state.timestamp) / 1000);
            const drift = Math.abs(videoRef.current.currentTime - expected);
            const sinceLastSeek = now - lastHardSeekAtRef.current;
            if (drift > 1.8 && sinceLastSeek > 1800) {
                persistentLogger.debug(`[Output] Drift correction seek: ${videoRef.current.currentTime.toFixed(2)} -> ${expected.toFixed(2)} (drift ${drift.toFixed(2)}s)`, undefined, 'OutputPage');
                videoRef.current.currentTime = expected;
                lastHardSeekAtRef.current = now;
            }
        }, 450);

        return () => window.clearInterval(interval);
    }, [state.currentMedia?.id, state.isPlaying, state.currentTime, state.seekTime, state.timestamp]);

    // Handle video ended
    useEffect(() => {
        const video = videoRef.current;
        if (!video) return;

        const handleEnded = () => {
            persistentLogger.debug('OutputPage: video ended event', undefined, 'OutputPage');
            handleMediaEnded();
        };

        video.addEventListener('ended', handleEnded);
        return () => video.removeEventListener('ended', handleEnded);
    }, []);

    // Force hardware acceleration for OBS capture
    useEffect(() => {
        // Add meta tag for hardware acceleration
        const meta = document.createElement('meta');
        meta.name = 'viewport';
        meta.content = 'width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no';
        document.head.appendChild(meta);
        
        // Force GPU compositing on the body
        document.body.style.transform = 'translateZ(0)';
        document.body.style.backfaceVisibility = 'hidden';
        
        return () => {
            document.head.removeChild(meta);
        };
    }, []);

    return (
        <div className={styles.container} style={{
            // Force hardware acceleration on container
            transform: 'translate3d(0, 0, 0)',
            backfaceVisibility: 'hidden',
            perspective: '1000px',
        }}>
            {/* 
                Video element with hardware acceleration hints
                - will-change: transform forces GPU layer
                - transform: translateZ(0) ensures compositing
                - Using translate3d for better OBS capture compatibility
            */}
            <video
                ref={videoRef}
                style={{
                    position: 'absolute',
                    top: 0,
                    left: 0,
                    width: '100%',
                    height: '100%',
                    objectFit: 'contain',
                    background: '#000',
                    // Hardware acceleration for OBS
                    willChange: 'transform, opacity',
                    transform: 'translate3d(0, 0, 0)',
                    backfaceVisibility: 'hidden',
                    WebkitBackfaceVisibility: 'hidden',
                    // Prevent flickering
                    imageRendering: 'auto',
                }}
                playsInline
                autoPlay
                muted={false}
                onError={(e) => console.error('[OutputPage] Video playback error:', e)}
                onContextMenu={(e) => e.preventDefault()}
            />
            <CGOverlay elements={state.cgElements} cgLayers={state.cgLayers} />
            
            {/* Debug overlay - hidden in production */}
            {!state.currentMedia && (
                <div style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    color: '#333',
                    fontSize: '0.8rem',
                    textAlign: 'center',
                    fontFamily: 'monospace',
                }}>
                    <div style={{ fontSize: '1.2rem', marginBottom: '0.5rem' }}>OUTPUT</div>
                    <div>{debugInfo}</div>
                    <div style={{ marginTop: '1rem', fontSize: '0.6rem', color: '#555' }}>
                        Anti-throttle: Wake Lock + RAF + Web Lock
                    </div>
                </div>
            )}
        </div>
    );
}
