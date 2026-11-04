// ============================================
// Unified Scheduler Component
// Combines WeeklyScheduler and MultiGridView functionality
// Supports both schedule editing and multi-site overview modes
// ============================================

import React, { useState, useCallback, useRef, useEffect, useMemo } from 'react';
import { HourSlot, WeeklySchedule, PlaylistItem, MediaItem, OBSSession, SiteLocation } from '../lib/types';
import { ErrorBoundary } from './ErrorBoundary';
import { HourSlotEditor } from './HourSlotEditor';
import { HourOperationsMenu } from './HourOperationsMenu';
import { ContextMenu } from './ContextMenu';
import { useScheduleGrid } from '../hooks/useScheduleGrid';
import { 
    copyHourToClipboard, 
    getClipboardSlot, 
    pasteHourFromClipboard,
    copyDayToClipboard,
    getClipboardDay,
    pasteDayFromClipboard,
    createFreePlaylist, 
    loadFreePlaylists, 
    saveFreePlaylists, 
    createEmptyWeeklySchedule 
} from '../lib/storage';
import { buildSchedulerHourMenuItems, buildSchedulerDayMenuItems } from '../lib/contextMenuActions';
import { executeContextMenuAction } from '../lib/contextMenuRegistry';
import { safePrompt } from '../lib/promptPolyfill';
import { formatDuration } from '../lib/utils';
import { aiScheduler } from '../lib/aiScheduler';
import { loadLibraryMedia, loadLocalMedia } from '../lib/storage';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { describeScheduleShift, makeSiteSlotId, PLAYOUT_SITE_PROFILES, shiftScheduleBlock } from '../lib/multiSitePlayout';
import { buildShakenSlotItems, fetchEligibleAdPool } from '../lib/shakeHourAds';
import { persistentLogger } from '../lib/persistentLogger';
import {
    createPlaylistItemFromMedia,
    getActiveChannelId,
    mergeShowHourAppendItems,
    resolveOrCreateMediaFromDiskPath,
    SHOW_HOUR_MAX_WATCH_FILES,
    slotHasFilePath,
    isDemotableProgramLikeRow,
} from '../lib/showHourMerge';
import { 
    Copy, Clipboard, ArrowRight, ArrowDown, Repeat, Download, Shuffle, Lock, 
    MoreHorizontal, X, Grid, Calendar, Edit2, Eye, ChevronLeft, ChevronRight, RefreshCw,
    Send, Radio, Wifi, WifiOff, Sparkles
} from 'lucide-react';
import styles from './UnifiedScheduler.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

// Parse slotId which can be either "D-H" (template) or "YYYY-MM-DD-H" (date-specific)
const parseSlotId = (slotId: string): { dayOfWeek: number | undefined; hour: number; date?: string } => {
    const parts = slotId.split('-');
    if (parts.length === 2) {
        // Template format: "D-H"
        return { dayOfWeek: parseInt(parts[0], 10), hour: parseInt(parts[1], 10) };
    } else if (parts.length === 4) {
        // Date format: "YYYY-MM-DD-H"
        const dateStr = `${parts[0]}-${parts[1]}-${parts[2]}`;
        const hour = parseInt(parts[3], 10);
        const date = new Date(dateStr);
        return { dayOfWeek: date.getDay(), hour, date: dateStr };
    }
    // Fallback - try to get at least the hour
    const hour = parseInt(parts[parts.length - 1], 10);
    return { dayOfWeek: undefined, hour: isNaN(hour) ? 0 : hour };
};

interface UnifiedSchedulerProps {
    obsSessions?: OBSSession[];
    mode?: 'schedule' | 'overview';
    viewType?: 'single' | 'multi';
    onHourClick?: (siteId: string, day: number, hour: number) => void;
    onHourContextMenu?: (e: React.MouseEvent, siteId: string, day: number, hour: number) => void;
    onAnalyticsFocus?: (payload?: Record<string, unknown>) => void;
}

