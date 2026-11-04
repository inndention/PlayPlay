import React, { useState, useRef, useEffect, useCallback } from 'react';
import { HourSlot, PlaylistItem, MediaItem, MediaType, OBSCommand, CGElement, OBSSession, Segment } from '../lib/types';
import type { AutomationAction } from '../lib/types';
import { MediaLibrary } from './MediaLibrary';
import { CGBuilder } from './CGBuilder';
import { AutoFillPanel } from './AutoFillPanel';
import { SegmentContextMenu, useSegmentContextMenu } from './SegmentContextMenu';
import { SegmentEditor } from './SegmentEditor';
import { loadOBSCommands, loadCGElements, saveCGElements } from '../lib/storage';
import { safePrompt } from '../lib/promptPolyfill';
import { dbSaveMedia } from '../lib/mediaDbClient';
import { formatDuration, cn } from '../lib/utils';
import { 
    X, Plus, Trash2, Edit2, Copy, MoveUp, MoveDown, 
    Film, Radio, Layers, Eye, EyeOff, Play, Pause,
    Clock, Calendar, ChevronDown, ChevronRight, Wand2, Scissors
} from 'lucide-react';
import styles from './HourSlotEditor.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

interface HourSlotEditorProps {
    slot: HourSlot;
    obsSessions: OBSSession[];
    onSave: (updatedSlot: HourSlot) => void;
    onClose: () => void;
}

type MediaBucket = {
    type: MediaType | 'all';
    items: PlaylistItem[];
};

