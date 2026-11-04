import React, { useState, useEffect, useMemo, useCallback } from 'react';
import styles from './SmartCollections.module.css';
import { MediaItem, MediaType } from '../lib/types';
import { formatDuration } from '../lib/utils';
import {
    Folder, FolderPlus, Star, Clock, Calendar, Film, Music, Image as ImageIcon,
    Tag, Filter, Zap, Trash2, Edit2, ChevronRight, ChevronDown, Play, Plus, X,
    Save, Check, Eye, Search, Sparkles, TrendingUp, AlertCircle, RefreshCw
} from 'lucide-react';

// Smart Collection Rule Types
type RuleField = 'type' | 'duration' | 'title' | 'client' | 'addedAt' | 'playCount' | 'priority' | 'series' | 'archived' | 'hasSegments';
type RuleOperator = 'equals' | 'notEquals' | 'contains' | 'notContains' | 'greaterThan' | 'lessThan' | 'between' | 'isEmpty' | 'isNotEmpty';

interface CollectionRule {
    id: string;
    field: RuleField;
    operator: RuleOperator;
    value: string | number | boolean;
    value2?: string | number; // For 'between' operator
}

interface SmartCollection {
    id: string;
    name: string;
    icon: string;
    color: string;
    rules: CollectionRule[];
    matchAll: boolean; // true = AND, false = OR
    sortBy: 'addedAt' | 'title' | 'duration' | 'playCount';
    sortOrder: 'asc' | 'desc';
    createdAt: string;
    updatedAt: string;
    isPinned?: boolean;
    isBuiltIn?: boolean;
}

interface SmartCollectionsProps {
    allMedia: MediaItem[];
    onSelectCollection?: (collection: SmartCollection, items: MediaItem[]) => void;
    onMediaClick?: (item: MediaItem) => void;
}

