/**
 * RUNDOWN MANAGER
 * Professional broadcast rundown and show management
 * Inspired by: Vizrt Mosart, Ross Video Overdrive, Grass Valley Ignite
 * 
 * Features:
 * - Segment-based rundown structure
 * - Live timing calculations
 * - Backtime/fronttime display
 * - Auto-rollover support
 * - Break management
 * - Story reordering
 */

import React, { useState, useCallback, useMemo, useEffect, useRef } from 'react';
import {
    GripVertical, Clock, PlayCircle, PauseCircle, SkipForward,
    ChevronDown, ChevronRight, Plus, Trash2, Copy, Edit3,
    Timer, AlertTriangle, CheckCircle2, Lock, Unlock,
    FileText, Film, Mic, Image, Radio, Tv, Zap,
    ArrowUp, ArrowDown, MoreHorizontal, Target
} from 'lucide-react';
import styles from './RundownManager.module.css';

// Types
export interface RundownSegment {
    id: string;
    order: number;
    type: 'segment' | 'break' | 'live' | 'package' | 'graphics' | 'float';
    title: string;
    slug?: string;
    plannedDuration: number; // seconds
    actualDuration?: number; // actual running time if played
    status: 'pending' | 'ready' | 'on-air' | 'done' | 'skipped' | 'held';
    startTime?: number; // Unix timestamp
    endTime?: number;
    notes?: string;
    items: RundownItem[];
    isExpanded?: boolean;
    isLocked?: boolean;
    color?: string;
    backtime?: string; // Time remaining from end of show
    fronttime?: string; // Time from start of show
    cueTime?: string; // When this should go on-air
}

export interface RundownItem {
    id: string;
    type: 'video' | 'audio' | 'graphics' | 'script' | 'camera' | 'sound' | 'effect';
    title: string;
    duration: number;
    source?: string;
    notes?: string;
    status: 'ready' | 'cued' | 'on-air' | 'done';
}

export interface Rundown {
    id: string;
    name: string;
    showDate: string;
    plannedStartTime: number;
    plannedEndTime: number;
    actualStartTime?: number;
    actualEndTime?: number;
    segments: RundownSegment[];
    isLive: boolean;
    status: 'draft' | 'ready' | 'on-air' | 'done';
}

interface RundownManagerProps {
    rundown: Rundown;
    currentSegmentId?: string;
    currentTime?: number;
    onSegmentSelect?: (segmentId: string) => void;
    onSegmentPlay?: (segmentId: string) => void;
    onSegmentSkip?: (segmentId: string) => void;
    onSegmentHold?: (segmentId: string) => void;
    onSegmentEdit?: (segment: RundownSegment) => void;
    onSegmentDelete?: (segmentId: string) => void;
    onSegmentReorder?: (segmentId: string, newOrder: number) => void;
    onSegmentAdd?: (afterSegmentId?: string) => void;
    onRundownUpdate?: (rundown: Rundown) => void;
}