export function HourSlotEditor({ slot, obsSessions, onSave, onClose }: HourSlotEditorProps) {
    const [editedSlot, setEditedSlot] = useState<HourSlot>(() => ({
        ...slot,
        items: Array.isArray(slot.items) ? slot.items : [],
    }));
    const [selectedItem, setSelectedItem] = useState<PlaylistItem | null>(null);
    const [obsCommands] = useState<OBSCommand[]>(() => loadOBSCommands());
    const [cgElements, setCGElements] = useState<CGElement[]>(() => loadCGElements());
    const [editingCG, setEditingCG] = useState<CGElement | null>(null);
    const [expandedBuckets, setExpandedBuckets] = useState<Set<MediaType | 'all'>>(new Set(['all']));
    const [showMediaLibrary, setShowMediaLibrary] = useState(true);
    const [showAutoFill, setShowAutoFill] = useState(false);
    const [editingSegmentsItem, setEditingSegmentsItem] = useState<PlaylistItem | null>(null);
    
    // Segment context menu
    const { menuState, showMenu: showSegmentMenu, hideMenu: hideSegmentMenu } = useSegmentContextMenu();

    // Save CG elements when changed
    useEffect(() => {
        saveCGElements(cgElements);
    }, [cgElements]);

    // Organize items by media type
    const organizeByType = useCallback((): MediaBucket[] => {
        const buckets: MediaBucket[] = [
            { type: 'all', items: editedSlot.items },
            { type: 'program', items: editedSlot.items.filter(i => i.media.type === 'program' || i.media.type === 'paid_program') },
            { type: 'ad', items: editedSlot.items.filter(i => i.media.type === 'ad') },
            { type: 'station_id', items: editedSlot.items.filter(i => i.media.type === 'station_id') },
            { type: 'bumper', items: editedSlot.items.filter(i => i.media.type === 'bumper') },
            { type: 'promo', items: editedSlot.items.filter(i => i.media.type === 'promo') },
        ];
        return buckets.filter(b => b.items.length > 0 || b.type === 'all');
    }, [editedSlot.items]);

    const buckets = organizeByType();

    const handleAddMedia = (media: MediaItem) => {
        const newItem: PlaylistItem = {
            id: generateId(),
            mediaId: media.id,
            media: media,
            isResolved: true,
        };
        setEditedSlot(prev => ({
            ...prev,
            items: [...prev.items, newItem],
        }));
    };

    const handleRemoveItem = (id: string) => {
        setEditedSlot(prev => ({
            ...prev,
            items: prev.items.filter(i => i.id !== id),
        }));
        if (selectedItem?.id === id) {
            setSelectedItem(null);
        }
    };

    const handleMoveItem = (id: string, direction: 'up' | 'down') => {
        const index = editedSlot.items.findIndex(i => i.id === id);
        if (index === -1) return;
        const newIndex = direction === 'up' ? index - 1 : index + 1;
        if (newIndex < 0 || newIndex >= editedSlot.items.length) return;

        const newItems = [...editedSlot.items];
        [newItems[index], newItems[newIndex]] = [newItems[newIndex], newItems[index]];
        setEditedSlot(prev => ({ ...prev, items: newItems }));
    };

    const handleDuplicateItem = (item: PlaylistItem) => {
        const newItem: PlaylistItem = {
            ...item,
            id: generateId(),
        };
        const index = editedSlot.items.findIndex(i => i.id === item.id);
        const newItems = [...editedSlot.items];
        newItems.splice(index + 1, 0, newItem);
        setEditedSlot(prev => ({ ...prev, items: newItems }));
    };

    const handleUpdateItem = (id: string, updates: Partial<PlaylistItem>) => {
        setEditedSlot(prev => ({
            ...prev,
            items: prev.items.map(i => i.id === id ? { ...i, ...updates } : i),
        }));
        if (selectedItem?.id === id) {
            setSelectedItem(prev => prev ? { ...prev, ...updates } : null);
        }
    };

    // Save segment as new media item in library
    const handleSaveSegmentAsMedia = async (segment: Segment, groupColor: string, item: PlaylistItem) => {
        const newMedia: MediaItem = {
            id: `segment_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,
            title: segment.label,
            type: item.media.type,
            duration: segment.endTime - segment.startTime,
            src: item.media.src,
            filePath: item.media.filePath,
            filename: item.media.filename,
            thumbnail: groupColor, // Use color as thumbnail indicator
            segments: [],
            addedAt: new Date().toISOString(),
            dateMade: new Date().toISOString(),
            priority: 0,
            playCount: 0,
            programSeries: item.media.programSeries,
            notes: `Segment from "${item.media.title}" (${formatDuration(segment.startTime)} - ${formatDuration(segment.endTime)})`,
        };

        // Save to DB
        await dbSaveMedia(newMedia);
        
        // Optionally add to slot with time in/out set
        const newItem: PlaylistItem = {
            id: generateId(),
            mediaId: newMedia.id,
            media: newMedia,
            isResolved: true,
            timeIn: segment.startTime,
            timeOut: segment.endTime,
        };
        
        // Insert after the source item
        const sourceIndex = editedSlot.items.findIndex(i => i.id === item.id);
        const newItems = [...editedSlot.items];
        newItems.splice(sourceIndex + 1, 0, newItem);
        setEditedSlot(prev => ({ ...prev, items: newItems }));
    };

    const handleAddOBSCommand = (itemId: string, commandId: string) => {
        const item = editedSlot.items.find(i => i.id === itemId);
        if (!item) return;
        
        const obsCommands = item.obsCommands || [];
        if (!obsCommands.includes(commandId)) {
            handleUpdateItem(itemId, { obsCommands: [...obsCommands, commandId] });
        }
    };

    const handleRemoveOBSCommand = (itemId: string, commandId: string) => {
        const item = editedSlot.items.find(i => i.id === itemId);
        if (!item) return;
        const obsCommands = (item.obsCommands || []).filter(id => id !== commandId);
        handleUpdateItem(itemId, { obsCommands });
    };

    const handleAddFacebookPost = async (itemId: string) => {
        const message = await safePrompt('Facebook Post: message text', '');
        if (!message) return;
        const link = (await safePrompt('Facebook Post: optional link URL', '')) || '';
        const action: AutomationAction = {
            id: `fb_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
            type: 'facebook_post',
            createdAt: Date.now(),
            enabled: true,
            message,
            link: link.trim() || undefined,
        };
        const item = slot.items.find(i => i.id === itemId);
        const actions = item?.actions || [];
        handleUpdateItem(itemId, { actions: [...actions, action] as any });
    };

    const handleAddFacebookLiveStart = async (itemId: string) => {
        const title = (await safePrompt('Facebook Live: optional title', '')) || '';
        const action: AutomationAction = {
            id: `fblive_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
            type: 'facebook_live_start',
            createdAt: Date.now(),
            enabled: true,
            message: title.trim() || undefined,
        };
        const item = slot.items.find(i => i.id === itemId);
        const actions = item?.actions || [];
        handleUpdateItem(itemId, { actions: [...actions, action] as any });
    };

    const handleAddFacebookLiveStop = (itemId: string) => {
        const action: AutomationAction = {
            id: `fblive_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
            type: 'facebook_live_stop',
            createdAt: Date.now(),
            enabled: true,
        };
        const item = slot.items.find(i => i.id === itemId);
        const actions = item?.actions || [];
        handleUpdateItem(itemId, { actions: [...actions, action] as any });
    };

    const handleRemoveAction = (itemId: string, actionId: string) => {
        const item = slot.items.find(i => i.id === itemId);
        const next = (item?.actions || []).filter(a => a.id !== actionId);
        handleUpdateItem(itemId, { actions: next as any });
    };

    // Expose tiny hooks for ItemDetailsPanel buttons (keeps props churn low)
    useEffect(() => {
        (window as any).__ppAddFacebookPost = handleAddFacebookPost;
        (window as any).__ppAddFacebookLiveStart = handleAddFacebookLiveStart;
        (window as any).__ppAddFacebookLiveStop = handleAddFacebookLiveStop;
        (window as any).__ppRemoveAction = handleRemoveAction;
        return () => {
            delete (window as any).__ppAddFacebookPost;
            delete (window as any).__ppAddFacebookLiveStart;
            delete (window as any).__ppAddFacebookLiveStop;
            delete (window as any).__ppRemoveAction;
        };
    }, [editedSlot.items]);

    const handleToggleCG = (itemId: string, cgId: string, type: 'onStart' | 'onEnd') => {
        const item = editedSlot.items.find(i => i.id === itemId);
        if (!item) return;
        
        const cgList = type === 'onStart' ? (item.cgOnStart || []) : (item.cgOnEnd || []);
        const newList = cgList.includes(cgId)
            ? cgList.filter(id => id !== cgId)
            : [...cgList, cgId];
        
        handleUpdateItem(itemId, {
            [type === 'onStart' ? 'cgOnStart' : 'cgOnEnd']: newList,
        });
    };

    const handleSave = () => {
        console.log('HourSlotEditor: Saving slot:', editedSlot.id);
        onSave(editedSlot);
        // Event will be dispatched by WeeklyScheduler after save
        onClose();
    };

    const handleDragOver = (e: React.DragEvent, bucketType: MediaType | 'all') => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
    };

    const handleDrop = (e: React.DragEvent, bucketType: MediaType | 'all') => {
        e.preventDefault();
        try {
            const data = e.dataTransfer.getData('application/json');
            if (!data) return;
            const media: MediaItem = JSON.parse(data);
            handleAddMedia(media);
        } catch (err) {
            console.error('Failed to parse dropped media:', err);
        }
    };

    const toggleBucket = (type: MediaType | 'all') => {
        setExpandedBuckets(prev => {
            const next = new Set(prev);
            if (next.has(type)) {
                next.delete(type);
            } else {
                next.add(type);
            }
            return next;
        });
    };

    const totalDuration = editedSlot.items.reduce((sum, item) => sum + item.media.duration, 0);
    const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

    return (
        <div className={styles.overlay} onClick={onClose}>
            <div className={styles.modal} onClick={(e) => e.stopPropagation()}>
                <div className={styles.header}>
                    <div className={styles.headerLeft}>
                        <h2 className={styles.title}>
                            {slot.dayOfWeek !== undefined ? DAYS[slot.dayOfWeek] : ''} {slot.hour.toString().padStart(2, '0')}:00 Editor
                        </h2>
                        <div className={styles.meta}>
                            <span>{editedSlot.items.length} items</span>
                            <span>•</span>
                            <span>{formatDuration(totalDuration)}</span>
                            {totalDuration > 3600 && <span className={styles.overWarning}> (OVER)</span>}
                        </div>
                    </div>
                    <div className={styles.headerRight}>
                        <button 
                            className={cn(styles.toggleBtn, showAutoFill && styles.active)}
                            onClick={() => setShowAutoFill(!showAutoFill)}
                        >
                            <Wand2 size={14} />
                            Auto-Fill
                        </button>
                        <button className={styles.toggleBtn} onClick={() => setShowMediaLibrary(!showMediaLibrary)}>
                            {showMediaLibrary ? 'Hide' : 'Show'} Library
                        </button>
                        <button className={styles.closeBtn} onClick={onClose}>
                            <X size={18} />
                        </button>
                    </div>
                </div>

                <div className={styles.content}>
                    {showMediaLibrary && (
                        <aside className={styles.libraryPanel}>
                            <MediaLibrary onItemClick={handleAddMedia} preferredViewMode="list" />
                        </aside>
                    )}
                    
                    {showAutoFill && (
                        <aside className={styles.autoFillPanel}>
                            <AutoFillPanel 
                                slot={editedSlot}
                                onUpdateSlot={setEditedSlot}
                            />
                        </aside>
                    )}

                    <main className={styles.mainPanel}>
                        {/* Slot Options */}
                        <div className={styles.optionsBar}>
                            <label className={styles.checkboxLabel}>
                                <input
                                    type="checkbox"
                                    checked={editedSlot.isLooping}
                                    onChange={(e) => setEditedSlot(prev => ({ ...prev, isLooping: e.target.checked }))}
                                />
                                <span>Loop Hour</span>
                            </label>
                            <label className={styles.checkboxLabel}>
                                <input
                                    type="checkbox"
                                    checked={editedSlot.randomizeAds}
                                    onChange={(e) => setEditedSlot(prev => ({ ...prev, randomizeAds: e.target.checked }))}
                                />
                                <span>Randomize Ads</span>
                            </label>
                            <label className={styles.checkboxLabel}>
                                <input
                                    type="checkbox"
                                    checked={editedSlot.keepProgramOrder}
                                    onChange={(e) => setEditedSlot(prev => ({ ...prev, keepProgramOrder: e.target.checked }))}
                                />
                                <span>Keep Program Order</span>
                            </label>
                        </div>

                        {/* Visual Timeline Representation */}
                        <div className={styles.visualTimeline}>
                            <div className={styles.timelineHeader}>
                                <Clock size={14} />
                                <span>Hour Timeline</span>
                                <span className={styles.timelineStats}>
                                    {formatDuration(totalDuration)} / 60:00
                                    {totalDuration < 3600 && (
                                        <span className={styles.timelineGap}> ({formatDuration(3600 - totalDuration)} gap)</span>
                                    )}
                                </span>
                            </div>
                            <div className={styles.timelineTrackWrapper}>
                                <div className={styles.timelineTrack}>
                                    {editedSlot.items.map((item, idx) => {
                                        const widthPercent = Math.max(0.5, (item.media.duration / 3600) * 100);
                                        const typeColors: Record<string, string> = {
                                            program: '#3b82f6',
                                            paid_program: '#8b5cf6',
                                            ad: '#ef4444',
                                            station_id: '#10b981',
                                            bumper: '#f59e0b',
                                            promo: '#ec4899',
                                            video: '#6366f1',
                                            image: '#14b8a6',
                                            live: '#f97316',
                                            gap: '#374151'
                                        };
                                        return (
                                            <div
                                                key={item.id}
                                                className={`${styles.timelineItem} ${selectedItem?.id === item.id ? styles.timelineItemSelected : ''}`}
                                                style={{
                                                    width: `${widthPercent}%`,
                                                    backgroundColor: typeColors[item.media.type] || '#6b7280',
                                                }}
                                                onClick={() => setSelectedItem(item)}
                                                title={`${item.media.title}\n${formatDuration(item.media.duration)} - ${item.media.type}`}
                                            >
                                                {widthPercent > 5 && (
                                                    <span className={styles.timelineItemLabel}>
                                                        {item.media.title?.slice(0, 15)}
                                                    </span>
                                                )}
                                            </div>
                                        );
                                    })}
                                    {/* Fill gap if under hour */}
                                    {totalDuration < 3600 && (
                                        <div
                                            className={styles.timelineGapBlock}
                                            style={{ width: `${((3600 - totalDuration) / 3600) * 100}%` }}
                                        >
                                            <span>Gap</span>
                                        </div>
                                    )}
                                </div>
                                {/* Time markers */}
                                <div className={styles.timeMarkers}>
                                    <span>:00</span>
                                    <span>:15</span>
                                    <span>:30</span>
                                    <span>:45</span>
                                    <span>:60</span>
                                </div>
                            </div>
                        </div>

                        {/* Media Type Buckets */}
                        <div className={styles.bucketsContainer}>
                            {buckets.map(bucket => {
                                const isExpanded = expandedBuckets.has(bucket.type);
                                const typeLabel = bucket.type === 'all' ? 'All Items' : bucket.type.replace('_', ' ').toUpperCase();
                                
                                return (
                                    <div key={bucket.type} className={styles.bucket}>
                                        <div 
                                            className={styles.bucketHeader}
                                            onClick={() => toggleBucket(bucket.type)}
                                        >
                                            {isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                                            <span className={styles.bucketTitle}>{typeLabel}</span>
                                            <span className={styles.bucketCount}>({bucket.items.length})</span>
                                        </div>
                                        
                                        {isExpanded && (
                                            <div
                                                className={styles.bucketContent}
                                                onDragOver={(e) => handleDragOver(e, bucket.type)}
                                                onDrop={(e) => handleDrop(e, bucket.type)}
                                            >
                                                {bucket.items.length === 0 ? (
                                                    <div className={styles.emptyBucket}>
                                                        Drop media here or click from library
                                                    </div>
                                                ) : (
                                                    bucket.items.map(item => (
                                                        <PlaylistItemCard
                                                            key={item.id}
                                                            item={item}
                                                            isSelected={selectedItem?.id === item.id}
                                                            cgElements={cgElements}
                                                            obsCommands={obsCommands}
                                                            onSelect={() => setSelectedItem(item)}
                                                            onRemove={() => handleRemoveItem(item.id)}
                                                            onMoveUp={() => handleMoveItem(item.id, 'up')}
                                                            onMoveDown={() => handleMoveItem(item.id, 'down')}
                                                            onDuplicate={() => handleDuplicateItem(item)}
                                                            onToggleCG={handleToggleCG}
                                                            onAddOBSCommand={handleAddOBSCommand}
                                                            onRemoveOBSCommand={handleRemoveOBSCommand}
                                                            onEditCG={(cg) => setEditingCG(cg)}
                                                            onContextMenu={(e, segment) => showSegmentMenu(e, item, segment)}
                                                            onEditSegments={() => setEditingSegmentsItem(item)}
                                                        />
                                                    ))
                                                )}
                                            </div>
                                        )}
                                    </div>
                                );
                            })}
                        </div>

                        {/* Item Details Panel */}
                        {selectedItem && (
                            <div className={styles.detailsPanel}>
                                <div className={styles.detailsHeader}>
                                    <h3>Item Details</h3>
                                    <button onClick={() => setSelectedItem(null)}><X size={14} /></button>
                                </div>
                                <ItemDetailsPanel
                                    item={selectedItem}
                                    cgElements={cgElements}
                                    obsCommands={obsCommands}
                                    obsSessions={obsSessions}
                                    onUpdate={(updates) => handleUpdateItem(selectedItem.id, updates)}
                                    onToggleCG={handleToggleCG}
                                    onAddOBSCommand={handleAddOBSCommand}
                                    onRemoveOBSCommand={handleRemoveOBSCommand}
                                    onEditCG={(cg) => setEditingCG(cg)}
                                />
                            </div>
                        )}
                    </main>
                </div>

                <div className={styles.footer}>
                    <button className={styles.cancelBtn} onClick={onClose}>Cancel</button>
                    <button className={styles.saveBtn} onClick={handleSave}>Save Changes</button>
                </div>
            </div>

            {/* CG Editor Modal */}
            {editingCG && (
                <div className={styles.cgEditorOverlay} onClick={() => setEditingCG(null)}>
                    <div className={styles.cgEditorModal} onClick={(e) => e.stopPropagation()}>
                        <CGBuilder
                            element={editingCG}
                            onSave={(updated) => {
                                setCGElements(prev => prev.map(e => e.id === updated.id ? updated : e));
                                setEditingCG(null);
                            }}
                            onCancel={() => setEditingCG(null)}
                        />
                    </div>
                </div>
            )}

            {/* Segment Context Menu */}
            {menuState.visible && menuState.item && (
                <SegmentContextMenu
                    x={menuState.x}
                    y={menuState.y}
                    item={menuState.item}
                    segment={menuState.segment}
                    onClose={hideSegmentMenu}
                    onUpdateItem={(updates) => handleUpdateItem(menuState.item!.id, updates)}
                    onSaveSegmentAsMedia={(segment, color) => handleSaveSegmentAsMedia(segment, color, menuState.item!)}
                    onEditSegments={() => {
                        setEditingSegmentsItem(menuState.item);
                        hideSegmentMenu();
                    }}
                />
            )}

            {/* Segment Editor Modal */}
            {editingSegmentsItem && (
                <div className={styles.cgEditorOverlay} onClick={() => setEditingSegmentsItem(null)}>
                    <div className={styles.cgEditorModal} onClick={(e) => e.stopPropagation()}>
                        <SegmentEditor
                            item={editingSegmentsItem.media}
                            onSave={async (collections, activeCollectionId) => {
                                const activeSegments = collections.find(c => c.isActive)?.segments || collections[0]?.segments || [];
                                handleUpdateItem(editingSegmentsItem.id, {
                                    media: {
                                        ...editingSegmentsItem.media,
                                        segmentCollections: collections,
                                        activeCollectionId: activeCollectionId,
                                        segments: activeSegments, // Legacy support
                                    },
                                });
                                setEditingSegmentsItem(null);
                            }}
                            onClose={() => setEditingSegmentsItem(null)}
                        />
                    </div>
                </div>
            )}
        </div>
    );
}

