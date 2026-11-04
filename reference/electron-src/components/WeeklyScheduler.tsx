import React, { useState, useCallback, useRef, useEffect } from 'react';
import { HourSlot, WeeklySchedule, PlaylistItem, MediaItem, OBSSession } from '../lib/types';
import { HourSlotEditor } from './HourSlotEditor';
import {
    loadWeeklySchedule,
    saveWeeklySchedule,
    copyHourToClipboard,
    getClipboardSlot,
    pasteHourFromClipboard,
    createFreePlaylist,
    loadFreePlaylists,
    saveFreePlaylists,
    createEmptyWeeklySchedule
} from '../lib/storage';
import { formatDuration } from '../lib/utils';
import { Copy, Clipboard, ArrowRight, ArrowDown, Repeat, Download, Shuffle, Lock, MoreHorizontal, X } from 'lucide-react';
import styles from './WeeklyScheduler.module.css';

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const HOURS = Array.from({ length: 24 }, (_, i) => i);

interface ContextMenuState {
    visible: boolean;
    x: number;
    y: number;
    slotId: string | null;
}

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

interface WeeklySchedulerProps {
    obsSessions?: OBSSession[];
}

export function WeeklyScheduler({ obsSessions = [] }: WeeklySchedulerProps) {
    const [schedule, setSchedule] = useState<WeeklySchedule>(createEmptyWeeklySchedule());
    const [selectedSlot, setSelectedSlot] = useState<string | null>(null);
    const [contextMenu, setContextMenu] = useState<ContextMenuState>({ visible: false, x: 0, y: 0, slotId: null });
    const [dragOverSlot, setDragOverSlot] = useState<string | null>(null);
    const [editingSlot, setEditingSlot] = useState<HourSlot | null>(null);
    const [mergedSlots, setMergedSlots] = useState<Set<string>>(new Set());
    const containerRef = useRef<HTMLDivElement>(null);

    // Load schedule on mount
    useEffect(() => {
        (async () => {
            const loaded = await loadWeeklySchedule();
            setSchedule(loaded);
        })();
    }, []);

    // Save on changes
    useEffect(() => {
        if (schedule.slots && Object.keys(schedule.slots).length > 0) {
            console.log('[WeeklyScheduler] Schedule changed, saving...',
                Object.keys(schedule.slots).filter(k => schedule.slots[k].items.length > 0).length, 'non-empty slots');
            saveWeeklySchedule(schedule)
                .then(() => console.log('[WeeklyScheduler] Save complete'))
                .catch(err => console.error('[WeeklyScheduler] Save failed:', err));
        }
    }, [schedule]);

    // Close context menu on click outside
    useEffect(() => {
        const handleClick = () => setContextMenu(prev => ({ ...prev, visible: false }));
        document.addEventListener('click', handleClick);
        return () => document.removeEventListener('click', handleClick);
    }, []);

    const handleCellClick = (slotId: string) => {
        setSelectedSlot(slotId);
    };

    const handleCellDoubleClick = (slotId: string) => {
        let slot = schedule.slots[slotId];
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
            // Update schedule with new slot
            setSchedule(prev => ({
                ...prev,
                slots: {
                    ...prev.slots,
                    [slotId]: slot,
                },
            }));
        }
        setEditingSlot(slot);
    };

    const handleContextMenu = (e: React.MouseEvent, slotId: string) => {
        e.preventDefault();
        setContextMenu({
            visible: true,
            x: e.clientX,
            y: e.clientY,
            slotId,
        });
        setSelectedSlot(slotId);
    };

    const handleCopyHour = useCallback(() => {
        if (!contextMenu.slotId) return;
        const slot = schedule.slots[contextMenu.slotId];
        if (slot) copyHourToClipboard(slot);
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule]);

    const handleMergeWithNextHours = useCallback(async () => {
        if (!contextMenu.slotId) return;
        const { safePrompt } = await import('../lib/promptPolyfill');
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

    const handlePasteHour = useCallback(() => {
        if (!contextMenu.slotId) return;
        const newSchedule = pasteHourFromClipboard(contextMenu.slotId, schedule);
        setSchedule(newSchedule);
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule]);

    const handleQuickCopyNextHour = useCallback(() => {
        if (!contextMenu.slotId) return;
        const parsed = parseSlotId(contextMenu.slotId);
        const nextHour = parsed.hour === 23 ? 0 : parsed.hour + 1;
        const nextDay = parsed.hour === 23 ? ((parsed.dayOfWeek ?? 0) + 1) % 7 : (parsed.dayOfWeek ?? 0);
        const targetId = `${nextDay}-${nextHour}`;

        const slot = schedule.slots[contextMenu.slotId];
        if (slot) {
            copyHourToClipboard(slot);
            const newSchedule = pasteHourFromClipboard(targetId, schedule);
            setSchedule(newSchedule);
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule]);

    const handleQuickCopyNextDay = useCallback(() => {
        if (!contextMenu.slotId) return;
        const parsed = parseSlotId(contextMenu.slotId);
        const nextDay = ((parsed.dayOfWeek ?? 0) + 1) % 7;
        const targetId = `${nextDay}-${parsed.hour}`;

        const slot = schedule.slots[contextMenu.slotId];
        if (slot) {
            copyHourToClipboard(slot);
            const newSchedule = pasteHourFromClipboard(targetId, schedule);
            setSchedule(newSchedule);
        }
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId, schedule]);

    const handleToggleLoop = useCallback(() => {
        if (!contextMenu.slotId) return;
        setSchedule(prev => ({
            ...prev,
            slots: {
                ...prev.slots,
                [contextMenu.slotId!]: {
                    ...prev.slots[contextMenu.slotId!],
                    isLooping: !prev.slots[contextMenu.slotId!].isLooping,
                },
            },
        }));
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId]);

    const handleToggleRandomize = useCallback(() => {
        if (!contextMenu.slotId) return;
        setSchedule(prev => ({
            ...prev,
            slots: {
                ...prev.slots,
                [contextMenu.slotId!]: {
                    ...prev.slots[contextMenu.slotId!],
                    randomizeAds: !prev.slots[contextMenu.slotId!].randomizeAds,
                },
            },
        }));
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId]);

    const handleToggleKeepPrograms = useCallback(() => {
        if (!contextMenu.slotId) return;
        setSchedule(prev => ({
            ...prev,
            slots: {
                ...prev.slots,
                [contextMenu.slotId!]: {
                    ...prev.slots[contextMenu.slotId!],
                    keepProgramOrder: !prev.slots[contextMenu.slotId!].keepProgramOrder,
                },
            },
        }));
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId]);

    const handleExportToFreePlaylist = useCallback(async () => {
        if (!contextMenu.slotId) return;
        const slot = schedule.slots[contextMenu.slotId];
        if (!slot || slot.items.length === 0) return;

        let dayLabel = '';
        if (typeof slot.dayOfWeek === 'number') {
            dayLabel = DAYS[slot.dayOfWeek] || '';
        } else {
            dayLabel = slot.date || '';
        }

        const name = `${dayLabel} ${slot.hour.toString().padStart(2, '0')}:00 Export`;

        const playlist = createFreePlaylist(name, contextMenu.slotId);
        playlist.items = slot.items.map(item => ({ ...item, id: `fp_${Date.now()}_${Math.random().toString(36).substr(2, 5)}` }));

        const playlists = await loadFreePlaylists();
        playlists.push(playlist);
        await saveFreePlaylists(playlists);

        setContextMenu(prev => ({ ...prev, visible: false }));
        alert(`Exported to Free Playlist: "${name}"`);
    }, [contextMenu.slotId, schedule]);

    const handleClearHour = useCallback(() => {
        if (!contextMenu.slotId) return;
        setSchedule(prev => ({
            ...prev,
            slots: {
                ...prev.slots,
                [contextMenu.slotId!]: {
                    ...prev.slots[contextMenu.slotId!],
                    items: [],
                },
            },
        }));
        setContextMenu(prev => ({ ...prev, visible: false }));
    }, [contextMenu.slotId]);

    const handleSaveSlot = useCallback((updatedSlot: HourSlot) => {
        console.log('WeeklyScheduler: Saving slot to schedule:', updatedSlot.id);
        const now = Date.now();
        setSchedule(prev => {
            const newSchedule = {
                ...prev,
                slots: {
                    ...prev.slots,
                    [updatedSlot.id]: updatedSlot,
                },
                lastModified: now,
            };
            // Save immediately
            saveWeeklySchedule(newSchedule).catch(console.error);
            console.log('WeeklyScheduler: Schedule saved, lastModified:', newSchedule.lastModified);
            
            // Dispatch event after a small delay to ensure state is updated
            setTimeout(() => {
                const event = new CustomEvent('schedule-updated', { 
                    detail: { slotId: updatedSlot.id } 
                });
                console.log('WeeklyScheduler: Dispatching schedule-updated event:', updatedSlot.id);
                window.dispatchEvent(event);
            }, 100);
            
            return newSchedule;
        });
        setEditingSlot(null);
    }, []);

    // Calculate fill level for a slot
    const getSlotFillLevel = (slot: HourSlot): number => {
        const totalDuration = slot.items.reduce((sum, item) => sum + item.media.duration, 0);
        return Math.min((totalDuration / 3600) * 100, 100); // 3600 seconds = 1 hour
    };

    const getSlotInfo = (slot: HourSlot): { fill: number; count: number; duration: number } => {
        const duration = slot.items.reduce((sum, item) => sum + item.media.duration, 0);
        return {
            fill: Math.min((duration / 3600) * 100, 100),
            count: slot.items.length,
            duration,
        };
    };

    const currentSlot = contextMenu.slotId ? schedule.slots[contextMenu.slotId] : null;
    const hasClipboard = getClipboardSlot() !== null;

    let slotDayLabel = '';
    if (currentSlot && typeof currentSlot.dayOfWeek === 'number') {
        slotDayLabel = DAYS[currentSlot.dayOfWeek] || '';
    }
    const slotHourLabel = currentSlot ? currentSlot.hour.toString().padStart(2, '0') : '';

    // Drag and drop handlers
    const handleDragOver = useCallback((e: React.DragEvent, slotId: string) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
        setDragOverSlot(slotId);
    }, []);

    const handleDragLeave = useCallback(() => {
        setDragOverSlot(null);
    }, []);

    const handleDrop = useCallback((e: React.DragEvent, slotId: string) => {
        e.preventDefault();
        setDragOverSlot(null);

        try {
            const data = e.dataTransfer.getData('application/json');
            if (!data) return;

            const media: MediaItem = JSON.parse(data);
            
            const newItem: PlaylistItem = {
                id: generateId(),
                mediaId: media.id,
                media: media,
                isResolved: true,
            };

            const parsed = parseSlotId(slotId);
            
            setSchedule(prev => {
                const existingSlot = prev.slots[slotId];
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
                
                return {
                    ...prev,
                    slots: {
                        ...prev.slots,
                        [slotId]: updatedSlot,
                    },
                    lastModified: Date.now(),
                };
            });
            // Don't dispatch event here - the debounced save in useScheduleGrid will handle it
        } catch (err) {
            console.error('Failed to parse dropped media:', err);
        }
    }, []);

    return (
        <div className={styles.container} ref={containerRef}>
            <div className={styles.header}>
                <h2 className={styles.title}>Weekly Schedule</h2>
                <div className={styles.legend}>
                    <span className={styles.legendItem}><span className={styles.legendEmpty}></span> Empty</span>
                    <span className={styles.legendItem}><span className={styles.legendPartial}></span> Partial</span>
                    <span className={styles.legendItem}><span className={styles.legendFull}></span> Full</span>
                    <span className={styles.legendItem}><span className={styles.legendOver}></span> Over</span>
                </div>
            </div>

            <div className={styles.grid}>
                {/* Header row */}
                <div className={styles.cornerCell}></div>
                {DAYS.map(day => (
                    <div key={day} className={styles.dayHeader}>{day}</div>
                ))}

                {/* Hour rows */}
                {HOURS.map(hour => (
                    <React.Fragment key={hour}>
                        <div className={styles.hourLabel}>
                            {hour.toString().padStart(2, '0')}:00
                        </div>
                        {DAYS.map((_, dayIndex) => {
                            const slotId = `${dayIndex}-${hour}`;
                            const slot = schedule.slots[slotId];
                            const info = getSlotInfo(slot);
                            const isSelected = selectedSlot === slotId;

                            const isDragOver = dragOverSlot === slotId;
                            const isMerged = mergedSlots.has(slotId);

                            return (
                                <div
                                    key={slotId}
                                    className={`${styles.cell} ${isSelected ? styles.cellSelected : ''} ${slot?.isLooping ? styles.cellLooping : ''} ${isDragOver ? styles.cellDragOver : ''} ${isMerged ? styles.cellMerged : ''}`}
                                    onClick={() => handleCellClick(slotId)}
                                    onDoubleClick={() => handleCellDoubleClick(slotId)}
                                    onContextMenu={(e) => handleContextMenu(e, slotId)}
                                    onDragOver={(e) => handleDragOver(e, slotId)}
                                    onDragLeave={handleDragLeave}
                                    onDrop={(e) => handleDrop(e, slotId)}
                                    title={`${info.count} items, ${formatDuration(info.duration)} (Double-click to edit)`}
                                >
                                    <div
                                        className={styles.fillBar}
                                        style={{
                                            width: `${info.fill}%`,
                                            backgroundColor: info.fill > 100 ? 'var(--danger)' :
                                                info.fill > 80 ? 'var(--success)' :
                                                    info.fill > 0 ? 'var(--warning)' : 'transparent'
                                        }}
                                    />
                                    {info.count > 0 && (
                                        <span className={styles.itemCount}>{info.count}</span>
                                    )}
                                    {slot.isLooping && <Repeat size={10} className={styles.loopIcon} />}
                                    {slot.randomizeAds && info.count > 0 && <Shuffle size={10} className={styles.shuffleIcon} />}
                                </div>
                            );
                        })}
                    </React.Fragment>
                ))}
            </div>

            {/* Context Menu */}
            {contextMenu.visible && (
                <div
                    className={styles.contextMenu}
                    style={{ left: contextMenu.x, top: contextMenu.y }}
                    onClick={(e) => e.stopPropagation()}
                >
                    <div className={styles.menuHeader}>
                        {slotDayLabel} {slotHourLabel}:00
                    </div>
                    <button className={styles.menuItem} onClick={handleCopyHour}>
                        <Copy size={14} /> Copy Hour
                    </button>
                    <button className={styles.menuItem} onClick={handlePasteHour} disabled={!hasClipboard}>
                        <Clipboard size={14} /> Paste Hour
                    </button>
                    <div className={styles.menuDivider} />
                    <button className={styles.menuItem} onClick={handleQuickCopyNextHour}>
                        <ArrowRight size={14} /> Quick Copy → Next Hour
                    </button>
                    <button className={styles.menuItem} onClick={handleQuickCopyNextDay}>
                        <ArrowDown size={14} /> Quick Copy → Next Day
                    </button>
                    <button className={styles.menuItem} onClick={handleMergeWithNextHours}>
                        <Download size={14} /> Merge with Next Hours…
                    </button>
                    <div className={styles.menuDivider} />
                    <button className={styles.menuItem} onClick={handleToggleLoop}>
                        <Repeat size={14} /> {currentSlot?.isLooping ? '✓ ' : ''}Loop Hour
                    </button>
                    <button className={styles.menuItem} onClick={handleToggleRandomize}>
                        <Shuffle size={14} /> {currentSlot?.randomizeAds ? '✓ ' : ''}Randomize Ads
                    </button>
                    <button className={styles.menuItem} onClick={handleToggleKeepPrograms}>
                        <Lock size={14} /> {currentSlot?.keepProgramOrder ? '✓ ' : ''}Keep Program Order
                    </button>
                    <div className={styles.menuDivider} />
                    <button className={styles.menuItem} onClick={handleExportToFreePlaylist}>
                        <Download size={14} /> Export to Free Playlist
                    </button>
                    <button className={`${styles.menuItem} ${styles.menuItemDanger}`} onClick={handleClearHour}>
                        <X size={14} /> Clear Hour
                    </button>
                </div>
            )}

            {/* Hour Slot Editor */}
            {editingSlot && (
                <HourSlotEditor
                    slot={editingSlot}
                    obsSessions={obsSessions}
                    onSave={handleSaveSlot}
                    onClose={() => setEditingSlot(null)}
                />
            )}
        </div>
    );
}
