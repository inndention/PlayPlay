// ============================================
// PLAYLIST EDITOR
// Professional broadcast rundown editor
// ============================================

import React, { useState, useRef, useCallback, useEffect, useMemo } from 'react';
import {
    DndContext,
    closestCenter,
    KeyboardSensor,
    PointerSensor,
    useSensor,
    useSensors,
    DragEndEvent,
    DragStartEvent,
    DragOverlay,
} from '@dnd-kit/core';
import {
    arrayMove,
    SortableContext,
    sortableKeyboardCoordinates,
    useSortable,
    verticalListSortingStrategy,
} from '@dnd-kit/sortable';
import { CSS } from '@dnd-kit/utilities';
import { 
    PlaylistItem, 
    MediaItem, 
    MediaType,
    SmartSlotConfig 
} from '../lib/types';
import {
    Clock,
    Trash2,
    GripVertical,
    Film,
    Music,
    Tv,
    Radio,
    Megaphone,
    Image as ImageIcon,
    ChevronDown,
    ChevronUp,
    MoreHorizontal,
    Play,
    Pause,
    Copy,
    Scissors,
    Target,
    Calendar,
    AlertCircle,
    Check,
    Eye,
    Lock,
    Unlock,
    RefreshCw,
    Plus,
    Loader2,
    SkipForward,
    ArrowUp,
    ArrowDown,
    Ban,
} from 'lucide-react';
import styles from './PlaylistEditor.module.css';
import { resolveMediaUrlSync } from '../lib/fileResolver';
import type { PSIPEvent } from '../lib/psipGenerator';

interface PlaylistEditorProps {
    playlist: PlaylistItem[];
    setPlaylist: (items: PlaylistItem[]) => void;
    activeItemId?: string | null;
    onItemSelect?: (item: PlaylistItem) => void;
    onItemContextMenu?: (e: React.MouseEvent, item: PlaylistItem) => void;
    showTimecodes?: boolean;
    isLocked?: boolean;
    hourStartTime?: number; // Unix timestamp for hour start (for time calculations)
    loopMode?: boolean;
    onLoopModeChange?: (loop: boolean) => void;
    // Additional props for compatibility
    onRemove?: (id: string) => void;
    onSetFixedTime?: (id: string, time: number | null) => void;
    scheduleStartTime?: number;
    channelName?: string;
    onPSIPUpdate?: (events: PSIPEvent[]) => void;
    // Hour rundown controls
    onSkipItem?: (id: string) => void;
    onAddFromLibrary?: () => void;
    currentPlaybackTime?: number;
    isPlaying?: boolean;
}

const TYPE_ICONS: Record<MediaType, React.ReactNode> = {
    video: <Film size={14} />,
    program: <Tv size={14} />,
    ad: <Megaphone size={14} />,
    station_id: <Radio size={14} />,
    bumper: <Play size={14} />,
    promo: <Film size={14} />,
    image: <ImageIcon size={14} />,
    live: <Radio size={14} />,
    paid_program: <Tv size={14} />,
    gap: <Clock size={14} />,
};

const TYPE_COLORS: Record<MediaType, string> = {
    video: '#64748b',
    program: '#3b82f6',
    ad: '#ef4444',
    station_id: '#22c55e',
    bumper: '#f59e0b',
    promo: '#8b5cf6',
    image: '#06b6d4',
    live: '#ec4899',
    paid_program: '#6366f1',
    gap: '#94a3b8',
};

function formatDuration(seconds: number): string {
    const mins = Math.floor(seconds / 60);
    const secs = Math.floor(seconds % 60);
    return `${mins}:${secs.toString().padStart(2, '0')}`;
}

function formatTimecode(ms: number): string {
    const date = new Date(ms);
    const hours = date.getHours().toString().padStart(2, '0');
    const mins = date.getMinutes().toString().padStart(2, '0');
    const secs = date.getSeconds().toString().padStart(2, '0');
    return `${hours}:${mins}:${secs}`;
}