function PlaylistItemCard({
    item,
    isSelected,
    cgElements,
    obsCommands,
    onSelect,
    onRemove,
    onMoveUp,
    onMoveDown,
    onDuplicate,
    onToggleCG,
    onAddOBSCommand,
    onRemoveOBSCommand,
    onEditCG,
    onContextMenu,
    onEditSegments,
}: {
    item: PlaylistItem;
    isSelected: boolean;
    cgElements: CGElement[];
    obsCommands: OBSCommand[];
    onSelect: () => void;
    onRemove: () => void;
    onMoveUp: () => void;
    onMoveDown: () => void;
    onDuplicate: () => void;
    onToggleCG: (itemId: string, cgId: string, type: 'onStart' | 'onEnd') => void;
    onAddOBSCommand: (itemId: string, commandId: string) => void;
    onRemoveOBSCommand: (itemId: string, commandId: string) => void;
    onEditCG: (cg: CGElement) => void;
    onContextMenu?: (e: React.MouseEvent, segment?: Segment) => void;
    onEditSegments?: () => void;
}) {
    const cgOnStart = item.cgOnStart || [];
    const cgOnEnd = item.cgOnEnd || [];
    const itemObsCommands = item.obsCommands || [];
    const segments = item.media.segmentCollections?.find(c => c.isActive)?.segments
        ?? item.media.segmentCollections?.[0]?.segments
        ?? item.media.segments
        ?? [];

    return (
        <div 
            className={cn(styles.itemCard, isSelected && styles.itemCardSelected)} 
            onClick={onSelect}
            onContextMenu={(e) => onContextMenu?.(e)}
        >
            <div className={styles.itemCardHeader}>
                <div className={styles.itemCardTitle}>
                    <Film size={14} />
                    <span>{item.media.title}</span>
                    <span className={styles.itemDuration}>{formatDuration(item.media.duration)}</span>
                </div>
                <div className={styles.itemCardActions}>
                    {onEditSegments && (
                        <button onClick={(e) => { e.stopPropagation(); onEditSegments(); }} title="Edit Segments">
                            <Scissors size={12} />
                        </button>
                    )}
                    <button onClick={(e) => { e.stopPropagation(); onMoveUp(); }} title="Move Up">
                        <MoveUp size={12} />
                    </button>
                    <button onClick={(e) => { e.stopPropagation(); onMoveDown(); }} title="Move Down">
                        <MoveDown size={12} />
                    </button>
                    <button onClick={(e) => { e.stopPropagation(); onDuplicate(); }} title="Duplicate">
                        <Copy size={12} />
                    </button>
                    <button onClick={(e) => { e.stopPropagation(); onRemove(); }} title="Remove">
                        <Trash2 size={12} />
                    </button>
                </div>
            </div>
            
            {/* Segment indicators */}
            {segments.length > 0 && (
                <div className={styles.segmentBar}>
                    {segments.map(seg => {
                        const width = ((seg.endTime - seg.startTime) / item.media.duration) * 100;
                        const left = (seg.startTime / item.media.duration) * 100;
                        return (
                            <div
                                key={seg.id}
                                className={styles.segmentIndicator}
                                style={{
                                    left: `${left}%`,
                                    width: `${width}%`,
                                    backgroundColor: seg.color || '#3b82f6',
                                }}
                                title={`${seg.label} (${formatDuration(seg.startTime)} - ${formatDuration(seg.endTime)})`}
                                onContextMenu={(e) => {
                                    e.stopPropagation();
                                    onContextMenu?.(e, seg);
                                }}
                            />
                        );
                    })}
                </div>
            )}
            
            <div className={styles.itemCardMeta}>
                <span className={styles.typeBadge}>{item.media.type}</span>
                {segments.length > 0 && (
                    <span className={styles.segmentBadge}>
                        <Scissors size={10} /> {segments.length}
                    </span>
                )}
                {(cgOnStart.length > 0 || cgOnEnd.length > 0) && (
                    <span className={styles.cgBadge}>
                        <Layers size={10} /> CG
                    </span>
                )}
                {itemObsCommands.length > 0 && (
                    <span className={styles.obsBadge}>
                        <Radio size={10} /> OBS
                    </span>
                )}
            </div>
        </div>
    );
}

