import React, { useMemo, useState } from 'react';
import styles from './MediaStatsDashboard.module.css';
import { MediaItem, MediaType } from '../lib/types';
import { formatDuration } from '../lib/utils';
import {
    BarChart2, PieChart, TrendingUp, Clock, Film, Play, Calendar, HardDrive,
    Tv, Tag, Megaphone, Zap, Star, Eye, Activity, Layers, Download, Filter,
    ChevronDown, ArrowUp, ArrowDown, RefreshCw
} from 'lucide-react';

interface MediaStatsDashboardProps {
    allMedia: MediaItem[];
}

interface StatCard {
    title: string;
    value: string | number;
    subValue?: string;
    icon: React.ElementType;
    color: string;
    trend?: { value: number; isUp: boolean };
}

type TimeRange = 'all' | 'week' | 'month' | 'year';

export function MediaStatsDashboard({ allMedia }: MediaStatsDashboardProps) {
    const [timeRange, setTimeRange] = useState<TimeRange>('all');
    const [selectedType, setSelectedType] = useState<MediaType | 'all'>('all');

    // Filter media by time range
    const filteredMedia = useMemo(() => {
        let filtered = allMedia;
        
        const now = new Date();
        if (timeRange !== 'all') {
            const cutoff = new Date();
            switch (timeRange) {
                case 'week': cutoff.setDate(now.getDate() - 7); break;
                case 'month': cutoff.setMonth(now.getMonth() - 1); break;
                case 'year': cutoff.setFullYear(now.getFullYear() - 1); break;
            }
            filtered = filtered.filter(m => m.addedAt && new Date(m.addedAt) >= cutoff);
        }
        
        if (selectedType !== 'all') {
            filtered = filtered.filter(m => m.type === selectedType);
        }
        
        return filtered;
    }, [allMedia, timeRange, selectedType]);

    // Calculate statistics
    const stats = useMemo(() => {
        const totalItems = filteredMedia.length;
        const totalDuration = filteredMedia.reduce((sum, m) => sum + (m.duration || 0), 0);
        const totalPlays = filteredMedia.reduce((sum, m) => sum + (m.playCount || 0), 0);
        const avgDuration = totalItems > 0 ? totalDuration / totalItems : 0;
        
        // Type breakdown
        const typeBreakdown: Record<string, number> = {};
        filteredMedia.forEach(m => {
            typeBreakdown[m.type] = (typeBreakdown[m.type] || 0) + 1;
        });
        
        // Most played
        const sortedByPlays = [...filteredMedia].sort((a, b) => (b.playCount || 0) - (a.playCount || 0));
        const topPlayed = sortedByPlays.slice(0, 5);
        
        // Recently added
        const sortedByDate = [...filteredMedia]
            .filter(m => m.addedAt)
            .sort((a, b) => (b.addedAt || '').localeCompare(a.addedAt || ''));
        const recentlyAdded = sortedByDate.slice(0, 5);
        
        // Duration distribution
        const durationBuckets = {
            '< 30s': filteredMedia.filter(m => m.duration < 30).length,
            '30s - 1m': filteredMedia.filter(m => m.duration >= 30 && m.duration < 60).length,
            '1m - 5m': filteredMedia.filter(m => m.duration >= 60 && m.duration < 300).length,
            '5m - 30m': filteredMedia.filter(m => m.duration >= 300 && m.duration < 1800).length,
            '30m - 1h': filteredMedia.filter(m => m.duration >= 1800 && m.duration < 3600).length,
            '> 1h': filteredMedia.filter(m => m.duration >= 3600).length,
        };
        
        // Unplayed items
        const unplayed = filteredMedia.filter(m => !m.playCount || m.playCount === 0).length;
        const unplayedPercent = totalItems > 0 ? Math.round((unplayed / totalItems) * 100) : 0;
        
        // Archived items
        const archived = filteredMedia.filter(m => m.archived).length;
        
        // Client breakdown (for ads)
        const clientBreakdown: Record<string, number> = {};
        filteredMedia.filter(m => m.type === 'ad' && m.client).forEach(m => {
            clientBreakdown[m.client!] = (clientBreakdown[m.client!] || 0) + 1;
        });
        
        // Series breakdown (for programs)
        const seriesBreakdown: Record<string, number> = {};
        filteredMedia.filter(m => m.programSeries).forEach(m => {
            seriesBreakdown[m.programSeries!] = (seriesBreakdown[m.programSeries!] || 0) + 1;
        });
        
        return {
            totalItems,
            totalDuration,
            totalPlays,
            avgDuration,
            typeBreakdown,
            topPlayed,
            recentlyAdded,
            durationBuckets,
            unplayed,
            unplayedPercent,
            archived,
            clientBreakdown,
            seriesBreakdown
        };
    }, [filteredMedia]);

    const getTypeColor = (type: string): string => {
        const colors: Record<string, string> = {
            program: '#3b82f6',
            ad: '#10b981',
            station_id: '#6366f1',
            bumper: '#ec4899',
            promo: '#8b5cf6',
            paid_program: '#f59e0b',
            video: '#06b6d4',
            image: '#84cc16',
            live: '#ef4444',
        };
        return colors[type] || '#64748b';
    };

    const getTypeIcon = (type: string) => {
        switch (type) {
            case 'program': return Tv;
            case 'ad': return Megaphone;
            case 'station_id': return Tag;
            case 'bumper': return Zap;
            case 'promo': return Star;
            default: return Film;
        }
    };

    const statCards: StatCard[] = [
        {
            title: 'Total Items',
            value: stats.totalItems,
            subValue: `${stats.archived} archived`,
            icon: Layers,
            color: '#3b82f6'
        },
        {
            title: 'Total Duration',
            value: formatDuration(stats.totalDuration),
            subValue: `Avg: ${formatDuration(stats.avgDuration)}`,
            icon: Clock,
            color: '#10b981'
        },
        {
            title: 'Total Plays',
            value: stats.totalPlays.toLocaleString(),
            subValue: `${stats.unplayedPercent}% never played`,
            icon: Play,
            color: '#8b5cf6'
        },
        {
            title: 'Active Content',
            value: stats.totalItems - stats.archived,
            subValue: `${stats.unplayed} unplayed`,
            icon: Activity,
            color: '#f59e0b'
        }
    ];

    // Calculate max value for chart scaling
    const maxTypeCount = Math.max(...Object.values(stats.typeBreakdown), 1);
    const maxDurationCount = Math.max(...Object.values(stats.durationBuckets), 1);

    return (
        <div className={styles.dashboard}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <BarChart2 size={24} />
                    <h2>Media Analytics</h2>
                </div>
                <div className={styles.filters}>
                    <div className={styles.filterGroup}>
                        <Filter size={14} />
                        <select value={timeRange} onChange={e => setTimeRange(e.target.value as TimeRange)}>
                            <option value="all">All Time</option>
                            <option value="week">Last 7 Days</option>
                            <option value="month">Last 30 Days</option>
                            <option value="year">Last Year</option>
                        </select>
                    </div>
                    <div className={styles.filterGroup}>
                        <Layers size={14} />
                        <select value={selectedType} onChange={e => setSelectedType(e.target.value as MediaType | 'all')}>
                            <option value="all">All Types</option>
                            <option value="program">Programs</option>
                            <option value="ad">Ads</option>
                            <option value="station_id">Station IDs</option>
                            <option value="bumper">Bumpers</option>
                            <option value="promo">Promos</option>
                            <option value="video">Videos</option>
                            <option value="image">Images</option>
                        </select>
                    </div>
                </div>
            </div>

            {/* Stat Cards */}
            <div className={styles.statCards}>
                {statCards.map((card, index) => (
                    <div key={index} className={styles.statCard}>
                        <div className={styles.statIcon} style={{ backgroundColor: `${card.color}20`, color: card.color }}>
                            <card.icon size={24} />
                        </div>
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>{card.value}</span>
                            <span className={styles.statTitle}>{card.title}</span>
                            {card.subValue && <span className={styles.statSub}>{card.subValue}</span>}
                        </div>
                    </div>
                ))}
            </div>

            <div className={styles.chartsRow}>
                {/* Type Distribution */}
                <div className={styles.chartCard}>
                    <h3><PieChart size={16} /> Content Type Distribution</h3>
                    <div className={styles.typeChart}>
                        {Object.entries(stats.typeBreakdown)
                            .sort((a, b) => b[1] - a[1])
                            .map(([type, count]) => {
                                const Icon = getTypeIcon(type);
                                const percentage = Math.round((count / stats.totalItems) * 100);
                                return (
                                    <div key={type} className={styles.typeRow}>
                                        <div className={styles.typeLabel}>
                                            <Icon size={14} style={{ color: getTypeColor(type) }} />
                                            <span>{type.replace('_', ' ')}</span>
                                        </div>
                                        <div className={styles.typeBar}>
                                            <div 
                                                className={styles.typeBarFill}
                                                style={{ 
                                                    width: `${(count / maxTypeCount) * 100}%`,
                                                    backgroundColor: getTypeColor(type)
                                                }}
                                            />
                                        </div>
                                        <span className={styles.typeCount}>{count}</span>
                                        <span className={styles.typePercent}>{percentage}%</span>
                                    </div>
                                );
                            })}
                    </div>
                </div>

                {/* Duration Distribution */}
                <div className={styles.chartCard}>
                    <h3><Clock size={16} /> Duration Distribution</h3>
                    <div className={styles.durationChart}>
                        {Object.entries(stats.durationBuckets).map(([bucket, count]) => (
                            <div key={bucket} className={styles.durationBar}>
                                <div className={styles.durationBarContainer}>
                                    <div 
                                        className={styles.durationBarFill}
                                        style={{ height: `${(count / maxDurationCount) * 100}%` }}
                                    />
                                </div>
                                <span className={styles.durationLabel}>{bucket}</span>
                                <span className={styles.durationCount}>{count}</span>
                            </div>
                        ))}
                    </div>
                </div>
            </div>

            <div className={styles.listsRow}>
                {/* Top Played */}
                <div className={styles.listCard}>
                    <h3><TrendingUp size={16} /> Most Played</h3>
                    <div className={styles.mediaList}>
                        {stats.topPlayed.length > 0 ? (
                            stats.topPlayed.map((item, index) => (
                                <div key={item.id} className={styles.mediaItem}>
                                    <span className={styles.rank}>{index + 1}</span>
                                    <div 
                                        className={styles.thumbnail}
                                        style={{
                                            backgroundImage: item.thumbnail?.startsWith('data:') 
                                                ? `url(${item.thumbnail})` 
                                                : undefined,
                                            backgroundColor: item.thumbnail?.startsWith('#') 
                                                ? item.thumbnail 
                                                : '#333'
                                        }}
                                    />
                                    <div className={styles.itemInfo}>
                                        <span className={styles.itemTitle}>{item.title}</span>
                                        <span className={styles.itemMeta}>{item.type} • {formatDuration(item.duration)}</span>
                                    </div>
                                    <div className={styles.playCount}>
                                        <Play size={12} />
                                        {item.playCount || 0}
                                    </div>
                                </div>
                            ))
                        ) : (
                            <div className={styles.emptyList}>No play data available</div>
                        )}
                    </div>
                </div>

                {/* Recently Added */}
                <div className={styles.listCard}>
                    <h3><Calendar size={16} /> Recently Added</h3>
                    <div className={styles.mediaList}>
                        {stats.recentlyAdded.length > 0 ? (
                            stats.recentlyAdded.map((item, index) => (
                                <div key={item.id} className={styles.mediaItem}>
                                    <span className={styles.rank}>{index + 1}</span>
                                    <div 
                                        className={styles.thumbnail}
                                        style={{
                                            backgroundImage: item.thumbnail?.startsWith('data:') 
                                                ? `url(${item.thumbnail})` 
                                                : undefined,
                                            backgroundColor: item.thumbnail?.startsWith('#') 
                                                ? item.thumbnail 
                                                : '#333'
                                        }}
                                    />
                                    <div className={styles.itemInfo}>
                                        <span className={styles.itemTitle}>{item.title}</span>
                                        <span className={styles.itemMeta}>{item.type} • {formatDuration(item.duration)}</span>
                                    </div>
                                    <div className={styles.addedDate}>
                                        {item.addedAt ? new Date(item.addedAt).toLocaleDateString() : '-'}
                                    </div>
                                </div>
                            ))
                        ) : (
                            <div className={styles.emptyList}>No recent additions</div>
                        )}
                    </div>
                </div>
            </div>

            {/* Client & Series Breakdown */}
            <div className={styles.breakdownRow}>
                {Object.keys(stats.clientBreakdown).length > 0 && (
                    <div className={styles.breakdownCard}>
                        <h3><Megaphone size={16} /> Ad Clients</h3>
                        <div className={styles.breakdownList}>
                            {Object.entries(stats.clientBreakdown)
                                .sort((a, b) => b[1] - a[1])
                                .slice(0, 10)
                                .map(([client, count]) => (
                                    <div key={client} className={styles.breakdownItem}>
                                        <span className={styles.breakdownName}>{client}</span>
                                        <span className={styles.breakdownCount}>{count} ads</span>
                                    </div>
                                ))}
                        </div>
                    </div>
                )}

                {Object.keys(stats.seriesBreakdown).length > 0 && (
                    <div className={styles.breakdownCard}>
                        <h3><Tv size={16} /> Program Series</h3>
                        <div className={styles.breakdownList}>
                            {Object.entries(stats.seriesBreakdown)
                                .sort((a, b) => b[1] - a[1])
                                .slice(0, 10)
                                .map(([series, count]) => (
                                    <div key={series} className={styles.breakdownItem}>
                                        <span className={styles.breakdownName}>{series}</span>
                                        <span className={styles.breakdownCount}>{count} episodes</span>
                                    </div>
                                ))}
                        </div>
                    </div>
                )}
            </div>

            {/* Storage Estimate */}
            <div className={styles.storageInfo}>
                <HardDrive size={16} />
                <span>Total library duration: {formatDuration(stats.totalDuration)}</span>
                <span className={styles.storageSep}>•</span>
                <span>{stats.totalItems} items</span>
                <span className={styles.storageSep}>•</span>
                <span>{stats.totalPlays.toLocaleString()} total plays</span>
            </div>
        </div>
    );
}

export default MediaStatsDashboard;
