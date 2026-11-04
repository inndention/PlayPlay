import React, { useState, useCallback, useEffect } from 'react';
import styles from './MediaFavorites.module.css';
import { 
    Star, Trash2, Play, Clock, Film, Image, Volume2, 
    Folder, Search, Grid, List, SortAsc, SortDesc
} from 'lucide-react';
import { MediaItem } from '../lib/types';
import { formatDuration } from '../lib/utils';

interface MediaFavoritesProps {
    mediaItems: MediaItem[];
    favorites: Set<string>;
    recentlyAccessed: string[];
    onToggleFavorite: (itemId: string) => void;
    onPlayItem: (item: MediaItem) => void;
    onItemClick?: (item: MediaItem) => void;
    onClearRecent?: () => void;
}

export const MediaFavorites: React.FC<MediaFavoritesProps> = ({
    mediaItems,
    favorites,
    recentlyAccessed,
    onToggleFavorite,
    onPlayItem,
    onItemClick,
    onClearRecent
}) => {
    const [activeTab, setActiveTab] = useState<'favorites' | 'recent'>('favorites');
    const [searchQuery, setSearchQuery] = useState('');
    const [viewMode, setViewMode] = useState<'grid' | 'list'>('grid');
    const [sortBy, setSortBy] = useState<'name' | 'date' | 'duration'>('name');
    const [sortDirection, setSortDirection] = useState<'asc' | 'desc'>('asc');

    const favoriteItems = mediaItems.filter(item => favorites.has(item.id));
    const recentItems = recentlyAccessed
        .map(id => mediaItems.find(item => item.id === id))
        .filter((item): item is MediaItem => item !== undefined)
        .slice(0, 20);

    const currentItems = activeTab === 'favorites' ? favoriteItems : recentItems;

    const filteredItems = currentItems.filter(item =>
        item.title.toLowerCase().includes(searchQuery.toLowerCase()) ||
        (item.filePath && item.filePath.toLowerCase().includes(searchQuery.toLowerCase()))
    );

    const sortedItems = [...filteredItems].sort((a, b) => {
        let comparison = 0;
        switch (sortBy) {
            case 'name':
                comparison = a.title.localeCompare(b.title);
                break;
            case 'date':
                comparison = new Date(b.addedAt || 0).getTime() - new Date(a.addedAt || 0).getTime();
                break;
            case 'duration':
                comparison = (a.duration || 0) - (b.duration || 0);
                break;
        }
        return sortDirection === 'asc' ? comparison : -comparison;
    });

    const getTypeIcon = (type: string) => {
        switch (type) {
            case 'video': return <Film size={14} />;
            case 'image': return <Image size={14} />;
            case 'audio': return <Volume2 size={14} />;
            default: return <Folder size={14} />;
        }
    };

    const toggleSort = (field: 'name' | 'date' | 'duration') => {
        if (sortBy === field) {
            setSortDirection(prev => prev === 'asc' ? 'desc' : 'asc');
        } else {
            setSortBy(field);
            setSortDirection('asc');
        }
    };

    return (
        <div className={styles.container}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.tabs}>
                    <button 
                        className={`${styles.tab} ${activeTab === 'favorites' ? styles.active : ''}`}
                        onClick={() => setActiveTab('favorites')}
                    >
                        <Star size={14} />
                        Favorites
                        <span className={styles.count}>{favoriteItems.length}</span>
                    </button>
                    <button 
                        className={`${styles.tab} ${activeTab === 'recent' ? styles.active : ''}`}
                        onClick={() => setActiveTab('recent')}
                    >
                        <Clock size={14} />
                        Recent
                        <span className={styles.count}>{recentItems.length}</span>
                    </button>
                </div>
            </div>

            {/* Toolbar */}
            <div className={styles.toolbar}>
                <div className={styles.searchBox}>
                    <Search size={14} />
                    <input
                        type="text"
                        placeholder="Search..."
                        value={searchQuery}
                        onChange={e => setSearchQuery(e.target.value)}
                    />
                </div>

                <div className={styles.viewToggle}>
                    <button 
                        className={viewMode === 'grid' ? styles.active : ''}
                        onClick={() => setViewMode('grid')}
                        title="Grid view"
                    >
                        <Grid size={14} />
                    </button>
                    <button 
                        className={viewMode === 'list' ? styles.active : ''}
                        onClick={() => setViewMode('list')}
                        title="List view"
                    >
                        <List size={14} />
                    </button>
                </div>

                <div className={styles.sortControls}>
                    <button 
                        className={sortBy === 'name' ? styles.active : ''}
                        onClick={() => toggleSort('name')}
                    >
                        Name
                    </button>
                    <button 
                        className={sortBy === 'date' ? styles.active : ''}
                        onClick={() => toggleSort('date')}
                    >
                        Date
                    </button>
                    <button 
                        className={sortBy === 'duration' ? styles.active : ''}
                        onClick={() => toggleSort('duration')}
                    >
                        Duration
                    </button>
                    <span className={styles.sortDirection}>
                        {sortDirection === 'asc' ? <SortAsc size={14} /> : <SortDesc size={14} />}
                    </span>
                </div>

                {activeTab === 'recent' && onClearRecent && (
                    <button className={styles.clearBtn} onClick={onClearRecent}>
                        <Trash2 size={14} />
                        Clear
                    </button>
                )}
            </div>

            {/* Content */}
            <div className={`${styles.content} ${viewMode === 'list' ? styles.listMode : ''}`}>
                {sortedItems.length === 0 ? (
                    <div className={styles.empty}>
                        {activeTab === 'favorites' ? (
                            <>
                                <Star size={32} />
                                <p>No favorites yet</p>
                                <span>Click the star icon on any media to add it here</span>
                            </>
                        ) : (
                            <>
                                <Clock size={32} />
                                <p>No recent items</p>
                                <span>Recently accessed media will appear here</span>
                            </>
                        )}
                    </div>
                ) : viewMode === 'grid' ? (
                    <div className={styles.grid}>
                        {sortedItems.map(item => (
                            <div 
                                key={item.id} 
                                className={styles.gridItem}
                                onClick={() => onItemClick?.(item)}
                            >
                                <div className={styles.thumbnail}>
                                    {item.thumbnail ? (
                                        <img src={item.thumbnail} alt={item.title} />
                                    ) : (
                                        <div className={styles.placeholderThumb}>
                                            {getTypeIcon(item.type)}
                                        </div>
                                    )}
                                    <button 
                                        className={styles.playBtn}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            onPlayItem(item);
                                        }}
                                    >
                                        <Play size={20} />
                                    </button>
                                    <button
                                        className={`${styles.favoriteBtn} ${favorites.has(item.id) ? styles.active : ''}`}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            onToggleFavorite(item.id);
                                        }}
                                    >
                                        <Star size={14} />
                                    </button>
                                    {item.duration && (
                                        <span className={styles.duration}>
                                            {formatDuration(item.duration)}
                                        </span>
                                    )}
                                </div>
                                <div className={styles.itemInfo}>
                                    <span className={styles.itemName}>{item.title}</span>
                                    <span className={styles.itemType}>
                                        {getTypeIcon(item.type)} {item.type}
                                    </span>
                                </div>
                            </div>
                        ))}
                    </div>
                ) : (
                    <div className={styles.list}>
                        {sortedItems.map(item => (
                            <div 
                                key={item.id} 
                                className={styles.listItem}
                                onClick={() => onItemClick?.(item)}
                            >
                                <div className={styles.listThumb}>
                                    {item.thumbnail ? (
                                        <img src={item.thumbnail} alt={item.title} />
                                    ) : (
                                        <div className={styles.placeholderThumb}>
                                            {getTypeIcon(item.type)}
                                        </div>
                                    )}
                                </div>
                                <div className={styles.listInfo}>
                                    <span className={styles.listName}>{item.title}</span>
                                    <span className={styles.listMeta}>
                                        {getTypeIcon(item.type)} {item.type}
                                        {item.duration && ` • ${formatDuration(item.duration)}`}
                                        {(item.stats as any)?.resolution && ` • ${(item.stats as any).resolution}`}
                                    </span>
                                </div>
                                <div className={styles.listActions}>
                                    <button 
                                        className={`${styles.favoriteBtn} ${favorites.has(item.id) ? styles.active : ''}`}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            onToggleFavorite(item.id);
                                        }}
                                    >
                                        <Star size={16} />
                                    </button>
                                    <button 
                                        className={styles.playBtn}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            onPlayItem(item);
                                        }}
                                    >
                                        <Play size={16} />
                                    </button>
                                </div>
                            </div>
                        ))}
                    </div>
                )}
            </div>
        </div>
    );
};

export default MediaFavorites;