export function UnifiedScheduler({ 
    obsSessions = [], 
    mode: initialMode = 'schedule',
    viewType: initialViewType = 'single',
    onHourClick,
    onHourContextMenu,
    onAnalyticsFocus,
}: UnifiedSchedulerProps) {
    const [mode, setMode] = useState<'schedule' | 'overview'>(initialMode);
    const [viewType, setViewType] = useState<'single' | 'multi'>(initialViewType);
    const [selectedSiteId, setSelectedSiteId] = useState<string | null>(null);
    const [selectedSlot, setSelectedSlot] = useState<string | null>(null);
    const [contextMenu, setContextMenu] = useState<{ visible: boolean; x: number; y: number; slotId: string | null; siteId?: string; day?: number; hour?: number }>({ 
        visible: false, x: 0, y: 0, slotId: null 
    });
    const [dayContextMenu, setDayContextMenu] = useState<{ visible: boolean; x: number; y: number; dayInfo: any | null }>({
        visible: false, x: 0, y: 0, dayInfo: null
    });
    const [dragOverSlot, setDragOverSlot] = useState<string | null>(null);
    const [editingSlot, setEditingSlot] = useState<HourSlot | null>(null);
    const [mergedSlots, setMergedSlots] = useState<Set<string>>(new Set());
    const [showRemoteMenu, setShowRemoteMenu] = useState(false);
    const containerRef = useRef<HTMLDivElement>(null);
    
    // Mock remote instances - in production, load from settings
    const remoteInstances = [
        { id: 'remote-1', name: 'Studio A', address: '192.168.1.101', port: 8080, isConnected: true },
        { id: 'remote-2', name: 'Studio B', address: '192.168.1.102', port: 8080, isConnected: false },
        { id: 'remote-3', name: 'Master Control', address: '192.168.1.100', port: 8080, isConnected: true },
    ];

    const {
        schedule,
        setSchedule,
        loading,
        calculateHourData,
        getFillPercentage,
        getFillColor,
        getSlot,
        updateSlot,
        sitesToDisplay,
        DAYS,
        displayDays,
        HOURS,
        SITE_LOCATIONS,
        nextWeek,
        prevWeek,
        goToToday,
        viewDate,
        carryOverWeek,
        trafficMaxAdSlotsPerHour,
    } = useScheduleGrid({ mode, viewType, selectedSiteId });
    const showHourWatchSignature = useMemo(() => {
        const rows: string[] = [];
        for (const s of Object.values(schedule.slots || {})) {
            const p = s.showHour?.watchFolderPath?.trim();
            if (s.showHour?.enabled && p) {
                rows.push(`${s.id}|${p}`);
            }
        }
        rows.sort();
        return rows.join(';');
    }, [schedule]);

    const [isGeneratingSchedule, setIsGeneratingSchedule] = useState(false);
    /** One-level undo for the last Shake ads apply (in-memory; tab session). */
    const [lastShakeUndo, setLastShakeUndo] = useState<{ slotId: string; items: PlaylistItem[] } | null>(null);
    /** One-level undo for last show-hour ingest (watch or refresh). */
    const [lastShowHourUndo, setLastShowHourUndo] = useState<{ slotId: string; items: PlaylistItem[] } | null>(null);
    const showHourWatchRegKeysRef = useRef<Set<string>>(new Set());

    // Close context menu on click outside
    useEffect(() => {
        const handleClick = () => {
            setContextMenu(prev => ({ ...prev, visible: false }));
            setDayContextMenu(prev => ({ ...prev, visible: false }));
        };
        document.addEventListener('click', handleClick);
        return () => document.removeEventListener('click', handleClick);
    }, []);

    const handleCellClick = useCallback((slotId: string, siteId?: string, day?: number, hour?: number) => {
        setSelectedSlot(slotId);
        if (onHourClick && siteId !== undefined && day !== undefined && hour !== undefined) {
            onHourClick(siteId, day, hour);
        }
    }, [onHourClick]);

    const handleCellDoubleClick = useCallback((slotId: string) => {
        if (mode === 'overview') return; // No editing in overview mode
        
        let slot = getSlot(slotId);
        if (!slot) {
            // Create slot if it doesn't exist
            const parsed = parseSlotId(slotId);
            slot = {
                id: slotId,
                dayOfWeek: parsed.dayOfWeek,
                hour: parsed.hour,
                date: parsed.date,
                items: [],
                isLooping: false,
                randomizeAds: true,
                keepProgramOrder: true,
            };
            updateSlot(slotId, slot);
        }
        setEditingSlot(slot);
    }, [mode, getSlot, updateSlot]);

    const handleContextMenu = useCallback((e: React.MouseEvent, slotId: string, siteId?: string, day?: number, hour?: number) => {
        e.preventDefault();
        setContextMenu({
            visible: true,
            x: e.clientX,
            y: e.clientY,
            slotId,
            siteId,
            day,
            hour,
        });
        setSelectedSlot(slotId);
        if (onHourContextMenu && siteId !== undefined && day !== undefined && hour !== undefined) {
            onHourContextMenu(e, siteId, day, hour);
        }
    }, [onHourContextMenu]);

    const handleCopyHour = useCallback(() => {
        if (!contextMenu.slotId) return;
        const slot = getSlot(contextMenu.slotId);
        if (slot) copyHourToClipboard(slot);
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, getSlot]);

    const handlePasteHour = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const newSchedule = pasteHourFromClipboard(contextMenu.slotId, schedule);
        setSchedule(newSchedule);
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, setSchedule]);

    const handleDayContextMenu = useCallback((e: React.MouseEvent, dayInfo: any) => {
        e.preventDefault();
        setDayContextMenu({
            visible: true,
            x: e.clientX,
            y: e.clientY,
            dayInfo,
        });
    }, []);

    const handleCopyDay = useCallback(() => {
        if (!dayContextMenu.dayInfo || !schedule) return;
        const dayInfo = dayContextMenu.dayInfo;
        const daySlots: Record<number, HourSlot> = {};
        for (let h = 0; h < 24; h++) {
            const slotId = `${dayInfo.isoDate}-${h}`;
            const templateId = `${dayInfo.dayOfWeek}-${h}`;
            const slot = schedule.slots[slotId] || schedule.slots[templateId];
            if (slot) daySlots[h] = JSON.parse(JSON.stringify(slot));
        }
        localStorage.setItem('copiedDaySlots', JSON.stringify(daySlots));
        setDayContextMenu(prev => ({ ...prev, visible: false }));
    }, [dayContextMenu, schedule]);

    const handlePasteDayExact = useCallback(() => {
        if (!dayContextMenu.dayInfo || !schedule) return;
        const copiedRaw = localStorage.getItem('copiedDaySlots');
        if (!copiedRaw) return;
        const copiedSlots: Record<number, HourSlot> = JSON.parse(copiedRaw);
        
        const dayInfo = dayContextMenu.dayInfo;
        const newSchedule = { ...schedule, slots: { ...schedule.slots } };
        
        for (let h = 0; h < 24; h++) {
            const slotToCopy = copiedSlots[h];
            if (slotToCopy) {
                const targetSlotId = `${dayInfo.isoDate}-${h}`;
                newSchedule.slots[targetSlotId] = {
                    ...slotToCopy,
                    id: targetSlotId,
                    date: dayInfo.isoDate,
                    dayOfWeek: dayInfo.dayOfWeek,
                    hour: h
                };
            }
        }
        setSchedule(newSchedule);
        setDayContextMenu(prev => ({ ...prev, visible: false }));
    }, [dayContextMenu, schedule, setSchedule]);

    const handlePasteDayShiftAds = useCallback(() => {
        if (!dayContextMenu.dayInfo || !schedule) return;
        const copiedRaw = localStorage.getItem('copiedDaySlots');
        if (!copiedRaw) return;
        const copiedSlots: Record<number, HourSlot> = JSON.parse(copiedRaw);
        
        const dayInfo = dayContextMenu.dayInfo;
        const newSchedule = { ...schedule, slots: { ...schedule.slots } };
        
        for (let h = 0; h < 24; h++) {
            const slotToCopy = copiedSlots[h];
            if (slotToCopy) {
                const targetSlotId = `${dayInfo.isoDate}-${h}`;
                const existingTargetSlot = newSchedule.slots[targetSlotId];
                // Shift ads logic: keep target ads, copy non-ad content from source
                const newItems = slotToCopy.items.filter(item => item.media?.type !== 'ad');
                const targetAds = existingTargetSlot?.items.filter(item => item.media?.type === 'ad') || [];
                
                newSchedule.slots[targetSlotId] = {
                    ...(existingTargetSlot || slotToCopy),
                    id: targetSlotId,
                    date: dayInfo.isoDate,
                    dayOfWeek: dayInfo.dayOfWeek,
                    hour: h,
                    items: [...newItems, ...targetAds],
                };
            }
        }
        setSchedule(newSchedule);
        setDayContextMenu(prev => ({ ...prev, visible: false }));
    }, [dayContextMenu, schedule, setSchedule]);

    const handleQuickCopyNextHour = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const parsed = parseSlotId(contextMenu.slotId);
        const nextHour = (parsed.hour + 1) % 24;
        const nextDay = parsed.dayOfWeek !== undefined ? (nextHour === 0 ? (parsed.dayOfWeek + 1) % 7 : parsed.dayOfWeek) : undefined;
        const nextSlotId = nextDay !== undefined ? `${nextDay}-${nextHour}` : `0-${nextHour}`;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(nextSlotId, { ...slot, id: nextSlotId, dayOfWeek: nextDay, hour: nextHour });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleQuickCopyNextDay = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const parsed = parseSlotId(contextMenu.slotId);
        const nextDay = parsed.dayOfWeek !== undefined ? (parsed.dayOfWeek + 1) % 7 : 0;
        const nextSlotId = `${nextDay}-${parsed.hour}`;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(nextSlotId, { ...slot, id: nextSlotId, dayOfWeek: nextDay, hour: parsed.hour });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleMergeWithNextHours = useCallback(async () => {
        if (!contextMenu.slotId) return;
        const parsed = parseSlotId(contextMenu.slotId);
        const countStr = await safePrompt('Merge how many hours (including this one)?', '2');
        const count = Math.max(1, parseInt(countStr || '2', 10) || 2);

        const slotIds: string[] = [];
        let d = parsed.dayOfWeek ?? 0;
        let h = parsed.hour;
        for (let i = 0; i < count; i++) {
            slotIds.push(`${d}-${h}`);
            h++;
            if (h >= 24) {
                h = 0;
                d = (d + 1) % 7;
            }
        }

        const event = new CustomEvent('merge-hours', { detail: { slotIds } });
        window.dispatchEvent(event);
        setMergedSlots(prev => {
            const next = new Set(prev);
            slotIds.forEach(id => next.add(id));
            return next;
        });
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId]);

    const handleToggleLoop = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(contextMenu.slotId, { ...slot, isLooping: !slot.isLooping });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleToggleRandomize = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(contextMenu.slotId, { ...slot, randomizeAds: !slot.randomizeAds });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleToggleKeepPrograms = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(contextMenu.slotId, { ...slot, keepProgramOrder: !slot.keepProgramOrder });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleExportToFreePlaylist = useCallback(async () => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        if (!slot || slot.items.length === 0) return;

        let dayLabel = '';
        if (typeof slot.dayOfWeek === 'number') {
            dayLabel = DAYS[slot.dayOfWeek] || '';
        } else {
            dayLabel = slot.date || '';
        }
        
        const name = `${dayLabel} ${slot.hour.toString().padStart(2, '0')}:00 Export`;

        const playlist = createFreePlaylist(name, contextMenu.slotId);
        playlist.items = slot.items.map((item: any) => ({ ...item, id: `fp_${Date.now()}_${Math.random().toString(36).substr(2, 5)}` }));

        const playlists = await loadFreePlaylists();
        playlists.push(playlist);
        await saveFreePlaylists(playlists);

        setContextMenu(prev => ({ ...prev, visible: false }));
        alert(`Exported to Free Playlist: "${name}"`);
    }, [contextMenu.slotId, schedule, getSlot, DAYS]);

    const handleClearHour = useCallback(() => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        if (slot) {
            updateSlot(contextMenu.slotId, { ...slot, items: [] });
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, updateSlot]);

    const handleSendToRemote = useCallback((remoteId: string) => {
        if (!contextMenu.slotId || !schedule) return;
        const slot = getSlot(contextMenu.slotId);
        const remote = remoteInstances.find(r => r.id === remoteId);
        
        if (slot && remote) {
            // In production, this would send the slot data to the remote instance
            console.log(`[UnifiedScheduler] Sending slot ${slot.id} to remote ${remote.name} (${remote.address}:${remote.port})`);
            console.log('[UnifiedScheduler] Slot data:', JSON.stringify(slot, null, 2));
            
            // Show feedback
            alert(`Slot sent to ${remote.name}!\n\nIn production, this would sync the slot data to the remote instance at ${remote.address}:${remote.port}`);
        }
        
        setShowRemoteMenu(false);
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule, getSlot, remoteInstances]);

    const loadAllMediaForRegionalOps = useCallback(async (): Promise<MediaItem[]> => {
        const [library, local, db] = await Promise.all([
            loadLibraryMedia(),
            Promise.resolve(loadLocalMedia()),
            dbFetchMedia().catch(() => []),
        ]);
        return Array.from(new Map([...library, ...local, ...db].map(item => [item.id, item])).values());
    }, []);

    const applySiteShift = useCallback(async (
        siteId: string,
        day: number,
        hour: number,
        mode: 'copy' | 'move',
        shiftHours = 0,
        itemTypes: Array<MediaItem['type'] | 'all'> = ['all'],
    ) => {
        if (!schedule) return;
        const mediaLibrary = await loadAllMediaForRegionalOps();
        const next = shiftScheduleBlock(schedule, {
            sourceSiteId: siteId,
            targetSiteId: siteId,
            sourceDay: day,
            sourceHour: hour,
            shiftHours,
            mode,
            itemTypes,
            regionalizeAds: true,
            mediaLibrary,
        });
        setSchedule(next);
        persistentLogger.info('Multi-site schedule shift', { description: describeScheduleShift({
            sourceSiteId: siteId,
            targetSiteId: siteId,
            sourceDay: day,
            sourceHour: hour,
            shiftHours,
            mode,
            itemTypes,
        }) }, 'UnifiedScheduler');
    }, [schedule, setSchedule, loadAllMediaForRegionalOps]);

    const copySiteHourToOtherSites = useCallback(async (siteId: string, day: number, hour: number) => {
        if (!schedule) return;
        const mediaLibrary = await loadAllMediaForRegionalOps();
        let next = schedule;
        for (const target of PLAYOUT_SITE_PROFILES.filter(site => site.enabled && site.id !== siteId)) {
            next = shiftScheduleBlock(next, {
                sourceSiteId: siteId,
                targetSiteId: target.id,
                sourceDay: day,
                sourceHour: hour,
                targetDay: day,
                targetHour: hour,
                mode: 'copy',
                itemTypes: ['all'],
                regionalizeAds: true,
                mediaLibrary,
            });
        }
        setSchedule(next);
    }, [schedule, setSchedule, loadAllMediaForRegionalOps]);


    const mergePathsIntoShowHourSlot = useCallback(
        async (slotId: string, paths: string[]) => {
            if (!paths.length) return;
            const slotNow = getSlot(slotId);
            if (!slotNow?.showHour?.enabled) return;
            const filtered = paths
                .filter((p) => !slotHasFilePath(slotNow, p))
                .slice(0, SHOW_HOUR_MAX_WATCH_FILES);
            if (!filtered.length) return;
            const wouldDemote = slotNow.items.some((it) => isDemotableProgramLikeRow(it));
            if (
                wouldDemote &&
                !window.confirm(
                    `Add ${filtered.length} new file(s) to this show hour?\n\n` +
                        'Existing program-like rows in this hour will be set to low scheduling priority (-8).',
                )
            ) {
                return;
            }
            setLastShowHourUndo({ slotId, items: JSON.parse(JSON.stringify(slotNow.items)) as PlaylistItem[] });
            const newRows: PlaylistItem[] = [];
            for (const p of filtered) {
                try {
                    const media = await resolveOrCreateMediaFromDiskPath(p);
                    newRows.push(createPlaylistItemFromMedia(media, generateId()));
                } catch (e) {
                    persistentLogger.error('Show hour ingest failed for path', e, 'UnifiedScheduler');
                }
            }
            if (!newRows.length) return;
            setSchedule((prev) => {
                const s = prev.slots[slotId];
                if (!s?.showHour?.enabled) return prev;
                const merged = mergeShowHourAppendItems(s.items, newRows);
                return {
                    ...prev,
                    slots: {
                        ...prev.slots,
                        [slotId]: { ...s, items: merged, updatedAt: Date.now() },
                    },
                };
            });
        },
        [getSlot, setSchedule],
    );

    const pickWatchFolderPath = useCallback(async (): Promise<string | null> => {
        const api = (typeof window !== 'undefined' ? (window as unknown as { electronAPI?: any }).electronAPI : null) as any;
        if (!api?.openFolderDialog) {
            alert('Folder picker requires the Electron app.');
            return null;
        }
        try {
            const raw = await api.openFolderDialog();
            if (Array.isArray(raw)) return raw[0] || null;
            if (raw && !raw.canceled && raw.filePaths?.[0]) return raw.filePaths[0];
        } catch (e) {
            persistentLogger.error('openFolderDialog failed', e, 'UnifiedScheduler');
        }
        return null;
    }, []);


    const handleSaveSlot = useCallback((updatedSlot: HourSlot) => {
        updateSlot(updatedSlot.id, updatedSlot);
        setEditingSlot(null);
        
        // Dispatch event for other components
        setTimeout(() => {
            window.dispatchEvent(new CustomEvent('schedule-updated', { 
                detail: { slotId: updatedSlot.id } 
            }));
        }, 100);
    }, [updateSlot]);


    useEffect(() => {
        const api = (typeof window !== 'undefined' ? (window as unknown as { electronAPI?: any }).electronAPI : null) as any;
        if (!api?.showHourWatchRegister || !api?.showHourWatchUnregister) return undefined;
        const ch = getActiveChannelId();
        for (const key of showHourWatchRegKeysRef.current) {
            const slotId = key.split('::')[1] || key;
            void api.showHourWatchUnregister({ slotId, channelId: ch });
        }
        showHourWatchRegKeysRef.current.clear();
        for (const s of Object.values(schedule.slots || {})) {
            const p = s.showHour?.watchFolderPath?.trim();
            if (s.showHour?.enabled && p) {
                const key = `${ch}::${s.id}`;
                void api.showHourWatchRegister({ slotId: s.id, folderPath: p, channelId: ch });
                showHourWatchRegKeysRef.current.add(key);
            }
        }
        return () => {
            for (const key of showHourWatchRegKeysRef.current) {
                const slotId = key.split('::')[1] || key;
                void api.showHourWatchUnregister({ slotId, channelId: ch });
            }
            showHourWatchRegKeysRef.current.clear();
        };
    }, [showHourWatchSignature]);

    useEffect(() => {
        const api = (typeof window !== 'undefined' ? (window as unknown as { electronAPI?: any }).electronAPI : null) as any;
        const off = api?.onShowHourWatchFiles?.(
            (payload: { channelId: string; slotId: string; paths: string[] }) => {
                if (!payload?.paths?.length) return;
                if (payload.channelId !== getActiveChannelId()) return;
                void mergePathsIntoShowHourSlot(payload.slotId, payload.paths);
            },
        );
        return () => {
            off?.();
        };
    }, [mergePathsIntoShowHourSlot]);


    const handleAISchedule = useCallback(async () => {
        if (!schedule) return;
        setIsGeneratingSchedule(true);
        try {
            const library = await loadLibraryMedia();
            const result = await aiScheduler.generateSchedule(library, {
                targetDurationMinutes: 60,
                commercialBreaksPerHour: 4,
                commercialBreakDurationMinutes: 3,
                avoidConsecutiveCategories: true,
                prioritizeHighPlayCount: false
            });

            if (result.success && result.schedule) {
                // Determine target slot
                const targetDay = displayDays[0].isoDate;
                const targetSlotId = `${targetDay}-20`; // 8 PM default for now
                
                const existingSlot = getSlot(targetSlotId);
                const parsed = parseSlotId(targetSlotId);
                
                const updatedSlot: HourSlot = existingSlot ? {
                    ...existingSlot,
                    items: [...existingSlot.items, ...result.schedule],
                } : {
                    id: targetSlotId,
                    dayOfWeek: parsed.dayOfWeek,
                    hour: parsed.hour,
                    date: parsed.date,
                    items: result.schedule,
                    isLooping: false,
                    randomizeAds: true,
                    keepProgramOrder: true,
                };
                
                updateSlot(targetSlotId, updatedSlot);
                alert('AI Schedule generated successfully and added to 20:00 (8 PM) slot.');
            } else {
                alert(`AI Scheduling failed: ${result.error}`);
            }
        } catch (e) {
            persistentLogger.error('AI scheduling failed', e, 'UnifiedScheduler');
            alert('An error occurred while generating the AI schedule.');
        } finally {
            setIsGeneratingSchedule(false);
        }
    }, [schedule, displayDays, getSlot, updateSlot]);

    // Drag and drop handlers
    const handleDragOver = useCallback((e: React.DragEvent, slotId: string) => {
        if (mode === 'overview') return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
        setDragOverSlot(slotId);
    }, [mode]);

    const handleDragLeave = useCallback(() => {
        setDragOverSlot(null);
    }, []);

    const handleDrop = useCallback((e: React.DragEvent, slotId: string) => {
        console.log('[UnifiedScheduler] handleDrop called:', slotId, 'mode:', mode, 'schedule:', !!schedule);
        
        if (mode === 'overview' || !schedule) return;
        e.preventDefault();
        setDragOverSlot(null);

        try {
            const data = e.dataTransfer.getData('application/json');
            console.log('[UnifiedScheduler] Drop data:', data);
            if (!data) return;

            const media: MediaItem = JSON.parse(data);
            console.log('[UnifiedScheduler] Parsed media:', media.id, media.title);
            
            const newItem: PlaylistItem = {
                id: generateId(),
                mediaId: media.id,
                media: { ...media, priority: typeof media.priority === 'number' ? media.priority : 0 },
                isResolved: true,
            };

            const existingSlot = getSlot(slotId);
            const parsed = parseSlotId(slotId);
            const useShowHour = Boolean(existingSlot?.showHour?.enabled);
            if (useShowHour && existingSlot) {
                const wouldDemote = existingSlot.items.some((it) => isDemotableProgramLikeRow(it));
                if (
                    wouldDemote &&
                    !window.confirm(
                        'Add this item to the show hour? Existing program-like rows will be set to low scheduling priority (-8).',
                    )
                ) {
                    return;
                }
                if (wouldDemote) {
                    setLastShowHourUndo({
                        slotId,
                        items: JSON.parse(JSON.stringify(existingSlot.items)) as PlaylistItem[],
                    });
                }
                const mergedItems = mergeShowHourAppendItems(existingSlot.items, [newItem]);
                updateSlot(slotId, {
                    ...existingSlot,
                    items: mergedItems,
                    updatedAt: Date.now(),
                });
                return;
            }

            const updatedSlot: HourSlot = existingSlot ? {
                ...existingSlot,
                items: [...existingSlot.items, newItem],
            } : {
                id: slotId,
                dayOfWeek: parsed.dayOfWeek,
                hour: parsed.hour,
                date: parsed.date,
                items: [newItem],
                isLooping: false,
                randomizeAds: true,
                keepProgramOrder: true,
            };

            console.log('[UnifiedScheduler] Calling updateSlot with:', slotId, 'items:', updatedSlot.items.length);
            updateSlot(slotId, updatedSlot);
            console.log('[UnifiedScheduler] updateSlot completed');
        } catch (err) {
            console.error('Failed to parse dropped media:', err);
        }
    }, [mode, schedule, getSlot, updateSlot]);

    // Get slot info for display
    const getSlotInfo = useCallback((slot: HourSlot | null) => {
        if (!slot) {
            return {
                fill: 0,
                rawFill: 0,
                count: 0,
                duration: 0,
                adCount: 0,
                adDurationSec: 0,
                adCoveragePct: null as number | null,
                primaryTitles: [] as string[],
            };
        }
        const duration = slot.items.reduce((sum, item) => sum + (item.media?.duration || 0), 0);
        const rawFill = (duration / 3600) * 100;
        const adItems = slot.items.filter(item => item.media?.type === 'ad');
        const adCount = adItems.length;
        const adDurationSec = adItems.reduce((sum, item) => sum + (item.media?.duration || 0), 0);
        const cap = trafficMaxAdSlotsPerHour;
        const adCoveragePct = cap > 0 ? Math.min(100, Math.round((100 * adCount) / cap)) : null;
        const programs = slot.items.filter(
            item => item.media && item.media.type !== 'ad' && item.media.type !== 'gap',
        );
        const primaryTitles = programs.slice(0, 2).map(item => {
            const t = item.media?.title?.trim() || 'Untitled';
            return t.length > 44 ? `${t.slice(0, 41)}…` : t;
        });
        return {
            fill: Math.min(rawFill, 100),
            rawFill,
            count: slot.items.length,
            duration,
            adCount,
            adDurationSec,
            adCoveragePct,
            primaryTitles,
        };
    }, [trafficMaxAdSlotsPerHour]);

    const handleUndoShakeHourAds = useCallback(() => {
        if (!lastShakeUndo || !schedule) return;
        const slot = getSlot(lastShakeUndo.slotId);
        if (!slot) {
            setLastShakeUndo(null);
            return;
        }
        updateSlot(lastShakeUndo.slotId, { ...slot, items: JSON.parse(JSON.stringify(lastShakeUndo.items)) });
        setLastShakeUndo(null);
    }, [lastShakeUndo, schedule, getSlot, updateSlot]);

    const handleShakeHourAdsForSlot = useCallback(
        async (slotId: string) => {
            if (!schedule || mode !== 'schedule') return;
            const slot = getSlot(slotId);
            if (!slot) {
                alert('No schedule data for this hour yet. Double-click to create a slot, or add items.');
                return;
            }
            const adRows = slot.items.filter(i => i.media?.type === 'ad');
            if (adRows.length === 0) {
                alert('This hour has no ad rows to shake.');
                return;
            }
            const ok = window.confirm(
                `Replace ${adRows.length} ad spot(s) with new picks from the combined library (DB + local + saved library)?\n\n` +
                    'Probability model: P(ad_i) ∝ max(ε, priority+10) × (adMetadata.weight if set and positive, else 1), normalized over the pool on each draw (independent draws; same pool).\n\n' +
                    'One undo is available from the hour menu or the header until you shake again.',
            );
            if (!ok) return;

            const pool = await fetchEligibleAdPool();
            if (pool.length === 0) {
                alert('No eligible ads found (type ad, not archived, duration > 0).');
                return;
            }

            const built = buildShakenSlotItems(slot, pool);
            if (!built) {
                alert('Could not build a new ad assignment.');
                return;
            }

            setLastShakeUndo({ slotId, items: JSON.parse(JSON.stringify(slot.items)) as PlaylistItem[] });
            updateSlot(slotId, { ...slot, items: built.items });
        },
        [schedule, mode, getSlot, updateSlot],
    );

    // Color for slot age (green → red → pulsing red spectrum)
    const slotAgeColor = useCallback((updatedAt: number | undefined): string => {
        if (!updatedAt) return '';
        const ageDays = (Date.now() - updatedAt) / 86_400_000;
        if (ageDays < 5) return '#10b981';   // bright green
        return '#ef4444';                     // red (>= 5 days)
    }, []);

    // Render schedule mode (editing)
    const renderScheduleMode = () => {
        if (!schedule) return <div>Loading...</div>;

        return (
            <div className={styles.grid}>
                <div className={styles.cornerCell}></div>
                {displayDays.map((dayInfo, idx) => (
                    <div 
                        key={idx} 
                        className={styles.dayHeader}
                        onContextMenu={(e) => handleDayContextMenu(e, dayInfo)}
                        style={{ cursor: 'context-menu' }}
                        title="Right-click to copy/paste day schedule"
                    >
                        {dayInfo.label}
                    </div>
                ))}

                {HOURS.map(hour => (
                    <React.Fragment key={hour}>
                        <div className={styles.hourLabel}>
                            {hour.toString().padStart(2, '0')}:00
                        </div>
                        {displayDays.map((dayInfo, dayIndex) => {
                            const dateSlotId = `${dayInfo.isoDate}-${hour}`;
                            const templateSlotId = `${dayInfo.dayOfWeek}-${hour}`;
                            const slot = schedule.slots[dateSlotId] || schedule.slots[templateSlotId];
                            const slotId = slot ? slot.id : dateSlotId;

                            const info = getSlotInfo(slot);
                            const isSelected = selectedSlot === slotId;
                            const isDragOver = dragOverSlot === slotId;
                            const isMerged = mergedSlots.has(slotId);
                            const isTemplate = !schedule.slots[dateSlotId] && !!schedule.slots[templateSlotId];
                            const isOverfull = info.rawFill > 100;

                            // Age of last content edit
                            const updatedAt = slot?.updatedAt;
                            const ageDays = updatedAt ? (Date.now() - updatedAt) / 86_400_000 : null;
                            const ageColor = updatedAt ? slotAgeColor(updatedAt) : '';
                            const isStale = ageDays !== null && ageDays >= 7;

                            return (
                                <div
                                    key={`${dayIndex}-${hour}`}
                                    className={`${styles.cell} ${isSelected ? styles.cellSelected : ''} ${slot?.isLooping ? styles.cellLooping : ''} ${isDragOver ? styles.cellDragOver : ''} ${isMerged ? styles.cellMerged : ''} ${isTemplate ? styles.cellTemplate : ''} ${isOverfull ? styles.cellOverfull : ''} ${slot?.showHour?.enabled ? styles.cellShowHour : ''}`}
                                    onClick={() => handleCellClick(slotId)}
                                    onDoubleClick={() => handleCellDoubleClick(slotId)}
                                    onContextMenu={(e) => handleContextMenu(e, slotId)}
                                    onDragOver={(e) => handleDragOver(e, slotId)}
                                    onDragLeave={handleDragLeave}
                                    onDrop={(e) => handleDrop(e, slotId)}
                                    title={`${dayInfo.label} ${hour.toString().padStart(2, '0')}:00\n${isTemplate ? '(Template) ' : ''}${info.count} items, ${formatDuration(info.duration)}\n${isOverfull ? `OVERFULL: ${Math.round(info.rawFill)}%` : ''}\nDouble-click to edit`}
                                    data-hour={`${hour.toString().padStart(2, '0')}:00`}
                                >
                                    <div
                                        className={`${styles.fillBar} ${isOverfull ? styles.fillBarOverfull : ''}`}
                                        style={{
                                            width: `${Math.min(info.rawFill, 100)}%`,
                                            backgroundColor: isOverfull ? 'var(--danger)' :
                                                info.fill > 80 ? 'var(--success)' :
                                                    info.fill > 0 ? 'var(--warning)' : 'transparent'
                                        }}
                                    />
                                    <span className={styles.cellFillPct}>{Math.round(info.rawFill)}%</span>
                                    {info.count > 0 && (
                                        <span className={styles.itemCount}>{info.count}</span>
                                    )}
                                    {info.primaryTitles.length > 0 && (
                                        <div className={styles.cellProgramStack}>
                                            {info.primaryTitles.map((t, ti) => (
                                                <div key={ti} className={styles.cellProgramTitle}>
                                                    {t}
                                                </div>
                                            ))}
                                        </div>
                                    )}
                                    {info.adCount > 0 && (
                                        <div className={styles.cellAdMeta}>
                                            Ads {info.adCoveragePct !== null && trafficMaxAdSlotsPerHour > 0
                                                ? `${info.adCount}/${trafficMaxAdSlotsPerHour} (${info.adCoveragePct}%)`
                                                : `${info.adCount} · ${formatDuration(info.adDurationSec)}`}
                                        </div>
                                    )}
                                    {slot?.isLooping && <Repeat size={10} className={styles.loopIcon} />}
                                    {slot?.randomizeAds && info.count > 0 && <Shuffle size={10} className={styles.shuffleIcon} />}
                                    {updatedAt && info.count > 0 && (
                                        <span
                                            className={`${styles.ageBadge} ${isStale ? styles.ageBadgeStale : ''}`}
                                            style={{ color: ageColor }}
                                            title={`Last updated: ${new Date(updatedAt).toLocaleDateString()}`}
                                        >
                                            {ageDays !== null && ageDays < 1 ? 'today' : ageDays !== null ? `${Math.floor(ageDays)}d` : ''}
                                        </span>
                                    )}
                                </div>
                            );
                        })}
                    </React.Fragment>
                ))}
            </div>
        );
    };

    // Render overview mode (multi-site status)
    const renderOverviewMode = () => {
        if (viewType === 'single') {
            // Single site view - similar to schedule but read-only
            return renderScheduleMode();
        }

        // Multi-site matrix view
        return (
            <div className={styles.matrixContainer}>
                <div className={styles.gridHeader}>
                    <div className={styles.cornerCell}></div>
                    {SITE_LOCATIONS.map(site => (
                        <div key={site.id} className={styles.siteHeader}>
                            {site.name}
                        </div>
                    ))}
                </div>
                <div className={styles.gridBody}>
                    {Array.from({ length: 7 }, (_, day) => (
                        <React.Fragment key={day}>
                            {Array.from({ length: 24 }, (_, hour) => (
                                <React.Fragment key={hour}>
                                    {hour === 0 && (
                                        <div className={styles.dayLabel}>{DAYS[day]}</div>
                                    )}
                                    {SITE_LOCATIONS.map(site => {
                                        const cell = calculateHourData(site.id, day, hour);
                                        const fillPct = getFillPercentage(cell);
                                        const fillColor = getFillColor(fillPct);
                                        const now = new Date();
                                        const isCurrentHour = now.getDay() === day && now.getHours() === hour;
                                        
                                        return (
                                            <div
                                                key={`${site.id}-${day}-${hour}`}
                                                className={`${styles.hourCell} ${cell.isEmpty ? styles.empty : ''} ${isCurrentHour ? styles.currentHour : ''}`}
                                                style={{
                                                    background: `linear-gradient(to top, ${fillColor} ${fillPct}%, transparent ${fillPct}%)`,
                                                }}
                                                onDoubleClick={() => handleCellDoubleClick(`${day}-${hour}`)}
                                                onContextMenu={(e) => handleContextMenu(e, `${day}-${hour}`, site.id, day, hour)}
                                                title={`${site.name} - ${DAYS[day]} ${hour}:00 - ${cell.itemCount} items, ${Math.round(fillPct)}% filled`}
                                            >
                                                <div className={styles.cellContent}>
                                                    <div className={styles.cellTime}>{hour}:00</div>
                                                    <div className={styles.cellFillPct}>{Math.round(fillPct)}% full</div>
                                                    {!cell.isEmpty && (
                                                        <>
                                                            <div className={styles.cellCount}>{cell.itemCount}</div>
                                                            {cell.adCount > 0 && (
                                                                <div className={styles.cellAds}>
                                                                    {cell.targetAdSlotsPerHour > 0
                                                                        ? `Ads ${cell.adCount}/${cell.targetAdSlotsPerHour}${cell.adSlotCoveragePct !== null ? ` (${cell.adSlotCoveragePct}%)` : ''}`
                                                                        : `${cell.adCount} ads · ${formatDuration(cell.adDurationSec)}`}
                                                                </div>
                                                            )}
                                                        </>
                                                    )}
                                                </div>
                                            </div>
                                        );
                                    })}
                                </React.Fragment>
                            ))}
                        </React.Fragment>
                    ))}
                </div>
            </div>
        );
    };

    const currentSlot = contextMenu.slotId ? getSlot(contextMenu.slotId) : null;
    const hasClipboard = getClipboardSlot() !== null;

    const handleScheduleMenuAction = async (actionId: string, payload?: Record<string, unknown>) => {
        const menuSlotId = String((payload?.slotId as string) || contextMenu.slotId || '');
        if (actionId === 'shake_hour_ads') {
            await handleShakeHourAdsForSlot(menuSlotId);
            return;
        }
        if (actionId === 'undo_shake_hour_ads') {
            handleUndoShakeHourAds();
            return;
        }

        if (actionId === 'toggle_show_hour') {
            const sid = menuSlotId;
            let slot = getSlot(sid);
            const parsed = parseSlotId(sid);
            if (!slot) {
                slot = {
                    id: sid,
                    dayOfWeek: parsed.dayOfWeek,
                    hour: parsed.hour,
                    date: parsed.date,
                    items: [],
                    isLooping: false,
                    randomizeAds: true,
                    keepProgramOrder: true,
                };
            }
            const nextOn = !slot.showHour?.enabled;
            updateSlot(sid, {
                ...slot,
                showHour: nextOn
                    ? { enabled: true, watchFolderPath: slot.showHour?.watchFolderPath }
                    : { enabled: false },
            });
            return;
        }
        if (actionId === 'show_hour_watch_folder') {
            const sid = menuSlotId;
            let slot = getSlot(sid);
            const parsed = parseSlotId(sid);
            if (!slot) {
                slot = {
                    id: sid,
                    dayOfWeek: parsed.dayOfWeek,
                    hour: parsed.hour,
                    date: parsed.date,
                    items: [],
                    isLooping: false,
                    randomizeAds: true,
                    keepProgramOrder: true,
                };
            }
            void (async () => {
                const dir = await pickWatchFolderPath();
                if (!dir) return;
                const latest = getSlot(sid) || slot;
                updateSlot(sid, {
                    ...latest,
                    showHour: { enabled: true, watchFolderPath: dir },
                });
            })();
            return;
        }
        if (actionId === 'show_hour_clear_watch_folder') {
            const sid = menuSlotId;
            const slot = getSlot(sid);
            if (!slot) return;
            updateSlot(sid, {
                ...slot,
                showHour: { enabled: true, watchFolderPath: undefined },
            });
            return;
        }
        if (actionId === 'show_hour_refresh_watch') {
            const sid = menuSlotId;
            const slot = getSlot(sid);
            const folder = slot?.showHour?.watchFolderPath?.trim();
            if (!folder) return;
            void (async () => {
                const api = (window as unknown as { electronAPI?: any }).electronAPI;
                if (!api?.showHourFolderList) {
                    alert('Refresh requires the Electron app with show-hour IPC support.');
                    return;
                }
                const res = await api.showHourFolderList(folder);
                const paths = (res && res.paths) || [];
                const slotFresh = getSlot(sid);
                const novel = paths
                    .filter((p: string) => !slotHasFilePath(slotFresh, p))
                    .slice(0, SHOW_HOUR_MAX_WATCH_FILES);
                if (!novel.length) {
                    alert('No new media files found at the top level of the watch folder.');
                    return;
                }
                await mergePathsIntoShowHourSlot(sid, novel);
            })();
            return;
        }

        await executeContextMenuAction(actionId as import('../lib/types').ContextMenuActionId, payload, {
            onEditHour: async (nextPayload) => {
                const slotId = String(nextPayload?.slotId || contextMenu.slotId || '');
                if (slotId) handleCellDoubleClick(slotId);
            },
            onCopyHour: async () => {
                handleCopyHour();
            },
            onPasteHour: async () => {
                handlePasteHour();
            },
            onClearHour: async () => {
                handleClearHour();
            },
            onViewAnalytics: async (nextPayload) => {
                onAnalyticsFocus?.({
                    targetType: 'hour',
                    targetId: nextPayload?.slotId || contextMenu.slotId || '',
                    slotId: nextPayload?.slotId || contextMenu.slotId || '',
                });
            },
            // Custom actions for Day menu
            onCopyDay: async (nextPayload?: Record<string, unknown>) => {
                if (!schedule || !nextPayload?.dayInfo) return;
                const info = nextPayload.dayInfo as { dayOfWeek?: number; isoDate?: string; label?: string };
                if (typeof info.dayOfWeek !== 'number' || !info.isoDate) return;
                copyDayToClipboard(info.dayOfWeek, schedule, info.isoDate);
            },
            onPasteDay: async (nextPayload?: Record<string, unknown>) => {
                if (!schedule || !nextPayload?.dayInfo || !getClipboardDay()) return;
                const info = nextPayload.dayInfo as { dayOfWeek?: number; isoDate?: string; label?: string };
                if (!info.isoDate) return;

                const confirmed = await safePrompt('Paste Day Option:', 'exact');
                // Options: "exact" or "shiftAds"
                const options = confirmed === 'shiftAds' ? { shiftAds: true } : { shiftAds: false };
                
                const next = pasteDayFromClipboard(info.isoDate, schedule, options);
                setSchedule(next);
            },
            onClearDay: async (nextPayload?: Record<string, unknown>) => {
                if (!schedule || !nextPayload?.dayInfo) return;
                const info = nextPayload.dayInfo as { dayOfWeek?: number; isoDate?: string; label?: string };
                if (!info.isoDate) return;
                if (!confirm(`Are you sure you want to clear all slots for ${info.label}?`)) return;
                
                let next = { ...schedule, slots: { ...schedule.slots } };
                for (let h = 0; h < 24; h++) {
                    const slotId = `${info.isoDate}-${h}`;
                    delete next.slots[slotId];
                }
                setSchedule(next);
            }
        });
    };

    let slotDayLabel = '';
    if (currentSlot && typeof currentSlot.dayOfWeek === 'number') {
        slotDayLabel = DAYS[currentSlot.dayOfWeek] || '';
    }
    const slotHourLabel = currentSlot ? currentSlot.hour.toString().padStart(2, '0') : '';

    return (
        <div className={styles.container} ref={containerRef}>
            <div className={styles.header}>
                <div className={styles.title}>
                    {mode === 'schedule' ? <Calendar size={18} /> : <Grid size={18} />}
                    {mode === 'schedule' ? 'Weekly Schedule' : 'Multi-Site Overview'}
                </div>
                <div className={styles.navigation}>
                    <button onClick={prevWeek} title="Previous Week"><ChevronLeft size={16} /></button>
                    <button onClick={goToToday} className={styles.todayBtn}>Today</button>
                    <button onClick={nextWeek} title="Next Week"><ChevronRight size={16} /></button>
                    <span className={styles.dateRange}>
                        {displayDays[0].label} - {displayDays[6].label}
                    </span>
                    <button 
                        onClick={() => carryOverWeek(displayDays[0].isoDate)} 
                        className={styles.carryOverBtn}
                        title="Carry over template to this week"
                    >
                        <RefreshCw size={14} /> Carry Over
                    </button>
                    <button 
                        onClick={handleAISchedule} 
                        className={styles.carryOverBtn}
                        title="Generate AI Schedule"
                        disabled={isGeneratingSchedule}
                        style={{ marginLeft: '8px', backgroundColor: 'var(--brand-primary)' }}
                    >
                        <Sparkles size={14} /> {isGeneratingSchedule ? 'Generating...' : 'AI Schedule'}
                    </button>
                    {mode === 'schedule' && lastShakeUndo && (
                        <button
                            type="button"
                            onClick={handleUndoShakeHourAds}
                            className={styles.todayBtn}
                            title="Revert the last Shake ads change for this tab"
                            style={{ marginLeft: 6, fontWeight: 700 }}
                        >
                            Undo ad shake
                        </button>
                    )}
                    {mode === 'schedule' && lastShowHourUndo && (
                        <button
                            type="button"
                            onClick={() => {
                                if (!lastShowHourUndo || !schedule) return;
                                const s = getSlot(lastShowHourUndo.slotId);
                                if (!s) {
                                    setLastShowHourUndo(null);
                                    return;
                                }
                                updateSlot(lastShowHourUndo.slotId, {
                                    ...s,
                                    items: JSON.parse(JSON.stringify(lastShowHourUndo.items)) as PlaylistItem[],
                                });
                                setLastShowHourUndo(null);
                            }}
                            className={styles.todayBtn}
                            title="Revert the last show-hour file ingest for this tab"
                            style={{ marginLeft: 6, fontWeight: 700 }}
                        >
                            Undo show hour ingest
                        </button>
                    )}
                </div>
                <div className={styles.controls}>
                    <button
                        className={`${styles.modeBtn} ${mode === 'schedule' ? styles.active : ''}`}
                        onClick={() => setMode('schedule')}
                        title="Schedule Mode - Edit hour slots"
                    >
                        <Edit2 size={14} /> Schedule
                    </button>
                    <button
                        className={`${styles.modeBtn} ${mode === 'overview' ? styles.active : ''}`}
                        onClick={() => setMode('overview')}
                        title="Overview Mode - View multi-site status"
                    >
                        <Eye size={14} /> Overview
                    </button>
                    {mode === 'overview' && (
                        <>
                            <div className={styles.divider} />
                            <button
                                className={`${styles.viewBtn} ${viewType === 'single' ? styles.active : ''}`}
                                onClick={() => setViewType('single')}
                            >
                                Single
                            </button>
                            <button
                                className={`${styles.viewBtn} ${viewType === 'multi' ? styles.active : ''}`}
                                onClick={() => setViewType('multi')}
                            >
                                Multi
                            </button>
                        </>
                    )}
                </div>
            </div>

            <div className={styles.content}>
                {mode === 'schedule' ? renderScheduleMode() : renderOverviewMode()}
            </div>

            {/* Context Menu */}
            {contextMenu.visible && mode === 'schedule' && contextMenu.slotId && (
                <ContextMenu
                    x={contextMenu.x}
                    y={contextMenu.y}
                    items={buildSchedulerHourMenuItems({
                        targetId: contextMenu.slotId,
                        targetType: 'hour',
                        slotId: contextMenu.slotId,
                    }, hasClipboard, { canUndoShake: !!lastShakeUndo, showHourSlot: currentSlot })}
                    onClose={() => setContextMenu(prev => ({ ...prev, visible: false }))}
                    onAction={handleScheduleMenuAction}
                />
            )}

            {/* Day Context Menu */}
            {dayContextMenu.visible && (
                <ContextMenu
                    x={dayContextMenu.x}
                    y={dayContextMenu.y}
                    items={buildSchedulerDayMenuItems({
                        targetId: dayContextMenu.dayInfo?.isoDate || '',
                        targetType: 'day',
                        dayInfo: dayContextMenu.dayInfo,
                    }, getClipboardDay() !== null)}
                    onClose={() => setDayContextMenu(prev => ({ ...prev, visible: false }))}
                    onAction={handleScheduleMenuAction}
                />
            )}

            {/* Hour Slot Editor */}
            {editingSlot && mode === 'schedule' && (
                <ErrorBoundary componentName="HourSlotEditor">
                    <HourSlotEditor
                        slot={editingSlot}
                        obsSessions={obsSessions}
                        onSave={handleSaveSlot}
                        onClose={() => setEditingSlot(null)}
                    />
                </ErrorBoundary>
            )}

            {/* Hour Operations Menu for overview mode */}
            {contextMenu.visible && mode === 'overview' && contextMenu.siteId !== undefined && (
                <HourOperationsMenu
                    x={contextMenu.x}
                    y={contextMenu.y}
                    siteId={contextMenu.siteId!}
                    day={contextMenu.day!}
                    hour={contextMenu.hour!}
                    onClose={() => setContextMenu(prev => ({ ...prev, visible: false }))}
                    onCopy={(siteId, day, hour) => {
                        copySiteHourToOtherSites(siteId, day, hour);
                    }}
                    onRedeployAds={(siteId, day, hour) => {
                        applySiteShift(siteId, day, hour, 'copy', 0, ['ad']);
                    }}
                    onFillWithIds={(siteId, day, hour) => {
                        console.log('Fill with IDs:', siteId, day, hour);
                    }}
                    onFillWithBumpers={(siteId, day, hour) => {
                        console.log('Fill with bumpers:', siteId, day, hour);
                    }}
                    onRecalculateAds={(siteId, day, hour) => {
                        applySiteShift(siteId, day, hour, 'copy', 0, ['ad']);
                    }}
                    onMerge={(siteId, day, hour) => {
                        applySiteShift(siteId, day, hour, 'copy', 1, ['program', 'paid_program']);
                    }}
                />
            )}
            {/* Day Context Menu */}
            {dayContextMenu.visible && dayContextMenu.dayInfo && (
                <div
                    className={styles.contextMenu}
                    style={{ left: dayContextMenu.x, top: dayContextMenu.y, position: 'fixed', zIndex: 10000 }}
                >
                    <div className={styles.menuHeader}>
                        {dayContextMenu.dayInfo.label} Operations
                    </div>
                    <button className={styles.menuItem} onClick={handleCopyDay}>
                        <Copy size={14} /> Copy Full Day
                    </button>
                    {localStorage.getItem('copiedDaySlots') && (
                        <>
                            <div className={styles.menuDivider}></div>
                            <button className={styles.menuItem} onClick={handlePasteDayExact}>
                                <Clipboard size={14} /> Paste Exact Copy
                            </button>
                            <button className={styles.menuItem} onClick={handlePasteDayShiftAds}>
                                <Sparkles size={14} /> Paste & Shift Ads
                            </button>
                        </>
                    )}
                </div>
            )}
        </div>
    );
}
