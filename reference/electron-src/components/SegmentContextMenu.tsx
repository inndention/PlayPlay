import React, { useState, useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';
import { MediaItem, Segment, PlaylistItem } from '../lib/types';
import { dbSaveMedia } from '../lib/mediaDbClient';
import {
    Scissors, Save, Copy, Trash2, Edit2, Film,
    ChevronRight, Palette, Clock, Tag
} from 'lucide-react';
import styles from './SegmentContextMenu.module.css';

interface SegmentContextMenuProps {
    x: number;
    y: number;
    item: PlaylistItem;
    segment?: Segment;
    onClose: () => void;
    onUpdateItem: (updates: Partial<PlaylistItem>) => void;
    onSaveSegmentAsMedia: (segment: Segment, groupColor: string) => void;
    onEditSegments: () => void;
}

const SEGMENT_COLORS = [
    '#ef4444', '#f97316', '#f59e0b', '#eab308',
    '#84cc16', '#22c55e', '#14b8a6', '#06b6d4',
    '#0ea5e9', '#3b82f6', '#6366f1', '#8b5cf6',
    '#a855f7', '#d946ef', '#ec4899', '#f43f5e',
];

export function SegmentContextMenu({
    x,
    y,
    item,
    segment,
    onClose,
    onUpdateItem,
    onSaveSegmentAsMedia,
    onEditSegments,
}: SegmentContextMenuProps) {
    const [showColorPicker, setShowColorPicker] = useState(false);
    const [showSaveDialog, setShowSaveDialog] = useState(false);
    const [saveTitle, setSaveTitle] = useState(segment?.label || '');
    const [saveColor, setSaveColor] = useState(segment?.color || '#3b82f6');
    const menuRef = useRef<HTMLDivElement>(null);

    // Close on click outside
    useEffect(() => {
        const handleClickOutside = (e: MouseEvent) => {
            if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
                onClose();
            }
        };
        document.addEventListener('mousedown', handleClickOutside);
        return () => document.removeEventListener('mousedown', handleClickOutside);
    }, [onClose]);

    // Adjust position if menu would go off screen
    useEffect(() => {
        if (menuRef.current) {
            const rect = menuRef.current.getBoundingClientRect();
            const viewport = {
                width: window.innerWidth,
                height: window.innerHeight,
            };

            if (rect.right > viewport.width) {
                menuRef.current.style.left = `${x - rect.width}px`;
            }
            if (rect.bottom > viewport.height) {
                menuRef.current.style.top = `${y - rect.height}px`;
            }
        }
    }, [x, y]);

    const handleSetTimeIn = () => {
        if (!segment) return;
        onUpdateItem({ timeIn: segment.startTime });
        onClose();
    };

    const handleSetTimeOut = () => {
        if (!segment) return;
        onUpdateItem({ timeOut: segment.endTime });
        onClose();
    };

    const handleSetSegmentColor = (color: string) => {
        if (!segment || !item.media.segments) return;

        const updatedSegments = item.media.segments.map(s =>
            s.id === segment.id ? { ...s, color } : s
        );

        onUpdateItem({
            media: {
                ...item.media,
                segments: updatedSegments,
            },
        });
        setShowColorPicker(false);
    };

    const handleRenameSegment = async () => {
        if (!segment || !item.media.segments) return;

        const { safePrompt } = await import('../lib/promptPolyfill');
        const newLabel = await safePrompt('Enter new segment name:', segment.label);
        if (!newLabel) return;

        const updatedSegments = item.media.segments.map(s =>
            s.id === segment.id ? { ...s, label: newLabel } : s
        );

        onUpdateItem({
            media: {
                ...item.media,
                segments: updatedSegments,
            },
        });
        onClose();
    };

    const handleDeleteSegment = () => {
        if (!segment || !item.media.segments) return;

        if (!confirm(`Delete segment "${segment.label}"?`)) return;

        const updatedSegments = item.media.segments.filter(s => s.id !== segment.id);

        onUpdateItem({
            media: {
                ...item.media,
                segments: updatedSegments,
            },
        });
        onClose();
    };

    const handleSaveToLibrary = () => {
        if (!segment) return;
        onSaveSegmentAsMedia(segment, saveColor);
        setShowSaveDialog(false);
        onClose();
    };

    const formatTime = (seconds: number) => {
        const m = Math.floor(seconds / 60);
        const s = Math.floor(seconds % 60);
        return `${m}:${s.toString().padStart(2, '0')}`;
    };

    return createPortal(
        <div
            ref={menuRef}
            className={styles.menu}
            style={{ left: x, top: y }}
        >
            {segment ? (
                <>
                    {/* Segment Header */}
                    <div className={styles.header}>
                        <span className={styles.segmentBadge} style={{ backgroundColor: segment.color || '#3b82f6' }}>
                            {segment.label}
                        </span>
                        <span className={styles.segmentTime}>
                            {formatTime(segment.startTime)} - {formatTime(segment.endTime)}
                        </span>
                    </div>

                    {/* Set Time In/Out */}
                    <button className={styles.menuItem} onClick={handleSetTimeIn}>
                        <Clock size={14} />
                        Set Time In ({formatTime(segment.startTime)})
                    </button>
                    <button className={styles.menuItem} onClick={handleSetTimeOut}>
                        <Clock size={14} />
                        Set Time Out ({formatTime(segment.endTime)})
                    </button>

                    <div className={styles.divider} />

                    {/* Edit Segment */}
                    <button className={styles.menuItem} onClick={handleRenameSegment}>
                        <Tag size={14} />
                        Rename Segment
                    </button>

                    {/* Color Picker */}
                    <div className={styles.menuItemWithSubmenu}>
                        <button
                            className={styles.menuItem}
                            onClick={() => setShowColorPicker(!showColorPicker)}
                        >
                            <Palette size={14} />
                            Change Color
                            <ChevronRight size={12} className={styles.chevron} />
                        </button>
                        {showColorPicker && (
                            <div className={styles.colorPicker}>
                                {SEGMENT_COLORS.map(color => (
                                    <button
                                        key={color}
                                        className={styles.colorSwatch}
                                        style={{ backgroundColor: color }}
                                        onClick={() => handleSetSegmentColor(color)}
                                    />
                                ))}
                            </div>
                        )}
                    </div>

                    <div className={styles.divider} />

                    {/* Save to Library */}
                    <button
                        className={styles.menuItem}
                        onClick={() => setShowSaveDialog(true)}
                    >
                        <Save size={14} />
                        Save as New Media
                    </button>

                    {showSaveDialog && (
                        <div className={styles.saveDialog}>
                            <input
                                type="text"
                                value={saveTitle}
                                onChange={e => setSaveTitle(e.target.value)}
                                placeholder="Title for new media"
                                className={styles.saveInput}
                                autoFocus
                            />
                            <div className={styles.saveColorRow}>
                                <span>Group Color:</span>
                                <div className={styles.colorPickerInline}>
                                    {SEGMENT_COLORS.slice(0, 8).map(color => (
                                        <button
                                            key={color}
                                            className={`${styles.colorSwatchSmall} ${saveColor === color ? styles.selected : ''}`}
                                            style={{ backgroundColor: color }}
                                            onClick={() => setSaveColor(color)}
                                        />
                                    ))}
                                </div>
                            </div>
                            <button className={styles.saveBtn} onClick={handleSaveToLibrary}>
                                <Save size={14} />
                                Save to Library
                            </button>
                        </div>
                    )}

                    <div className={styles.divider} />

                    <button className={`${styles.menuItem} ${styles.danger}`} onClick={handleDeleteSegment}>
                        <Trash2 size={14} />
                        Delete Segment
                    </button>
                </>
            ) : (
                <>
                    {/* No segment selected - show general options */}
                    <div className={styles.header}>
                        <Film size={14} />
                        <span>{item.media.title}</span>
                    </div>

                    <button className={styles.menuItem} onClick={onEditSegments}>
                        <Scissors size={14} />
                        Edit Segments
                    </button>

                    {item.media.segments && item.media.segments.length > 0 && (
                        <>
                            <div className={styles.divider} />
                            <div className={styles.segmentList}>
                                <span className={styles.segmentListHeader}>Segments:</span>
                                {item.media.segments.map(seg => (
                                    <div key={seg.id} className={styles.segmentListItem}>
                                        <span
                                            className={styles.segmentDot}
                                            style={{ backgroundColor: seg.color || '#3b82f6' }}
                                        />
                                        <span>{seg.label}</span>
                                        <span className={styles.segmentDuration}>
                                            {formatTime(seg.endTime - seg.startTime)}
                                        </span>
                                    </div>
                                ))}
                            </div>
                        </>
                    )}
                </>
            )}
        </div>,
        document.body
    );
}

// Hook for managing segment context menu
export function useSegmentContextMenu() {
    const [menuState, setMenuState] = useState<{
        visible: boolean;
        x: number;
        y: number;
        item: PlaylistItem | null;
        segment: Segment | undefined;
    }>({
        visible: false,
        x: 0,
        y: 0,
        item: null,
        segment: undefined,
    });

    const showMenu = (e: React.MouseEvent, item: PlaylistItem, segment?: Segment) => {
        e.preventDefault();
        e.stopPropagation();
        setMenuState({
            visible: true,
            x: e.clientX,
            y: e.clientY,
            item,
            segment,
        });
    };

    const hideMenu = () => {
        setMenuState(prev => ({ ...prev, visible: false }));
    };

    return { menuState, showMenu, hideMenu };
}

