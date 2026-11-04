// ============================================
// Shared logic for schedule grid components
// ============================================

import { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { WeeklySchedule, HourSlot, HourGridCell, SiteLocation } from '../lib/types';
import { loadWeeklySchedule, saveWeeklySchedule, createEmptyWeeklySchedule, loadClients } from '../lib/storage';
import { getSlotForSite, PLAYOUT_SITE_PROFILES } from '../lib/multiSitePlayout';

const DAYS_OF_WEEK = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const HOURS = Array.from({ length: 24 }, (_, i) => i);

const SITE_LOCATIONS: SiteLocation[] = PLAYOUT_SITE_PROFILES;

export interface UseScheduleGridOptions {
    mode?: 'schedule' | 'overview';
    viewType?: 'single' | 'multi';
    selectedSiteId?: string | null;
    startDate?: Date; // Reference date for the view
}

export function useScheduleGrid(options: UseScheduleGridOptions = {}) {
    const { mode = 'schedule', viewType = 'single', selectedSiteId = null } = options;
    
    // Calculate 7 days starting from options.startDate or today
    const [viewDate, setViewDate] = useState<Date>(options.startDate || new Date());
    
    const displayDays = useMemo(() => {
        const days = [];
        const base = new Date(viewDate);
        base.setHours(0, 0, 0, 0);
        
        for (let i = 0; i < 7; i++) {
            const d = new Date(base);
            d.setDate(base.getDate() + i);
            days.push({
                date: d,
                label: `${DAYS_OF_WEEK[d.getDay()]} ${d.getMonth() + 1}/${d.getDate()}`,
                dayOfWeek: d.getDay(),
                isoDate: d.toISOString().split('T')[0]
            });
        }
        return days;
    }, [viewDate]);

    const [schedule, setScheduleState] = useState<WeeklySchedule>(() => createEmptyWeeklySchedule());
    const [selectedSlot, setSelectedSlot] = useState<string | null>(null);
    const [loading, setLoading] = useState(true);
    const [trafficMaxAdSlotsPerHour, setTrafficMaxAdSlotsPerHour] = useState(0);
    const hasLoadedOnceRef = useRef(false);
    const hasPendingLocalSaveRef = useRef(false);

    const readTrafficAdSlotCap = useCallback(() => {
        const clients = loadClients();
        const caps = clients
            .filter(c => c.status !== 'cancelled' && c.status !== 'paused')
            .map(c => c.rules?.maxPlaysPerHour ?? 0)
            .filter(n => typeof n === 'number' && n > 0);
        setTrafficMaxAdSlotsPerHour(caps.length ? Math.max(...caps) : 0);
    }, []);

    useEffect(() => {
        readTrafficAdSlotCap();
        window.addEventListener('storage', readTrafficAdSlotCap);
        return () => window.removeEventListener('storage', readTrafficAdSlotCap);
    }, [readTrafficAdSlotCap]);

    // Navigation
    const nextWeek = () => {
        const d = new Date(viewDate);
        d.setDate(d.getDate() + 7);
        setViewDate(d);
    };

    const prevWeek = () => {
        const d = new Date(viewDate);
        d.setDate(d.getDate() - 7);
        setViewDate(d);
    };

    const goToToday = () => {
        setViewDate(new Date());
    };

    // Load schedule
    useEffect(() => {
        let mounted = true;
        const load = async () => {
            try {
                const loaded = await loadWeeklySchedule();
                if (mounted) {
                    setScheduleState(loaded);
                }
            } catch (e) {
                console.error('[useScheduleGrid] Failed to load schedule:', e);
            } finally {
                if (mounted) {
                    setLoading(false);
                }
            }
        };
        load();
        return () => { mounted = false; };
    }, []);

    // Mark that initial load has finished (so save effect is allowed to run)
    useEffect(() => {
        if (!loading) hasLoadedOnceRef.current = true;
    }, [loading]);

    // Track whether we made the last change (to avoid reloading our own changes)
    const isLocalUpdateRef = useRef(false);

    // Refresh when other parts of the app update the schedule
    useEffect(() => {
        let cancelled = false;
        const reload = async () => {
            // Skip reload if we made this change ourselves
            if (isLocalUpdateRef.current) {
                isLocalUpdateRef.current = false;
                return;
            }
            
            try {
                const updated = await loadWeeklySchedule();
                if (!cancelled) {
                    setScheduleState(updated);
                    setLoading(false);
                }
            } catch (e) {
                console.error('[useScheduleGrid] Failed to reload schedule:', e);
            }
        };

        const handler = (e: CustomEvent) => {
            // Reload on storage changes or channel switches (channel switch means a different schedule)
            if (e.detail?.source === 'storage' || e.detail?.reason === 'channel-switch') {
                reload();
            }
        };

        const channelHandler = () => reload();

        const storageHandler = () => reload();

        window.addEventListener('schedule-updated', handler as any);
        window.addEventListener('active-channel-changed', channelHandler as any);
        window.addEventListener('storage', storageHandler as any);
        return () => {
            cancelled = true;
            window.removeEventListener('schedule-updated', handler as any);
            window.removeEventListener('active-channel-changed', channelHandler as any);
            window.removeEventListener('storage', storageHandler as any);
        };
    }, []);

    // Save schedule on changes (debounced). Only after initial load has completed so we never overwrite with empty.
    useEffect(() => {
        if (!schedule || !hasLoadedOnceRef.current) return;
        if (!hasPendingLocalSaveRef.current) return;
        const slots = schedule.slots;
        if (!slots || typeof slots !== 'object') return;

        const timeoutId = setTimeout(() => {
            saveWeeklySchedule(schedule)
                .then(() => console.log('[useScheduleGrid] Save complete'))
                .catch(err => console.error('[useScheduleGrid] Save failed:', err));
            hasPendingLocalSaveRef.current = false;
        }, 1000);

        return () => clearTimeout(timeoutId);
    }, [schedule, loading]);

    // Calculate hour data for a specific site/day/hour
    const calculateHourData = useCallback((siteId: string, dayIdx: number, hour: number): HourGridCell => {
        const dayInfo = displayDays[dayIdx];
        if (!schedule || !dayInfo) {
            return {
                siteId,
                day: dayIdx,
                hour,
                filledDuration: 0,
                itemCount: 0,
                adCount: 0,
                adDurationSec: 0,
                targetAdSlotsPerHour: trafficMaxAdSlotsPerHour,
                adSlotCoveragePct: null,
                isEmpty: true,
            };
        }

        const dateSlotId = `${dayInfo.isoDate}-${hour}`;
        const slot = schedule.slots[`site:${siteId}:${dateSlotId}`] || getSlotForSite(schedule, siteId, dayInfo.dayOfWeek, hour);
        
        if (!slot || !slot.items) {
            return {
                siteId,
                day: dayIdx,
                hour,
                filledDuration: 0,
                itemCount: 0,
                adCount: 0,
                adDurationSec: 0,
                targetAdSlotsPerHour: trafficMaxAdSlotsPerHour,
                adSlotCoveragePct: null,
                isEmpty: true,
            };
        }

        const filledDuration = slot.items.reduce((sum: number, item: any) => {
            return sum + (item.media?.duration || 0);
        }, 0);

        const adItems = slot.items.filter((item: any) => item.media?.type === 'ad');
        const adCount = adItems.length;
        const adDurationSec = adItems.reduce(
            (sum: number, item: any) => sum + (item.media?.duration || 0),
            0,
        );
        const target = trafficMaxAdSlotsPerHour;
        const adSlotCoveragePct =
            target > 0 ? Math.min(100, Math.round((100 * adCount) / target)) : null;

        return {
            siteId,
            day: dayIdx,
            hour,
            filledDuration,
            itemCount: slot.items.length,
            adCount,
            adDurationSec,
            targetAdSlotsPerHour: target,
            adSlotCoveragePct,
            isEmpty: slot.items.length === 0,
        };
    }, [schedule, displayDays, trafficMaxAdSlotsPerHour]);

    // Get fill percentage
    const getFillPercentage = useCallback((cell: HourGridCell): number => {
        const maxDuration = 3600; // 1 hour in seconds
        return Math.min(100, (cell.filledDuration / maxDuration) * 100);
    }, []);

    // Get fill color based on percentage
    const getFillColor = useCallback((percentage: number): string => {
        if (percentage === 0) return 'transparent';
        if (percentage < 50) return '#ef4444'; // Red - underfilled
        if (percentage < 90) return '#f59e0b'; // Yellow - partially filled
        if (percentage < 100) return '#10b981'; // Green - well filled
        return '#3b82f6'; // Blue - overfilled
    }, []);

    // Helper functions for slot management
    const getSlot = useCallback((id: string): HourSlot | null => {
        const slots = schedule?.slots;
        if (!slots) return null;
        return slots[id] || null;
    }, [schedule]);

    const updateSlot = useCallback((id: string, slot: HourSlot) => {
        const slots = schedule?.slots;
        if (!schedule || !slots) {
            console.warn('[useScheduleGrid] updateSlot called but schedule not ready');
            return;
        }

        // Mark this as a local update so we don't reload from storage
        isLocalUpdateRef.current = true;

        hasPendingLocalSaveRef.current = true;
        const now = Date.now();
        setScheduleState(prev => ({
            ...prev,
            slots: {
                ...(prev.slots || {}),
                [id]: { ...slot, updatedAt: now }
            },
            lastModified: now
        }));
    }, [schedule]);

    const sitesToDisplay = useMemo(() => {
        if (selectedSiteId) {
            return SITE_LOCATIONS.filter(s => s.id === selectedSiteId);
        }
        return SITE_LOCATIONS;
    }, [selectedSiteId]);

    // Carry over from template or prior week
    const carryOverWeek = useCallback(async (targetStartDate: string) => {
        if (!schedule) return;

        const targetDate = new Date(targetStartDate);
        const newSlots: Record<string, HourSlot> = { ...schedule.slots };

        for (let i = 0; i < 7; i++) {
            const currentDay = new Date(targetDate);
            currentDay.setDate(targetDate.getDate() + i);
            const isoDate = currentDay.toISOString().split('T')[0];
            const dayOfWeek = currentDay.getDay();

            for (let hour = 0; hour < 24; hour++) {
                const targetId = `${isoDate}-${hour}`;
                const templateId = `${dayOfWeek}-${hour}`;
                
                // If the target date doesn't have a slot, or we want to force carry over
                const sourceSlot = schedule.slots?.[templateId];
                if (sourceSlot) {
                    const items = Array.isArray(sourceSlot.items) ? sourceSlot.items : [];
                    newSlots[targetId] = {
                        ...sourceSlot,
                        id: targetId,
                        date: isoDate,
                        items: items.map(item => ({
                            ...item,
                            id: `${Date.now().toString(36)}_${Math.random().toString(36).substr(2, 5)}`,
                            // Note: Smart Slots with type='latest' will automatically 
                            // resolve to latest episode when played
                        }))
                    };
                }
            }
        }

        hasPendingLocalSaveRef.current = true;
        setScheduleState({
            ...schedule,
            slots: newSlots,
            lastModified: Date.now(),
        });
    }, [schedule]);

    const setSchedule = useCallback((value: WeeklySchedule | ((prev: WeeklySchedule) => WeeklySchedule)) => {
        hasPendingLocalSaveRef.current = true;
        setScheduleState(value);
    }, []);

    return {
        schedule,
        setSchedule,
        selectedSlot,
        setSelectedSlot,
        loading,
        calculateHourData,
        getFillPercentage,
        getFillColor,
        getSlot,
        updateSlot,
        sitesToDisplay,
        DAYS: displayDays.map(d => d.label),
        displayDays,
        HOURS,
        SITE_LOCATIONS,
        nextWeek,
        prevWeek,
        goToToday,
        viewDate,
        carryOverWeek,
        trafficMaxAdSlotsPerHour,
        refreshTrafficAdSlotCap: readTrafficAdSlotCap,
    };
}