function formatHMS(seconds: number): string {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

function formatRelAge(isoDate: string | undefined): string {
    if (!isoDate) return '';
    const ms = Date.now() - new Date(isoDate).getTime();
    const d = Math.floor(ms / 86_400_000);
    const h = Math.floor((ms % 86_400_000) / 3_600_000);
    if (d > 0) return `${d}D ${h}H AGO`;
    if (h > 0) return `${h}H AGO`;
    return 'JUST NOW';
}

function formatDateMDY(isoDate: string | undefined): string {
    if (!isoDate) return 'UNSET';
    const d = new Date(isoDate);
    return `${(d.getMonth() + 1).toString().padStart(2, '0')}/${d.getDate().toString().padStart(2, '0')}/${d.getFullYear().toString().slice(-2)}`;
}

interface SortableItemProps {
    item: PlaylistItem;
    isActive: boolean;
    isNext: boolean;
    activeRemaining: number | null;
    onSelect: () => void;
    onContextMenu: (e: React.MouseEvent) => void;
    onDelete: () => void;
    onMoveUp?: () => void;
    onMoveDown?: () => void;
    onSkip?: () => void;
    showTimecodes: boolean;
    isLocked: boolean;
    canMoveUp: boolean;
    canMoveDown: boolean;
    currentPlaybackTime: number;
    isPlaying: boolean;
}

function SortableItem({
    item,
    isActive,
    isNext,
    activeRemaining,
    onSelect,
    onContextMenu,
    onDelete,
    onMoveUp,
    onMoveDown,
    onSkip,
    showTimecodes,
    isLocked,
    canMoveUp,
    canMoveDown,
    currentPlaybackTime,
    isPlaying,
}: SortableItemProps) {
    const {
        attributes,
        listeners,
        setNodeRef,
        transform,
        transition,
        isDragging,
    } = useSortable({ id: item.id, disabled: isLocked });

    const style = {
        transform: CSS.Transform.toString(transform),
        transition,
        opacity: isDragging ? 0.5 : 1,
    };

    const typeColor = TYPE_COLORS[item.media.type] || '#64748b';
    const typeIcon = TYPE_ICONS[item.media.type] || <Film size={14} />;
    const isSkipped = (item as any).isSkipped;
    const isGap = item.media.type === 'gap' || item.media.src?.startsWith('signal:');
    const thumbUrl = !isGap ? resolveMediaUrlSync(item.media) : '';
    const plannedStart = item.startTime ?? null;
    const plannedStop = item.endTime ?? (item.startTime ? item.startTime + item.media.duration * 1000 : null);
    const actualStart = item.startTime != null && item.timeIn != null
        ? item.startTime + Math.max(0, currentPlaybackTime - item.timeIn) * 1000
        : null;
    const actualStop = actualStart != null ? actualStart + item.media.duration * 1000 : null;
    const itemTimeIn = item.timeIn || 0;
    const effectiveDuration = Math.max(1, (item.timeOut ?? item.media.duration) - itemTimeIn);
    const remaining = isActive ? Math.max(0, effectiveDuration - Math.max(0, currentPlaybackTime - itemTimeIn)) : null;
    const imminent = isActive && isPlaying && remaining !== null && remaining <= 30;
    const blinkMs = remaining != null && remaining > 0
        ? Math.max(180, Math.min(900, Math.round((remaining / 30) * 900)))
        : 900;

    // Next-item blink: red when active item has ≤30s left, accelerates as time runs out
    const isNextImminent = isNext && activeRemaining !== null && activeRemaining <= 30;
    const nextBlinkMs = isNextImminent && activeRemaining !== null && activeRemaining > 0
        ? Math.max(120, Math.min(900, Math.round((activeRemaining / 30) * 900)))
        : 900;

    // Today's play count for ads
    const todayStr = new Date().toISOString().split('T')[0];
    const todayPlayCount = item.media.stats?.playHistory?.filter(
        e => new Date(e.timestamp).toISOString().split('T')[0] === todayStr
    ).length ?? 0;

    const itemStyleCombined = isNextImminent
        ? { ...style, animationDuration: `${nextBlinkMs}ms` }
        : imminent
        ? { ...style, animationDuration: `${blinkMs}ms` }
        : style;

    return (
        <div
            ref={setNodeRef}
            className={`${styles.item} ${isActive ? styles.itemActive : ''} ${isNext && !isActive ? styles.itemNext : ''} ${isNextImminent ? styles.itemNextImminent : ''} ${isDragging ? styles.dragging : ''} ${isSkipped ? styles.skipped : ''} ${imminent ? styles.itemImminent : ''}`}
            style={itemStyleCombined}
            onClick={onSelect}
            onContextMenu={onContextMenu}
        >
            {!isLocked && (
                <div className={styles.dragHandle} {...attributes} {...listeners}>
                    <GripVertical size={14} />
                </div>
            )}

            <div className={styles.itemThumb}>
                {thumbUrl ? (
                    <video key={`thumb-${item.id}-${thumbUrl}`} src={thumbUrl} muted preload="metadata" playsInline className={styles.thumbVideo} aria-hidden />
                ) : (
                    <div className={styles.thumbPlaceholder} style={{ backgroundColor: typeColor }}>{typeIcon}</div>
                )}
            </div>

            <div className={styles.typeIndicator} style={{ backgroundColor: typeColor }}>
                {typeIcon}
            </div>
            
            <div className={styles.itemContent}>
                <div className={styles.itemTitle}>
                    {item.media.title}
                    {item.segmentId && (
                        <span className={styles.segmentTag}>
                            SEG {item.segmentLabel || item.segmentId}
                        </span>
                    )}
                </div>
                <div className={styles.itemMeta}>
                    <span className={styles.duration}>
                        <Clock size={12} />
                        {formatDuration(item.media.duration)}
                    </span>
                    {item.isFixedTime && (
                        <span className={styles.fixedTime}>
                            <Target size={12} />
                            Fixed
                        </span>
                    )}
                    {item.smartConfig && (
                        <span className={styles.smartSlot}>
                            <RefreshCw size={12} />
                            Smart
                        </span>
                    )}
                    {isSkipped && (
                        <span className={styles.skippedBadge}>
                            <Ban size={12} />
                            Skipped
                        </span>
                    )}
                </div>
                {(plannedStart || actualStart) && (
                    <div className={styles.timeLineMeta}>
                        {plannedStart && plannedStop && (
                            <span className={styles.timePair}>
                                Planned {formatTimecode(plannedStart)} - {formatTimecode(plannedStop)}
                            </span>
                        )}
                        {actualStart && actualStop && (
                            <span className={styles.timePairActual}>
                                Actual {formatTimecode(actualStart)} - {formatTimecode(actualStop)}
                            </span>
                        )}
                    </div>
                )}

                {/* IN / OUT times */}
                <div className={styles.inOutTimes}>
                    <span className={styles.inOutLabel}>IN</span>
                    <span className={styles.inOutValue}>{formatHMS(itemTimeIn)}</span>
                    <span className={styles.inOutSep}>/</span>
                    <span className={styles.inOutLabel}>OUT</span>
                    <span className={styles.inOutValue}>{formatHMS(item.timeOut ?? item.media.duration)}</span>
                </div>

                {/* Type-specific metadata */}
                {(item.media.type === 'program' || item.media.type === 'paid_program') && (
                    <div className={styles.typeMeta}>
                        <span>CREATED: {formatDateMDY(item.media.dateMade || item.media.addedAt)} ({formatRelAge(item.media.dateMade || item.media.addedAt)})</span>
                    </div>
                )}
                {item.media.type === 'ad' && (
                    <div className={styles.typeMeta}>
                        <span>EXPIRES: {formatDateMDY(item.media.availableEnd)}</span>
                        <span>PLAYED TODAY: {todayPlayCount} · TOTAL: {item.media.playCount ?? 0}</span>
                        {item.media.lastPlayedAt && <span>LAST: {formatRelAge(item.media.lastPlayedAt)}</span>}
                    </div>
                )}
                {(item.media.type === 'station_id' || item.media.type === 'bumper') && (
                    <div className={styles.typeMeta}>
                        <span>FILL TIME {Math.round(item.media.duration)}s</span>
                    </div>
                )}
            </div>

            {showTimecodes && item.startTime && (
                <div className={styles.timecode}>
                    {formatTimecode(item.startTime)}
                </div>
            )}

            {!isLocked && (
                <div className={styles.itemActions}>
                    <button
                        className={styles.actionBtn}
                        onClick={(e) => {
                            e.stopPropagation();
                            onMoveUp?.();
                        }}
                        disabled={!canMoveUp}
                        title="Move up"
                    >
                        <ArrowUp size={12} />
                    </button>
                    <button
                        className={styles.actionBtn}
                        onClick={(e) => {
                            e.stopPropagation();
                            onMoveDown?.();
                        }}
                        disabled={!canMoveDown}
                        title="Move down"
                    >
                        <ArrowDown size={12} />
                    </button>
                    <button
                        className={`${styles.actionBtn} ${styles.skipBtn}`}
                        onClick={(e) => {
                            e.stopPropagation();
                            onSkip?.();
                        }}
                        title={isSkipped ? "Unskip item" : "Skip item"}
                    >
                        <SkipForward size={12} />
                    </button>
                    <button
                        className={`${styles.actionBtn} ${styles.deleteAction}`}
                        onClick={(e) => {
                            e.stopPropagation();
                            onDelete();
                        }}
                        title="Remove from playlist"
                    >
                        <Trash2 size={12} />
                    </button>
                </div>
            )}
        </div>
    );
}

export function PlaylistEditor({
    playlist,
    setPlaylist,
    activeItemId,
    onItemSelect,
    onItemContextMenu,
    showTimecodes = true,
    isLocked = false,
    hourStartTime,
    loopMode = false,
    onLoopModeChange,
    onRemove,
    onSetFixedTime,
    scheduleStartTime,
    channelName,
    onPSIPUpdate,
    onSkipItem,
    onAddFromLibrary,
    currentPlaybackTime = 0,
    isPlaying = false,
}: PlaylistEditorProps) {
    const [dragActiveId, setDragActiveId] = useState<string | null>(null);

    const activeIndex = useMemo(
        () => playlist.findIndex(item => item.id === activeItemId),
        [playlist, activeItemId]
    );
    const activeItem = activeIndex >= 0 ? playlist[activeIndex] : null;
    const nextItem = activeIndex >= 0 && activeIndex < playlist.length - 1 ? playlist[activeIndex + 1] : null;

    const activeRemaining = useMemo(() => {
        if (!activeItem || !isPlaying) return null;
        const timeIn = activeItem.timeIn || 0;
        const effectiveDur = Math.max(1, (activeItem.timeOut ?? activeItem.media.duration) - timeIn);
        return Math.max(0, effectiveDur - Math.max(0, currentPlaybackTime - timeIn));
    }, [activeItem, currentPlaybackTime, isPlaying]);

    const sensors = useSensors(
        useSensor(PointerSensor, {
            activationConstraint: {
                distance: 8,
            },
        }),
        useSensor(KeyboardSensor, {
            coordinateGetter: sortableKeyboardCoordinates,
        })
    );

    const handleDragStart = (event: DragStartEvent) => {
        setDragActiveId(event.active.id as string);
    };

    const handleDragEnd = (event: DragEndEvent) => {
        const { active, over } = event;
        setDragActiveId(null);

        if (over && active.id !== over.id) {
            const oldIndex = playlist.findIndex((item) => item.id === active.id);
            const newIndex = playlist.findIndex((item) => item.id === over.id);
            
            const newPlaylist = arrayMove(playlist, oldIndex, newIndex);
            setPlaylist(newPlaylist);
        }
    };

    const handleDelete = useCallback((itemId: string) => {
        setPlaylist(playlist.filter(item => item.id !== itemId));
    }, [playlist, setPlaylist]);

    const handleMoveUp = useCallback((itemId: string) => {
        const index = playlist.findIndex(item => item.id === itemId);
        if (index > 0) {
            const newPlaylist = arrayMove(playlist, index, index - 1);
            setPlaylist(newPlaylist);
        }
    }, [playlist, setPlaylist]);

    const handleMoveDown = useCallback((itemId: string) => {
        const index = playlist.findIndex(item => item.id === itemId);
        if (index < playlist.length - 1) {
            const newPlaylist = arrayMove(playlist, index, index + 1);
            setPlaylist(newPlaylist);
        }
    }, [playlist, setPlaylist]);

    const handleSkipItem = useCallback((itemId: string) => {
        if (onSkipItem) {
            onSkipItem(itemId);
        } else {
            // Default behavior: toggle skipped state
            setPlaylist(playlist.map(item => 
                item.id === itemId 
                    ? { ...item, isSkipped: !(item as any).isSkipped } as any
                    : item
            ));
        }
    }, [playlist, setPlaylist, onSkipItem]);

    const totalDuration = useMemo(() => {
        return playlist.reduce((sum, item) => sum + (item.media.duration || 0), 0);
    }, [playlist]);

    const draggedItem = dragActiveId 
        ? playlist.find(item => item.id === dragActiveId) 
        : null;

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <span>Hour Rundown</span>
                    <span className={styles.itemCount}>({playlist.length} items)</span>
                </div>
                <div className={styles.headerActions}>
                    <span className={styles.totalDuration}>
                        <Clock size={14} />
                        {formatDuration(totalDuration)}
                    </span>
                    {onAddFromLibrary && !isLocked && (
                        <button
                            className={styles.addBtn}
                            onClick={onAddFromLibrary}
                            title="Add item from library"
                        >
                            <Plus size={14} />
                        </button>
                    )}
                    {onLoopModeChange && (
                        <button
                            className={`${styles.loopBtn} ${loopMode ? styles.active : ''}`}
                            onClick={() => onLoopModeChange(!loopMode)}
                            title={loopMode ? 'Loop enabled' : 'Loop disabled'}
                        >
                            <RefreshCw size={14} />
                        </button>
                    )}
                    {isLocked ? (
                        <Lock size={14} className={styles.lockIcon} />
                    ) : (
                        <Unlock size={14} className={styles.unlockIcon} />
                    )}
                </div>
            </div>

            {/* Playhead — shows current item countdown and next */}
            {activeItem && (
                <div className={styles.playhead}>
                    <div className={styles.playheadCurrent}>
                        <Play size={11} />
                        <span className={styles.playheadTitle}>{activeItem.media.title}</span>
                        {activeRemaining !== null && (
                            <span className={styles.playheadCountdown}>-{formatHMS(activeRemaining)}</span>
                        )}
                    </div>
                    {nextItem && (
                        <div className={styles.playheadNext}>
                            <span>→ NEXT:</span>
                            <span className={styles.playheadNextTitle}>{nextItem.media.title}</span>
                        </div>
                    )}
                </div>
            )}

            <div className={styles.list}>
                {playlist.length === 0 ? (
                    <div className={styles.emptyState}>
                        <Film size={32} strokeWidth={1} />
                        <p>No items in playlist</p>
                        <p className={styles.hint}>Drag media here to add</p>
                    </div>
                ) : (
                    <DndContext
                        sensors={sensors}
                        collisionDetection={closestCenter}
                        onDragStart={handleDragStart}
                        onDragEnd={handleDragEnd}
                    >
                        <SortableContext
                            items={playlist.map(item => item.id)}
                            strategy={verticalListSortingStrategy}
                        >
                            {playlist.map((item, index) => (
                                <SortableItem
                                    key={item.id}
                                    item={item}
                                    isActive={item.id === activeItemId}
                                    isNext={index === activeIndex + 1}
                                    activeRemaining={activeRemaining}
                                    onSelect={() => onItemSelect?.(item)}
                                    onContextMenu={(e) => onItemContextMenu?.(e, item)}
                                    onDelete={() => handleDelete(item.id)}
                                    onMoveUp={() => handleMoveUp(item.id)}
                                    onMoveDown={() => handleMoveDown(item.id)}
                                    onSkip={() => handleSkipItem(item.id)}
                                    showTimecodes={showTimecodes}
                                    isLocked={isLocked}
                                    canMoveUp={index > 0}
                                    canMoveDown={index < playlist.length - 1}
                                    currentPlaybackTime={currentPlaybackTime}
                                    isPlaying={isPlaying}
                                />
                            ))}
                        </SortableContext>
                        <DragOverlay>
                            {draggedItem ? (
                                <div className={`${styles.item} ${styles.dragOverlay}`}>
                                    <div className={styles.dragHandle}>
                                        <GripVertical size={14} />
                                    </div>
                                    <div 
                                        className={styles.typeIndicator} 
                                        style={{ backgroundColor: TYPE_COLORS[draggedItem.media.type] }}
                                    >
                                        {TYPE_ICONS[draggedItem.media.type]}
                                    </div>
                                    <div className={styles.itemContent}>
                                        <div className={styles.itemTitle}>{draggedItem.media.title}</div>
                                    </div>
                                </div>
                            ) : null}
                        </DragOverlay>
                    </DndContext>
                )}
            </div>

            {totalDuration > 3600 && (
                <div className={styles.overflowWarning}>
                    <AlertCircle size={14} />
                    <span>Playlist exceeds 1 hour by {formatDuration(totalDuration - 3600)}</span>
                </div>
            )}
        </div>
    );
}

export default PlaylistEditor;