// Built-in collections
const BUILT_IN_COLLECTIONS: SmartCollection[] = [
    {
        id: 'recently-added',
        name: 'Recently Added',
        icon: 'clock',
        color: '#3b82f6',
        rules: [],
        matchAll: true,
        sortBy: 'addedAt',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true,
        isPinned: true
    },
    {
        id: 'favorites',
        name: 'Favorites',
        icon: 'star',
        color: '#f59e0b',
        rules: [{ id: '1', field: 'priority', operator: 'greaterThan', value: 0 }],
        matchAll: true,
        sortBy: 'addedAt',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true,
        isPinned: true
    },
    {
        id: 'most-played',
        name: 'Most Played',
        icon: 'trending',
        color: '#22c55e',
        rules: [{ id: '1', field: 'playCount', operator: 'greaterThan', value: 0 }],
        matchAll: true,
        sortBy: 'playCount',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    },
    {
        id: 'programs',
        name: 'Programs',
        icon: 'film',
        color: '#6366f1',
        rules: [{ id: '1', field: 'type', operator: 'equals', value: 'program' }],
        matchAll: true,
        sortBy: 'addedAt',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    },
    {
        id: 'ads',
        name: 'Advertisements',
        icon: 'tag',
        color: '#10b981',
        rules: [{ id: '1', field: 'type', operator: 'equals', value: 'ad' }],
        matchAll: true,
        sortBy: 'title',
        sortOrder: 'asc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    },
    {
        id: 'short-form',
        name: 'Short Form (<60s)',
        icon: 'zap',
        color: '#ec4899',
        rules: [{ id: '1', field: 'duration', operator: 'lessThan', value: 60 }],
        matchAll: true,
        sortBy: 'duration',
        sortOrder: 'asc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    },
    {
        id: 'long-form',
        name: 'Long Form (>10min)',
        icon: 'film',
        color: '#8b5cf6',
        rules: [{ id: '1', field: 'duration', operator: 'greaterThan', value: 600 }],
        matchAll: true,
        sortBy: 'duration',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    },
    {
        id: 'unplayed',
        name: 'Never Played',
        icon: 'eye',
        color: '#64748b',
        rules: [{ id: '1', field: 'playCount', operator: 'equals', value: 0 }],
        matchAll: true,
        sortBy: 'addedAt',
        sortOrder: 'desc',
        createdAt: '',
        updatedAt: '',
        isBuiltIn: true
    }
];

const STORAGE_KEY = 'playplay_smart_collections';

export function SmartCollections({ allMedia, onSelectCollection, onMediaClick }: SmartCollectionsProps) {
    const [collections, setCollections] = useState<SmartCollection[]>(() => {
        try {
            const saved = localStorage.getItem(STORAGE_KEY);
            return saved ? [...BUILT_IN_COLLECTIONS, ...JSON.parse(saved)] : BUILT_IN_COLLECTIONS;
        } catch {
            return BUILT_IN_COLLECTIONS;
        }
    });
    
    const [selectedCollection, setSelectedCollection] = useState<SmartCollection | null>(null);
    const [editingCollection, setEditingCollection] = useState<SmartCollection | null>(null);
    const [expandedSections, setExpandedSections] = useState<Set<string>>(new Set(['pinned', 'builtin']));
    const [searchQuery, setSearchQuery] = useState('');

    // Save custom collections to localStorage
    useEffect(() => {
        const customCollections = collections.filter(c => !c.isBuiltIn);
        localStorage.setItem(STORAGE_KEY, JSON.stringify(customCollections));
    }, [collections]);

    // Apply rules to filter media
    const evaluateRule = useCallback((item: MediaItem, rule: CollectionRule): boolean => {
        let fieldValue: any;
        
        switch (rule.field) {
            case 'type': fieldValue = item.type; break;
            case 'duration': fieldValue = item.duration; break;
            case 'title': fieldValue = item.title; break;
            case 'client': fieldValue = item.client || ''; break;
            case 'addedAt': fieldValue = item.addedAt || ''; break;
            case 'playCount': fieldValue = item.playCount || 0; break;
            case 'priority': fieldValue = item.priority || 0; break;
            case 'series': fieldValue = item.programSeries || ''; break;
            case 'archived': fieldValue = item.archived || false; break;
            case 'hasSegments': fieldValue = (item.segments?.length || 0) > 0; break;
            default: return false;
        }

        switch (rule.operator) {
            case 'equals':
                return fieldValue === rule.value;
            case 'notEquals':
                return fieldValue !== rule.value;
            case 'contains':
                return String(fieldValue).toLowerCase().includes(String(rule.value).toLowerCase());
            case 'notContains':
                return !String(fieldValue).toLowerCase().includes(String(rule.value).toLowerCase());
            case 'greaterThan':
                return Number(fieldValue) > Number(rule.value);
            case 'lessThan':
                return Number(fieldValue) < Number(rule.value);
            case 'between':
                return Number(fieldValue) >= Number(rule.value) && Number(fieldValue) <= Number(rule.value2);
            case 'isEmpty':
                return !fieldValue || fieldValue === '' || fieldValue === 0;
            case 'isNotEmpty':
                return fieldValue && fieldValue !== '' && fieldValue !== 0;
            default:
                return false;
        }
    }, []);

    const getCollectionItems = useCallback((collection: SmartCollection): MediaItem[] => {
        // Special case: "Recently Added" shows all items sorted by date
        if (collection.id === 'recently-added') {
            return [...allMedia]
                .filter(m => !m.archived)
                .sort((a, b) => (b.addedAt || '').localeCompare(a.addedAt || ''))
                .slice(0, 50); // Limit to 50 most recent
        }

        let filtered = allMedia.filter(item => {
            if (item.archived) return false;
            if (collection.rules.length === 0) return true;
            
            const results = collection.rules.map(rule => evaluateRule(item, rule));
            return collection.matchAll 
                ? results.every(r => r) 
                : results.some(r => r);
        });

        // Apply sorting
        filtered.sort((a, b) => {
            let comparison = 0;
            switch (collection.sortBy) {
                case 'addedAt':
                    comparison = (a.addedAt || '').localeCompare(b.addedAt || '');
                    break;
                case 'title':
                    comparison = a.title.localeCompare(b.title);
                    break;
                case 'duration':
                    comparison = a.duration - b.duration;
                    break;
                case 'playCount':
                    comparison = (a.playCount || 0) - (b.playCount || 0);
                    break;
            }
            return collection.sortOrder === 'asc' ? comparison : -comparison;
        });

        return filtered;
    }, [allMedia, evaluateRule]);

    const handleSelectCollection = (collection: SmartCollection) => {
        setSelectedCollection(collection);
        const items = getCollectionItems(collection);
        onSelectCollection?.(collection, items);
    };

    const handleCreateCollection = () => {
        const newCollection: SmartCollection = {
            id: `custom_${Date.now()}`,
            name: 'New Collection',
            icon: 'folder',
            color: '#3b82f6',
            rules: [],
            matchAll: true,
            sortBy: 'addedAt',
            sortOrder: 'desc',
            createdAt: new Date().toISOString(),
            updatedAt: new Date().toISOString()
        };
        setEditingCollection(newCollection);
    };

    const handleSaveCollection = (collection: SmartCollection) => {
        collection.updatedAt = new Date().toISOString();
        
        setCollections(prev => {
            const existing = prev.findIndex(c => c.id === collection.id);
            if (existing >= 0) {
                const updated = [...prev];
                updated[existing] = collection;
                return updated;
            }
            return [...prev, collection];
        });
        
        setEditingCollection(null);
    };

    const handleDeleteCollection = (id: string) => {
        if (!confirm('Delete this collection?')) return;
        setCollections(prev => prev.filter(c => c.id !== id));
        if (selectedCollection?.id === id) {
            setSelectedCollection(null);
        }
    };

    const toggleSection = (section: string) => {
        setExpandedSections(prev => {
            const next = new Set(prev);
            if (next.has(section)) next.delete(section);
            else next.add(section);
            return next;
        });
    };

    const getIconComponent = (iconName: string) => {
        switch (iconName) {
            case 'star': return Star;
            case 'clock': return Clock;
            case 'calendar': return Calendar;
            case 'film': return Film;
            case 'music': return Music;
            case 'image': return ImageIcon;
            case 'tag': return Tag;
            case 'zap': return Zap;
            case 'trending': return TrendingUp;
            case 'eye': return Eye;
            case 'sparkles': return Sparkles;
            default: return Folder;
        }
    };

    const pinnedCollections = collections.filter(c => c.isPinned);
    const builtInCollections = collections.filter(c => c.isBuiltIn && !c.isPinned);
    const customCollections = collections.filter(c => !c.isBuiltIn);

    // Filter collections by search
    const filterBySearch = (cols: SmartCollection[]) => {
        if (!searchQuery) return cols;
        return cols.filter(c => c.name.toLowerCase().includes(searchQuery.toLowerCase()));
    };

    return (
        <div className={styles.container}>
            <div className={styles.sidebar}>
                <div className={styles.header}>
                    <h3><Sparkles size={16} /> Smart Collections</h3>
                    <button className={styles.addBtn} onClick={handleCreateCollection} title="Create Collection">
                        <Plus size={16} />
                    </button>
                </div>

                <div className={styles.searchBox}>
                    <Search size={14} />
                    <input 
                        placeholder="Search collections..."
                        value={searchQuery}
                        onChange={e => setSearchQuery(e.target.value)}
                    />
                </div>

                <div className={styles.collectionsList}>
                    {/* Pinned Collections */}
                    {pinnedCollections.length > 0 && (
                        <div className={styles.section}>
                            <div 
                                className={styles.sectionHeader} 
                                onClick={() => toggleSection('pinned')}
                            >
                                {expandedSections.has('pinned') ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                                <Star size={14} />
                                <span>Pinned</span>
                                <span className={styles.count}>{pinnedCollections.length}</span>
                            </div>
                            {expandedSections.has('pinned') && (
                                <div className={styles.sectionContent}>
                                    {filterBySearch(pinnedCollections).map(collection => {
                                        const Icon = getIconComponent(collection.icon);
                                        const itemCount = getCollectionItems(collection).length;
                                        return (
                                            <div 
                                                key={collection.id}
                                                className={`${styles.collectionItem} ${selectedCollection?.id === collection.id ? styles.selected : ''}`}
                                                onClick={() => handleSelectCollection(collection)}
                                            >
                                                <div className={styles.collectionIcon} style={{ color: collection.color }}>
                                                    <Icon size={16} />
                                                </div>
                                                <span className={styles.collectionName}>{collection.name}</span>
                                                <span className={styles.itemCount}>{itemCount}</span>
                                            </div>
                                        );
                                    })}
                                </div>
                            )}
                        </div>
                    )}

                    {/* Built-in Collections */}
                    <div className={styles.section}>
                        <div 
                            className={styles.sectionHeader} 
                            onClick={() => toggleSection('builtin')}
                        >
                            {expandedSections.has('builtin') ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                            <Zap size={14} />
                            <span>Smart</span>
                            <span className={styles.count}>{builtInCollections.length}</span>
                        </div>
                        {expandedSections.has('builtin') && (
                            <div className={styles.sectionContent}>
                                {filterBySearch(builtInCollections).map(collection => {
                                    const Icon = getIconComponent(collection.icon);
                                    const itemCount = getCollectionItems(collection).length;
                                    return (
                                        <div 
                                            key={collection.id}
                                            className={`${styles.collectionItem} ${selectedCollection?.id === collection.id ? styles.selected : ''}`}
                                            onClick={() => handleSelectCollection(collection)}
                                        >
                                            <div className={styles.collectionIcon} style={{ color: collection.color }}>
                                                <Icon size={16} />
                                            </div>
                                            <span className={styles.collectionName}>{collection.name}</span>
                                            <span className={styles.itemCount}>{itemCount}</span>
                                        </div>
                                    );
                                })}
                            </div>
                        )}
                    </div>

                    {/* Custom Collections */}
                    <div className={styles.section}>
                        <div 
                            className={styles.sectionHeader} 
                            onClick={() => toggleSection('custom')}
                        >
                            {expandedSections.has('custom') ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                            <Folder size={14} />
                            <span>Custom</span>
                            <span className={styles.count}>{customCollections.length}</span>
                        </div>
                        {expandedSections.has('custom') && (
                            <div className={styles.sectionContent}>
                                {filterBySearch(customCollections).map(collection => {
                                    const Icon = getIconComponent(collection.icon);
                                    const itemCount = getCollectionItems(collection).length;
                                    return (
                                        <div 
                                            key={collection.id}
                                            className={`${styles.collectionItem} ${selectedCollection?.id === collection.id ? styles.selected : ''}`}
                                            onClick={() => handleSelectCollection(collection)}
                                        >
                                            <div className={styles.collectionIcon} style={{ color: collection.color }}>
                                                <Icon size={16} />
                                            </div>
                                            <span className={styles.collectionName}>{collection.name}</span>
                                            <span className={styles.itemCount}>{itemCount}</span>
                                            <button 
                                                className={styles.editBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    setEditingCollection(collection);
                                                }}
                                            >
                                                <Edit2 size={12} />
                                            </button>
                                            <button 
                                                className={styles.deleteBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleDeleteCollection(collection.id);
                                                }}
                                            >
                                                <Trash2 size={12} />
                                            </button>
                                        </div>
                                    );
                                })}
                                {customCollections.length === 0 && (
                                    <div className={styles.emptyCustom}>
                                        <FolderPlus size={24} />
                                        <p>No custom collections yet</p>
                                        <button onClick={handleCreateCollection}>Create Collection</button>
                                    </div>
                                )}
                            </div>
                        )}
                    </div>
                </div>
            </div>

            {/* Collection Content */}
            <div className={styles.content}>
                {selectedCollection ? (
                    <>
                        <div className={styles.contentHeader}>
                            <div className={styles.collectionInfo}>
                                {React.createElement(getIconComponent(selectedCollection.icon), { 
                                    size: 24, 
                                    style: { color: selectedCollection.color } 
                                })}
                                <div>
                                    <h2>{selectedCollection.name}</h2>
                                    <span className={styles.rulesSummary}>
                                        {selectedCollection.rules.length === 0 
                                            ? 'All items' 
                                            : `${selectedCollection.rules.length} rule${selectedCollection.rules.length > 1 ? 's' : ''} (${selectedCollection.matchAll ? 'Match All' : 'Match Any'})`}
                                    </span>
                                </div>
                            </div>
                            <div className={styles.contentActions}>
                                <span className={styles.totalItems}>
                                    {getCollectionItems(selectedCollection).length} items
                                </span>
                            </div>
                        </div>

                        <div className={styles.itemsGrid}>
                            {getCollectionItems(selectedCollection).map(item => (
                                <div 
                                    key={item.id} 
                                    className={styles.mediaItem}
                                    onClick={() => onMediaClick?.(item)}
                                >
                                    <div 
                                        className={styles.mediaThumbnail}
                                        style={{
                                            backgroundImage: item.thumbnail?.startsWith('data:') 
                                                ? `url(${item.thumbnail})` 
                                                : undefined,
                                            backgroundColor: item.thumbnail?.startsWith('#') 
                                                ? item.thumbnail 
                                                : '#333'
                                        }}
                                    >
                                        {!item.thumbnail?.startsWith('data:') && <Film size={24} />}
                                        <span className={styles.duration}>{formatDuration(item.duration)}</span>
                                    </div>
                                    <div className={styles.mediaInfo}>
                                        <span className={styles.mediaTitle}>{item.title}</span>
                                        <span className={styles.mediaType}>{item.type}</span>
                                    </div>
                                </div>
                            ))}
                            {getCollectionItems(selectedCollection).length === 0 && (
                                <div className={styles.emptyContent}>
                                    <AlertCircle size={48} />
                                    <p>No items match this collection's rules</p>
                                </div>
                            )}
                        </div>
                    </>
                ) : (
                    <div className={styles.noSelection}>
                        <Sparkles size={48} />
                        <h3>Select a Collection</h3>
                        <p>Choose a collection from the sidebar to view its contents</p>
                    </div>
                )}
            </div>

            {/* Collection Editor Modal */}
            {editingCollection && (
                <CollectionEditor
                    collection={editingCollection}
                    onSave={handleSaveCollection}
                    onCancel={() => setEditingCollection(null)}
                />
            )}
        </div>
    );
}

// Collection Editor Component
function CollectionEditor({ 
    collection, 
    onSave, 
    onCancel 
}: { 
    collection: SmartCollection; 
    onSave: (c: SmartCollection) => void; 
    onCancel: () => void; 
}) {
    const [formData, setFormData] = useState<SmartCollection>(collection);

    const addRule = () => {
        const newRule: CollectionRule = {
            id: `rule_${Date.now()}`,
            field: 'type',
            operator: 'equals',
            value: 'program'
        };
        setFormData(prev => ({
            ...prev,
            rules: [...prev.rules, newRule]
        }));
    };

    const updateRule = (ruleId: string, updates: Partial<CollectionRule>) => {
        setFormData(prev => ({
            ...prev,
            rules: prev.rules.map(r => r.id === ruleId ? { ...r, ...updates } : r)
        }));
    };

    const removeRule = (ruleId: string) => {
        setFormData(prev => ({
            ...prev,
            rules: prev.rules.filter(r => r.id !== ruleId)
        }));
    };

    const ICONS = ['folder', 'star', 'clock', 'calendar', 'film', 'music', 'image', 'tag', 'zap', 'trending', 'eye', 'sparkles'];
    const COLORS = ['#3b82f6', '#22c55e', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#6366f1', '#10b981', '#64748b'];

    return (
        <div className={styles.editorOverlay}>
            <div className={styles.editor}>
                <div className={styles.editorHeader}>
                    <h3>{collection.createdAt ? 'Edit Collection' : 'Create Collection'}</h3>
                    <button onClick={onCancel}><X size={20} /></button>
                </div>

                <div className={styles.editorContent}>
                    <div className={styles.formGroup}>
                        <label>Name</label>
                        <input
                            value={formData.name}
                            onChange={e => setFormData(prev => ({ ...prev, name: e.target.value }))}
                            placeholder="Collection name..."
                        />
                    </div>

                    <div className={styles.formRow}>
                        <div className={styles.formGroup}>
                            <label>Icon</label>
                            <div className={styles.iconPicker}>
                                {ICONS.map(icon => (
                                    <button
                                        key={icon}
                                        className={formData.icon === icon ? styles.iconSelected : ''}
                                        onClick={() => setFormData(prev => ({ ...prev, icon }))}
                                    >
                                        {React.createElement(getIconComponent(icon), { size: 16 })}
                                    </button>
                                ))}
                            </div>
                        </div>

                        <div className={styles.formGroup}>
                            <label>Color</label>
                            <div className={styles.colorPicker}>
                                {COLORS.map(color => (
                                    <button
                                        key={color}
                                        className={formData.color === color ? styles.colorSelected : ''}
                                        style={{ backgroundColor: color }}
                                        onClick={() => setFormData(prev => ({ ...prev, color }))}
                                    />
                                ))}
                            </div>
                        </div>
                    </div>

                    <div className={styles.rulesSection}>
                        <div className={styles.rulesSectionHeader}>
                            <h4><Filter size={16} /> Rules</h4>
                            <div className={styles.matchMode}>
                                <button
                                    className={formData.matchAll ? styles.active : ''}
                                    onClick={() => setFormData(prev => ({ ...prev, matchAll: true }))}
                                >
                                    Match All
                                </button>
                                <button
                                    className={!formData.matchAll ? styles.active : ''}
                                    onClick={() => setFormData(prev => ({ ...prev, matchAll: false }))}
                                >
                                    Match Any
                                </button>
                            </div>
                        </div>

                        <div className={styles.rulesList}>
                            {formData.rules.map(rule => (
                                <div key={rule.id} className={styles.ruleRow}>
                                    <select
                                        value={rule.field}
                                        onChange={e => updateRule(rule.id, { field: e.target.value as RuleField })}
                                    >
                                        <option value="type">Type</option>
                                        <option value="duration">Duration</option>
                                        <option value="title">Title</option>
                                        <option value="client">Client</option>
                                        <option value="playCount">Play Count</option>
                                        <option value="priority">Priority</option>
                                        <option value="series">Series</option>
                                        <option value="archived">Archived</option>
                                        <option value="hasSegments">Has Segments</option>
                                    </select>

                                    <select
                                        value={rule.operator}
                                        onChange={e => updateRule(rule.id, { operator: e.target.value as RuleOperator })}
                                    >
                                        <option value="equals">Equals</option>
                                        <option value="notEquals">Not Equals</option>
                                        <option value="contains">Contains</option>
                                        <option value="notContains">Not Contains</option>
                                        <option value="greaterThan">Greater Than</option>
                                        <option value="lessThan">Less Than</option>
                                        <option value="between">Between</option>
                                        <option value="isEmpty">Is Empty</option>
                                        <option value="isNotEmpty">Is Not Empty</option>
                                    </select>

                                    {!['isEmpty', 'isNotEmpty'].includes(rule.operator) && (
                                        <>
                                            {rule.field === 'type' ? (
                                                <select
                                                    value={String(rule.value)}
                                                    onChange={e => updateRule(rule.id, { value: e.target.value })}
                                                >
                                                    <option value="program">Program</option>
                                                    <option value="ad">Ad</option>
                                                    <option value="station_id">Station ID</option>
                                                    <option value="bumper">Bumper</option>
                                                    <option value="promo">Promo</option>
                                                    <option value="paid_program">Paid Program</option>
                                                    <option value="video">Video</option>
                                                    <option value="image">Image</option>
                                                    <option value="live">Live</option>
                                                </select>
                                            ) : rule.field === 'archived' || rule.field === 'hasSegments' ? (
                                                <select
                                                    value={String(rule.value)}
                                                    onChange={e => updateRule(rule.id, { value: e.target.value === 'true' })}
                                                >
                                                    <option value="true">Yes</option>
                                                    <option value="false">No</option>
                                                </select>
                                            ) : (
                                                <input
                                                    type={['duration', 'playCount', 'priority'].includes(rule.field) ? 'number' : 'text'}
                                                    value={String(rule.value)}
                                                    onChange={e => updateRule(rule.id, { 
                                                        value: ['duration', 'playCount', 'priority'].includes(rule.field) 
                                                            ? Number(e.target.value) 
                                                            : e.target.value 
                                                    })}
                                                    placeholder="Value..."
                                                />
                                            )}
                                            {rule.operator === 'between' && (
                                                <input
                                                    type="number"
                                                    value={String(rule.value2 || '')}
                                                    onChange={e => updateRule(rule.id, { value2: Number(e.target.value) })}
                                                    placeholder="Max..."
                                                />
                                            )}
                                        </>
                                    )}

                                    <button className={styles.removeRuleBtn} onClick={() => removeRule(rule.id)}>
                                        <Trash2 size={14} />
                                    </button>
                                </div>
                            ))}

                            <button className={styles.addRuleBtn} onClick={addRule}>
                                <Plus size={14} /> Add Rule
                            </button>
                        </div>
                    </div>

                    <div className={styles.formRow}>
                        <div className={styles.formGroup}>
                            <label>Sort By</label>
                            <select
                                value={formData.sortBy}
                                onChange={e => setFormData(prev => ({ ...prev, sortBy: e.target.value as any }))}
                            >
                                <option value="addedAt">Date Added</option>
                                <option value="title">Title</option>
                                <option value="duration">Duration</option>
                                <option value="playCount">Play Count</option>
                            </select>
                        </div>
                        <div className={styles.formGroup}>
                            <label>Order</label>
                            <select
                                value={formData.sortOrder}
                                onChange={e => setFormData(prev => ({ ...prev, sortOrder: e.target.value as 'asc' | 'desc' }))}
                            >
                                <option value="asc">Ascending</option>
                                <option value="desc">Descending</option>
                            </select>
                        </div>
                    </div>

                    <label className={styles.checkboxLabel}>
                        <input
                            type="checkbox"
                            checked={formData.isPinned || false}
                            onChange={e => setFormData(prev => ({ ...prev, isPinned: e.target.checked }))}
                        />
                        Pin to top of list
                    </label>
                </div>

                <div className={styles.editorActions}>
                    <button className={styles.cancelBtn} onClick={onCancel}>Cancel</button>
                    <button className={styles.saveBtn} onClick={() => onSave(formData)}>
                        <Save size={16} /> Save Collection
                    </button>
                </div>
            </div>
        </div>
    );
}

// Helper for icon component lookup
function getIconComponent(iconName: string) {
    switch (iconName) {
        case 'star': return Star;
        case 'clock': return Clock;
        case 'calendar': return Calendar;
        case 'film': return Film;
        case 'music': return Music;
        case 'image': return ImageIcon;
        case 'tag': return Tag;
        case 'zap': return Zap;
        case 'trending': return TrendingUp;
        case 'eye': return Eye;
        case 'sparkles': return Sparkles;
        default: return Folder;
    }
}

export default SmartCollections;