function ItemDetailsPanel({
    item,
    cgElements,
    obsCommands,
    obsSessions,
    onUpdate,
    onToggleCG,
    onAddOBSCommand,
    onRemoveOBSCommand,
    onEditCG,
}: {
    item: PlaylistItem;
    cgElements: CGElement[];
    obsCommands: OBSCommand[];
    obsSessions: OBSSession[];
    onUpdate: (updates: Partial<PlaylistItem>) => void;
    onToggleCG: (itemId: string, cgId: string, type: 'onStart' | 'onEnd') => void;
    onAddOBSCommand: (itemId: string, commandId: string) => void;
    onRemoveOBSCommand: (itemId: string, commandId: string) => void;
    onEditCG: (cg: CGElement) => void;
}) {
    const cgOnStart = item.cgOnStart || [];
    const cgOnEnd = item.cgOnEnd || [];
    const itemObsCommands = item.obsCommands || [];
    const itemActions = item.actions || [];

    return (
        <div className={styles.detailsContent}>
            <div className={styles.detailsSection}>
                <h4>Media Info</h4>
                <div className={styles.infoRow}>
                    <span>Title:</span>
                    <span>{item.media.title}</span>
                </div>
                <div className={styles.infoRow}>
                    <span>Type:</span>
                    <span>{item.media.type}</span>
                </div>
                <div className={styles.infoRow}>
                    <span>Duration:</span>
                    <span>{formatDuration(item.media.duration)}</span>
                </div>
            </div>

            <div className={styles.detailsSection}>
                <h4>CG On Start</h4>
                <div className={styles.cgList}>
                    {cgElements.map(cg => {
                        const isActive = cgOnStart.includes(cg.id);
                        return (
                            <div key={cg.id} className={styles.cgItem}>
                                <label className={styles.cgCheckbox}>
                                    <input
                                        type="checkbox"
                                        checked={isActive}
                                        onChange={() => onToggleCG(item.id, cg.id, 'onStart')}
                                    />
                                    <span>{cg.name}</span>
                                    <button onClick={() => onEditCG(cg)} className={styles.editCGBtn}>
                                        <Edit2 size={12} />
                                    </button>
                                </label>
                            </div>
                        );
                    })}
                    {cgElements.length === 0 && (
                        <div className={styles.emptyState}>No CG elements available</div>
                    )}
                </div>
            </div>

            <div className={styles.detailsSection}>
                <h4>CG On End</h4>
                <div className={styles.cgList}>
                    {cgElements.map(cg => {
                        const isActive = cgOnEnd.includes(cg.id);
                        return (
                            <div key={cg.id} className={styles.cgItem}>
                                <label className={styles.cgCheckbox}>
                                    <input
                                        type="checkbox"
                                        checked={isActive}
                                        onChange={() => onToggleCG(item.id, cg.id, 'onEnd')}
                                    />
                                    <span>{cg.name}</span>
                                    <button onClick={() => onEditCG(cg)} className={styles.editCGBtn}>
                                        <Edit2 size={12} />
                                    </button>
                                </label>
                            </div>
                        );
                    })}
                    {cgElements.length === 0 && (
                        <div className={styles.emptyState}>No CG elements available</div>
                    )}
                </div>
            </div>

            <div className={styles.detailsSection}>
                <h4>OBS Commands</h4>
                <div className={styles.obsList}>
                    {obsCommands.map(cmd => {
                        const isActive = itemObsCommands.includes(cmd.id);
                        return (
                            <div key={cmd.id} className={styles.obsItem}>
                                <label className={styles.obsCheckbox}>
                                    <input
                                        type="checkbox"
                                        checked={isActive}
                                        onChange={() => {
                                            if (isActive) {
                                                onRemoveOBSCommand(item.id, cmd.id);
                                            } else {
                                                onAddOBSCommand(item.id, cmd.id);
                                            }
                                        }}
                                    />
                                    <span>{cmd.label}</span>
                                </label>
                            </div>
                        );
                    })}
                    {obsCommands.length === 0 && (
                        <div className={styles.emptyState}>No OBS commands available</div>
                    )}
                </div>
            </div>

            <div className={styles.detailsSection}>
                <h4>Facebook Actions</h4>
                <div className={styles.obsList}>
                    <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 8 }}>
                        <button
                            className={styles.smallBtn}
                            onClick={() => (window as any).__ppAddFacebookPost?.(item.id)}
                            type="button"
                        >
                            + Post
                        </button>
                        <button
                            className={styles.smallBtn}
                            onClick={() => (window as any).__ppAddFacebookLiveStart?.(item.id)}
                            type="button"
                        >
                            + Live Start
                        </button>
                        <button
                            className={styles.smallBtn}
                            onClick={() => (window as any).__ppAddFacebookLiveStop?.(item.id)}
                            type="button"
                        >
                            + Live Stop
                        </button>
                    </div>

                    {itemActions.filter(a => a.type.startsWith('facebook_')).map(a => (
                        <div key={a.id} className={styles.obsItem}>
                            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8, width: '100%' }}>
                                <span>
                                    {a.type === 'facebook_post'
                                        ? `Post: ${(a.message || '').slice(0, 60)}`
                                        : a.type === 'facebook_live_start'
                                            ? `Live Start${a.message ? `: ${a.message}` : ''}`
                                            : 'Live Stop'}
                                </span>
                                <button
                                    className={styles.smallBtn}
                                    onClick={() => (window as any).__ppRemoveAction?.(item.id, a.id)}
                                    type="button"
                                >
                                    Remove
                                </button>
                            </div>
                        </div>
                    ))}

                    {itemActions.filter(a => a.type.startsWith('facebook_')).length === 0 && (
                        <div className={styles.emptyState}>No Facebook actions</div>
                    )}
                </div>
            </div>
        </div>
    );
}