// Utilities
const formatDuration = (seconds: number): string => {
    const totalSeconds = Math.round(seconds);
    const h = Math.floor(totalSeconds / 3600);
    const m = Math.floor((totalSeconds % 3600) / 60);
    const s = totalSeconds % 60;
    if (h > 0) {
        return `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    }
    return `${m}:${s.toString().padStart(2, '0')}`;
};

const formatTime = (timestamp: number): string => {
    const date = new Date(timestamp);
    return date.toLocaleTimeString('en-US', { 
        hour: '2-digit', 
        minute: '2-digit', 
        second: '2-digit',
        hour12: false 
    });
};

const getSegmentIcon = (type: string) => {
    switch (type) {
        case 'segment': return <FileText size={14} />;
        case 'break': return <PauseCircle size={14} />;
        case 'live': return <Radio size={14} />;
        case 'package': return <Film size={14} />;
        case 'graphics': return <Image size={14} />;
        case 'float': return <Target size={14} />;
        default: return <FileText size={14} />;
    }
};

const getItemIcon = (type: string) => {
    switch (type) {
        case 'video': return <Film size={12} />;
        case 'audio': return <Mic size={12} />;
        case 'graphics': return <Image size={12} />;
        case 'script': return <FileText size={12} />;
        case 'camera': return <Tv size={12} />;
        case 'sound': return <Mic size={12} />;
        case 'effect': return <Zap size={12} />;
        default: return <FileText size={12} />;
    }
};

// Segment Row Component
const SegmentRow: React.FC<{
    segment: RundownSegment;
    isSelected: boolean;
    isCurrent: boolean;
    runningTime: number;
    onSelect: () => void;
    onPlay: () => void;
    onSkip: () => void;
    onHold: () => void;
    onEdit: () => void;
    onDelete: () => void;
    onToggleExpand: () => void;
    onMoveUp: () => void;
    onMoveDown: () => void;
}> = ({
    segment,
    isSelected,
    isCurrent,
    runningTime,
    onSelect,
    onPlay,
    onSkip,
    onHold,
    onEdit,
    onDelete,
    onToggleExpand,
    onMoveUp,
    onMoveDown,
}) => {
    const [showActions, setShowActions] = useState(false);

    const timingVariance = useMemo(() => {
        if (!segment.actualDuration) return 0;
        return segment.actualDuration - segment.plannedDuration;
    }, [segment.actualDuration, segment.plannedDuration]);

    return (
        <div 
            className={`${styles.segmentRow} ${styles[`status${segment.status}`]} ${isSelected ? styles.selected : ''} ${isCurrent ? styles.current : ''}`}
            onClick={onSelect}
            onMouseEnter={() => setShowActions(true)}
            onMouseLeave={() => setShowActions(false)}
        >
            {/* Order/Drag Handle */}
            <div className={styles.segmentHandle}>
                <GripVertical size={14} />
                <span className={styles.segmentOrder}>{segment.order}</span>
            </div>

            {/* Expand Toggle */}
            <button 
                className={styles.expandBtn}
                onClick={(e) => { e.stopPropagation(); onToggleExpand(); }}
            >
                {segment.isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
            </button>

            {/* Type Icon */}
            <div className={`${styles.segmentType} ${styles[`type${segment.type}`]}`}>
                {getSegmentIcon(segment.type)}
            </div>

            {/* Title & Slug */}
            <div className={styles.segmentInfo}>
                <div className={styles.segmentTitle}>
                    {segment.isLocked && <Lock size={12} className={styles.lockIcon} />}
                    {segment.title}
                </div>
                {segment.slug && (
                    <div className={styles.segmentSlug}>{segment.slug}</div>
                )}
            </div>

            {/* Timing Info */}
            <div className={styles.timingInfo}>
                {/* Cue Time */}
                <div className={styles.timingCell}>
                    <span className={styles.timingLabel}>CUE</span>
                    <span className={styles.timingValue}>{segment.cueTime || '--:--'}</span>
                </div>

                {/* Duration */}
                <div className={styles.timingCell}>
                    <span className={styles.timingLabel}>DUR</span>
                    <span className={styles.timingValue}>{formatDuration(segment.plannedDuration)}</span>
                </div>

                {/* Backtime */}
                <div className={styles.timingCell}>
                    <span className={styles.timingLabel}>BACK</span>
                    <span className={styles.timingValue}>{segment.backtime || '--:--'}</span>
                </div>

                {/* Variance */}
                {segment.actualDuration && (
                    <div className={`${styles.timingCell} ${timingVariance > 0 ? styles.overTime : styles.underTime}`}>
                        <span className={styles.timingLabel}>VAR</span>
                        <span className={styles.timingValue}>
                            {timingVariance > 0 ? '+' : ''}{formatDuration(Math.abs(timingVariance))}
                        </span>
                    </div>
                )}
            </div>

            {/* Status Badge */}
            <div className={`${styles.statusBadge} ${styles[`badge${segment.status}`]}`}>
                {segment.status === 'on-air' && <span className={styles.liveDot} />}
                {segment.status.toUpperCase()}
            </div>

            {/* Quick Actions */}
            <div className={`${styles.quickActions} ${showActions ? styles.visible : ''}`}>
                <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onPlay(); }} title="Play">
                    <PlayCircle size={16} />
                </button>
                <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onSkip(); }} title="Skip">
                    <SkipForward size={16} />
                </button>
                <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onHold(); }} title="Hold">
                    <PauseCircle size={16} />
                </button>
                <button className={styles.actionBtn} onClick={(e) => { e.stopPropagation(); onEdit(); }} title="Edit">
                    <Edit3 size={16} />
                </button>
            </div>

            {/* Expanded Items */}
            {segment.isExpanded && segment.items.length > 0 && (
                <div className={styles.segmentItems}>
                    {segment.items.map((item, idx) => (
                        <div key={item.id} className={`${styles.itemRow} ${styles[`item${item.status}`]}`}>
                            <span className={styles.itemIcon}>{getItemIcon(item.type)}</span>
                            <span className={styles.itemTitle}>{item.title}</span>
                            <span className={styles.itemDuration}>{formatDuration(item.duration)}</span>
                            <span className={`${styles.itemStatus} ${styles[item.status]}`}>
                                {item.status}
                            </span>
                        </div>
                    ))}
                </div>
            )}
        </div>
    );
};

// Main Component
export const RundownManager: React.FC<RundownManagerProps> = ({
    rundown,
    currentSegmentId,
    currentTime = Date.now(),
    onSegmentSelect,
    onSegmentPlay,
    onSegmentSkip,
    onSegmentHold,
    onSegmentEdit,
    onSegmentDelete,
    onSegmentReorder,
    onSegmentAdd,
    onRundownUpdate,
}) => {
    const [selectedSegmentId, setSelectedSegmentId] = useState<string | null>(null);
    const [expandedSegments, setExpandedSegments] = useState<Set<string>>(new Set());

    // Calculate rundown stats
    const stats = useMemo(() => {
        const totalPlanned = rundown.segments.reduce((sum, s) => sum + s.plannedDuration, 0);
        const completed = rundown.segments.filter(s => s.status === 'done').length;
        const remaining = rundown.segments.filter(s => !['done', 'skipped'].includes(s.status))
            .reduce((sum, s) => sum + s.plannedDuration, 0);
        
        return { totalPlanned, completed, remaining, total: rundown.segments.length };
    }, [rundown.segments]);

    // Calculate running time
    const runningTime = useMemo(() => {
        if (!rundown.actualStartTime) return 0;
        return Math.floor((currentTime - rundown.actualStartTime) / 1000);
    }, [rundown.actualStartTime, currentTime]);

    const handleToggleExpand = (segmentId: string) => {
        setExpandedSegments(prev => {
            const next = new Set(prev);
            if (next.has(segmentId)) {
                next.delete(segmentId);
            } else {
                next.add(segmentId);
            }
            return next;
        });
    };

    const handleSelect = (segmentId: string) => {
        setSelectedSegmentId(segmentId);
        onSegmentSelect?.(segmentId);
    };

    return (
        <div className={styles.container}>
            {/* Header */}
            <header className={styles.header}>
                <div className={styles.headerLeft}>
                    <h2 className={styles.title}>
                        <FileText size={20} />
                        {rundown.name}
                    </h2>
                    <span className={`${styles.rundownStatus} ${styles[rundown.status]}`}>
                        {rundown.status === 'on-air' && <span className={styles.liveDot} />}
                        {rundown.status.toUpperCase()}
                    </span>
                </div>

                <div className={styles.headerCenter}>
                    {/* Show Time Info */}
                    <div className={styles.showTiming}>
                        <div className={styles.timeBlock}>
                            <span className={styles.timeLabel}>Start</span>
                            <span className={styles.timeValue}>{formatTime(rundown.plannedStartTime)}</span>
                        </div>
                        <div className={styles.timeBlock}>
                            <span className={styles.timeLabel}>Running</span>
                            <span className={styles.timeValue}>{formatDuration(runningTime)}</span>
                        </div>
                        <div className={styles.timeBlock}>
                            <span className={styles.timeLabel}>Remaining</span>
                            <span className={styles.timeValue}>{formatDuration(stats.remaining)}</span>
                        </div>
                        <div className={styles.timeBlock}>
                            <span className={styles.timeLabel}>End</span>
                            <span className={styles.timeValue}>{formatTime(rundown.plannedEndTime)}</span>
                        </div>
                    </div>
                </div>

                <div className={styles.headerRight}>
                    <button className={styles.addBtn} onClick={() => onSegmentAdd?.()}>
                        <Plus size={16} />
                        Add Segment
                    </button>
                </div>
            </header>

            {/* Progress Bar */}
            <div className={styles.progressBar}>
                <div 
                    className={styles.progressFill}
                    style={{ width: `${(stats.completed / stats.total) * 100}%` }}
                />
                <span className={styles.progressLabel}>
                    {stats.completed} of {stats.total} segments complete
                </span>
            </div>

            {/* Column Headers */}
            <div className={styles.columnHeaders}>
                <div className={styles.colOrder}>#</div>
                <div className={styles.colExpand}></div>
                <div className={styles.colType}>Type</div>
                <div className={styles.colTitle}>Segment</div>
                <div className={styles.colCue}>Cue</div>
                <div className={styles.colDur}>Duration</div>
                <div className={styles.colBack}>Backtime</div>
                <div className={styles.colStatus}>Status</div>
                <div className={styles.colActions}></div>
            </div>

            {/* Segments List */}
            <div className={styles.segmentsList}>
                {rundown.segments.map((segment, index) => (
                    <SegmentRow
                        key={segment.id}
                        segment={{
                            ...segment,
                            isExpanded: expandedSegments.has(segment.id),
                        }}
                        isSelected={selectedSegmentId === segment.id}
                        isCurrent={currentSegmentId === segment.id}
                        runningTime={runningTime}
                        onSelect={() => handleSelect(segment.id)}
                        onPlay={() => onSegmentPlay?.(segment.id)}
                        onSkip={() => onSegmentSkip?.(segment.id)}
                        onHold={() => onSegmentHold?.(segment.id)}
                        onEdit={() => onSegmentEdit?.(segment)}
                        onDelete={() => onSegmentDelete?.(segment.id)}
                        onToggleExpand={() => handleToggleExpand(segment.id)}
                        onMoveUp={() => onSegmentReorder?.(segment.id, index - 1)}
                        onMoveDown={() => onSegmentReorder?.(segment.id, index + 1)}
                    />
                ))}
            </div>

            {/* Footer Stats */}
            <footer className={styles.footer}>
                <div className={styles.footerStats}>
                    <span>Total Duration: {formatDuration(stats.totalPlanned)}</span>
                    <span>•</span>
                    <span>{stats.total} Segments</span>
                    <span>•</span>
                    <span className={runningTime > stats.totalPlanned ? styles.overTime : ''}>
                        {runningTime > stats.totalPlanned ? 'OVERTIME' : 'On Schedule'}
                    </span>
                </div>
            </footer>
        </div>
    );
};

export default RundownManager;
