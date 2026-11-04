import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { Eye, EyeOff, Globe, Radio } from 'lucide-react';
import styles from './MultiSiteTicker.module.css';
import { SiteLocation, SiteState } from '../lib/types';
import { loadSiteStates, saveSiteStates, loadSiteVisibility, saveSiteVisibility } from '../lib/storage';
import { trackerApi } from '../lib/trackerApi';
import { PLAYOUT_SITE_PROFILES } from '../lib/multiSitePlayout';

export interface MultiSiteTickerProps {
    scheduleStartTime?: number;
    onToggleSiteInterface?: (locationId: string) => void;
    activeSiteId?: string | null;
}

const SITE_LOCATIONS: SiteLocation[] = PLAYOUT_SITE_PROFILES;

export function MultiSiteTicker({ scheduleStartTime, onToggleSiteInterface, activeSiteId }: MultiSiteTickerProps) {
    const [siteStates, setSiteStates] = useState<Record<string, SiteState>>({});
    const [visibleSites, setVisibleSites] = useState<Set<string>>(new Set());
    const [isLoading, setIsLoading] = useState(false);
    const [hasConnection, setHasConnection] = useState(false);
    const [isTrackerEnabled, setIsTrackerEnabled] = useState(false);

    // Check if tracker is enabled and has connection
    useEffect(() => {
        const checkConnection = async () => {
            const enabled = trackerApi.isEnabled();
            setIsTrackerEnabled(enabled);
            
            if (!enabled) {
                setHasConnection(false);
                return;
            }
            
            // Check if we have an actual connection state
            const isConnected = trackerApi.getConnectionState();
            if (isConnected) {
                // Try to get site states to verify connection is working
                try {
                    const states = await trackerApi.getSiteStates();
                    // Connection exists if we can reach the API (even if no sites are configured)
                    setHasConnection(states !== null);
                } catch (err) {
                    setHasConnection(false);
                }
            } else {
                setHasConnection(false);
            }
        };
        
        checkConnection();
        // Re-check every 10 seconds
        const interval = setInterval(checkConnection, 10000);
        
        // Also listen for connection state changes
        const unsubscribe = trackerApi.onConnectionStateChange((connected) => {
            setHasConnection(connected);
        });
        
        return () => {
            clearInterval(interval);
            unsubscribe();
        };
    }, []);

    // Load initial visibility and states - only if tracker is enabled
    useEffect(() => {
        if (!isTrackerEnabled) {
            // Don't load anything if tracker is not enabled
            setVisibleSites(new Set());
            setSiteStates({});
            return;
        }
        
        const loadInitial = async () => {
            const visibility = await loadSiteVisibility();
            const states = await loadSiteStates();
            
            // Only show sites that have been explicitly made visible AND have connection
            const savedVisible = new Set(visibility);
            setVisibleSites(savedVisible);
            
            setSiteStates(states);
        };
        loadInitial();
    }, [isTrackerEnabled]);

    // Sync with backend tracker.php - only if enabled and connected
    useEffect(() => {
        // Check if tracker is enabled - if not, don't set up sync
        if (!trackerApi.isEnabled()) {
            return;
        }

        let isMounted = true;
        let syncInterval: NodeJS.Timeout | null = null;

        const sync = async () => {
            if (!isMounted || !trackerApi.isEnabled()) return;
            
            try {
                const remoteStates = await trackerApi.getSiteStates();
                if (remoteStates && isMounted) {
                    // Only update if there are actual changes to prevent unnecessary re-renders
                    setSiteStates(prev => {
                        const hasChanges = Object.keys(remoteStates).some(key => {
                            const remote = remoteStates[key];
                            const local = prev[key];
                            if (!local) return true;
                            // Compare key fields to avoid unnecessary updates
                            return remote.timestamp !== local.timestamp ||
                                   remote.isPlaying !== local.isPlaying ||
                                   remote.currentMedia?.id !== local.currentMedia?.id;
                        });
                        
                        if (!hasChanges) return prev; // No changes, return same reference
                        
                        return {
                            ...prev,
                            ...remoteStates,
                        };
                    });
                }
            } catch (err) {
                // Silently fail - don't spam console if API is not available
                // Only log if we were previously connected
            }
        };

        // Initial sync after a short delay
        const initialTimeout = setTimeout(sync, 500);
        
        // Then sync every 5 seconds (less frequent to reduce load)
        syncInterval = setInterval(sync, 5000);

        return () => {
            isMounted = false;
            clearTimeout(initialTimeout);
            if (syncInterval) {
                clearInterval(syncInterval);
            }
        };
    }, []);

    // Save visibility changes - use a ref to track previous value and debounce
    const prevVisibleSitesRef = useRef<string>('');
    useEffect(() => {
        const current = Array.from(visibleSites).sort().join(',');
        if (current !== prevVisibleSitesRef.current) {
            prevVisibleSitesRef.current = current;
            saveSiteVisibility(Array.from(visibleSites)).catch(console.error);
        }
    }, [visibleSites]);

    // Save state changes - debounce to prevent constant saves
    const saveTimeoutRef = useRef<NodeJS.Timeout | null>(null);
    useEffect(() => {
        if (Object.keys(siteStates).length > 0) {
            if (saveTimeoutRef.current) {
                clearTimeout(saveTimeoutRef.current);
            }
            saveTimeoutRef.current = setTimeout(() => {
                saveSiteStates(siteStates).catch(console.error);
                saveTimeoutRef.current = null;
            }, 1000); // Debounce saves by 1 second
        }
        return () => {
            if (saveTimeoutRef.current) {
                clearTimeout(saveTimeoutRef.current);
            }
        };
    }, [siteStates]);

    const toggleVisibility = useCallback((siteId: string) => {
        setVisibleSites(prev => {
            const next = new Set(prev);
            if (next.has(siteId)) {
                next.delete(siteId);
            } else {
                next.add(siteId);
            }
            return next;
        });
    }, []);

    const handleTickerClick = useCallback((siteId: string) => {
        if (onToggleSiteInterface) {
            onToggleSiteInterface(siteId);
        }
    }, [onToggleSiteInterface]);

    const visibleLocations = useMemo(() => {
        return SITE_LOCATIONS.filter(loc => visibleSites.has(loc.id));
    }, [visibleSites]);

    // Memoize default state to prevent creating new objects on every render
    const defaultState = useMemo(() => ({
        locationId: '',
        currentMedia: null,
        nextMedia: null,
        isPlaying: false,
        currentTime: 0,
        timestamp: 0,
    }), []);

    const getSiteState = useCallback((siteId: string): SiteState => {
        const state = siteStates[siteId];
        if (state) return state;
        
        // Return a stable default object (don't create new one each time)
        return {
            ...defaultState,
            locationId: siteId,
        };
    }, [siteStates, defaultState]);

    const formatTime = (ms: number) => {
        const d = new Date(ms);
        return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    };

    // Don't render if tracker is not enabled
    if (!isTrackerEnabled) {
        return null;
    }

    return (
        <div className={styles.multiSiteTicker} style={{ minHeight: '40px' }}>
            {!hasConnection ? (
                <div className={styles.emptyMessage} style={{ padding: '10px' }}>
                    Connecting to tracker API...
                </div>
            ) : (
                <>
                    <div className={styles.header}>
                        <div className={styles.title}>Multi-Site Tickers</div>
                        <div className={styles.siteToggleList}>
                            {SITE_LOCATIONS.map(loc => (
                                <button
                                    key={loc.id}
                                    className={`${styles.toggleBtn} ${visibleSites.has(loc.id) ? styles.visible : ''}`}
                                    onClick={() => toggleVisibility(loc.id)}
                                    title={`${visibleSites.has(loc.id) ? 'Hide' : 'Show'} ${loc.name}`}
                                >
                                    {visibleSites.has(loc.id) ? (
                                        <Eye size={14} />
                                    ) : (
                                        <EyeOff size={14} />
                                    )}
                                    <span>{loc.name}</span>
                                </button>
                            ))}
                        </div>
                    </div>

                    <div className={styles.tickersContainer}>
                        {visibleLocations.length === 0 ? (
                            <div className={styles.emptyMessage}>
                                No remote sites configured. Enable tracker API and connect remote sites to see status.
                            </div>
                        ) : (
                            visibleLocations.map(loc => {
                            const state = getSiteState(loc.id);
                            const isActive = state.isPlaying;
                            const currentTitle = state.currentMedia?.title || 'Idle';
                            const nextTitle = state.nextMedia?.title || '—';

                            return (
                                <div
                                    key={loc.id}
                                    className={`${styles.ticker} ${isActive ? styles.active : ''} ${activeSiteId === loc.id ? styles.activeSite : ''}`}
                                    onClick={() => handleTickerClick(loc.id)}
                                    title={`Click to toggle ${loc.name} interface`}
                                >
                                    <div className={styles.tickerHeader}>
                                        <div className={styles.tickerIcon}>
                                            {loc.type === 'radio' ? <Radio size={14} /> : <Globe size={14} />}
                                        </div>
                                        <div className={styles.tickerName}>{loc.name}</div>
                                        {activeSiteId === loc.id && (
                                            <div className={styles.activeIndicator} style={{ width: '8px', height: '8px', borderRadius: '50%', background: '#22c55e', marginLeft: 'auto' }} />
                                        )}
                                    </div>
                                    <div className={styles.tickerContent}>
                                        <div className={styles.tickerCurrent}>
                                            <span className={styles.tickerLabel}>Now:</span>
                                            <span className={styles.tickerValue}>{currentTitle}</span>
                                        </div>
                                        <div className={styles.tickerNext}>
                                            <span className={styles.tickerLabel}>Next:</span>
                                            <span className={styles.tickerValue}>{nextTitle}</span>
                                        </div>
                                        {state.currentTime > 0 && (
                                            <div className={styles.tickerTime}>
                                                {formatTime(state.timestamp)}
                                            </div>
                                        )}
                                    </div>
                                    {isActive && <div className={styles.activeIndicator} />}
                                </div>
                            );
                        }))}
                    </div>
                </>
            )}
        </div>
    );
}
