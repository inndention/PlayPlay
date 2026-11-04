/**
 * TRAFFIC MANAGER
 * 
 * Manages probabilistic ad scheduling:
 * - View ad pool with weights and probabilities
 * - Track play counts vs targets
 * - See delivery status
 * - View play logs
 */

import React, { useState, useEffect, useMemo } from 'react';
import {
    TrafficItem,
    ContentClass,
    PlayStats,
    PlayLog,
    loadPlayLogs,
    loadPlayStats,
    getPlayStats,
    getTodayStats,
    getDaypart,
    getDaypartDisplay,
    calculateWeight,
    SelectionContext,
} from '../lib/trafficEngine';
import { loadLocalMedia } from '../lib/storage';
import { MediaItem } from '../lib/types';
import {
    BarChart2, TrendingUp, Clock, Target, Play, Pause,
    AlertTriangle, CheckCircle, XCircle, RefreshCw,
    Calendar, Filter, ChevronDown, ChevronUp
} from 'lucide-react';
import styles from './TrafficManager.module.css';

type ViewTab = 'pool' | 'stats' | 'log';

export function TrafficManager() {
    const [activeTab, setActiveTab] = useState<ViewTab>('pool');
    const [adPool, setAdPool] = useState<TrafficItem[]>([]);
    const [playLogs, setPlayLogs] = useState<PlayLog[]>([]);
    const [playStats, setPlayStats] = useState<Map<string, PlayStats>>(new Map());
    const [selectedDaypart, setSelectedDaypart] = useState<string>('all');
    const [sortBy, setSortBy] = useState<'weight' | 'plays' | 'delivery'>('weight');
    
    // Load data
    useEffect(() => {
        const loadData = () => {
            // Convert local media to traffic items for demo
            const localMedia = loadLocalMedia();
            const trafficItems: TrafficItem[] = localMedia
                .filter(m => m.type === 'ad' || m.type === 'promo')
                .map(m => ({
                    ...m,
                    contentClass: (m.type === 'ad' ? 'commercial' : 'promo') as ContentClass,
                    baseWeight: 50,
                    isActive: true,
                    isPaused: false,
                    targetPlays: 100,
                    targetPerDay: 10,
                }));
            
            setAdPool(trafficItems);
            setPlayLogs(loadPlayLogs());
            setPlayStats(loadPlayStats());
        };
        
        loadData();
        const interval = setInterval(loadData, 5000); // Refresh every 5s
        return () => clearInterval(interval);
    }, []);
    
    // Current context for weight calculations
    const currentContext = useMemo((): SelectionContext => {
        const now = Date.now();
        const hour = new Date(now).getHours();
        return {
            now,
            daypart: getDaypart(hour),
            hour,
            availableDuration: 60, // 60 seconds for demo
            recentPlays: playLogs.slice(-50),
            dayStats: getTodayStats(),
        };
    }, [playLogs]);
    
    // Calculate weights and probabilities for display
    const poolWithWeights = useMemo(() => {
        let totalWeight = 0;
        const items = adPool.map(item => {
            const { weight, eligible, reason } = calculateWeight(item, currentContext);
            totalWeight += weight;
            const stats = playStats.get(item.id);
            const deliveryRate = item.targetPlays && stats 
                ? (stats.totalPlays / item.targetPlays) * 100 
                : 0;
            
            return {
                item,
                weight,
                eligible,
                reason,
                stats,
                deliveryRate,
            };
        });
        
        // Calculate probabilities
        return items.map(i => ({
            ...i,
            probability: totalWeight > 0 ? (i.weight / totalWeight) * 100 : 0,
        }));
    }, [adPool, currentContext, playStats]);
    
    // Sort items
    const sortedPool = useMemo(() => {
        return [...poolWithWeights].sort((a, b) => {
            switch (sortBy) {
                case 'weight':
                    return b.weight - a.weight;
                case 'plays':
                    return (b.stats?.totalPlays || 0) - (a.stats?.totalPlays || 0);
                case 'delivery':
                    return b.deliveryRate - a.deliveryRate;
                default:
                    return 0;
            }
        });
    }, [poolWithWeights, sortBy]);
    
    // Summary stats
    const summaryStats = useMemo(() => {
        const today = playLogs.filter(l => {
            const logDate = new Date(l.timestamp).toDateString();
            return logDate === new Date().toDateString();
        });
        
        const commercials = today.filter(l => l.contentClass === 'commercial');
        const promos = today.filter(l => l.contentClass === 'promo');
        const programs = today.filter(l => l.contentClass === 'program');
        
        return {
            totalToday: today.length,
            commercials: commercials.length,
            promos: promos.length,
            programs: programs.length,
            completionRate: today.length > 0 
                ? (today.filter(l => l.completed).length / today.length) * 100 
                : 100,
        };
    }, [playLogs]);
    
    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h2 className={styles.title}>
                    <BarChart2 size={20} />
                    Traffic Manager
                </h2>
                
                <div className={styles.currentContext}>
                    <span className={styles.daypartBadge}>
                        {getDaypartDisplay(currentContext.daypart)}
                    </span>
                    <span className={styles.hourBadge}>
                        Hour {currentContext.hour}
                    </span>
                </div>
            </div>
            
            {/* Summary Cards */}
            <div className={styles.summaryCards}>
                <div className={styles.summaryCard}>
                    <div className={styles.summaryLabel}>Today's Plays</div>
                    <div className={styles.summaryValue}>{summaryStats.totalToday}</div>
                </div>
                <div className={styles.summaryCard}>
                    <div className={styles.summaryLabel}>Commercials</div>
                    <div className={styles.summaryValue}>{summaryStats.commercials}</div>
                </div>
                <div className={styles.summaryCard}>
                    <div className={styles.summaryLabel}>Promos</div>
                    <div className={styles.summaryValue}>{summaryStats.promos}</div>
                </div>
                <div className={styles.summaryCard}>
                    <div className={styles.summaryLabel}>Completion</div>
                    <div className={styles.summaryValue}>{summaryStats.completionRate.toFixed(0)}%</div>
                </div>
            </div>
            
            {/* Tabs */}
            <div className={styles.tabs}>
                <button
                    className={`${styles.tab} ${activeTab === 'pool' ? styles.active : ''}`}
                    onClick={() => setActiveTab('pool')}
                >
                    Ad Pool ({adPool.length})
                </button>
                <button
                    className={`${styles.tab} ${activeTab === 'stats' ? styles.active : ''}`}
                    onClick={() => setActiveTab('stats')}
                >
                    Delivery Stats
                </button>
                <button
                    className={`${styles.tab} ${activeTab === 'log' ? styles.active : ''}`}
                    onClick={() => setActiveTab('log')}
                >
                    Play Log
                </button>
            </div>
            
            {/* Content */}
            <div className={styles.content}>
                {activeTab === 'pool' && (
                    <div className={styles.poolView}>
                        <div className={styles.poolHeader}>
                            <span className={styles.poolHeaderLabel}>
                                Probabilistic selection weights
                            </span>
                            <select 
                                className={styles.sortSelect}
                                value={sortBy}
                                onChange={(e) => setSortBy(e.target.value as any)}
                            >
                                <option value="weight">Sort by Weight</option>
                                <option value="plays">Sort by Plays</option>
                                <option value="delivery">Sort by Delivery</option>
                            </select>
                        </div>
                        
                        <div className={styles.poolList}>
                            {sortedPool.length === 0 ? (
                                <div className={styles.emptyPool}>
                                    No ads in pool. Import media with type "ad" or "commercial".
                                </div>
                            ) : (
                                sortedPool.map(({ item, weight, eligible, reason, probability, stats, deliveryRate }) => (
                                    <div 
                                        key={item.id} 
                                        className={`${styles.poolItem} ${!eligible ? styles.ineligible : ''}`}
                                    >
                                        <div className={styles.poolItemMain}>
                                            <div className={styles.poolItemInfo}>
                                                <span className={styles.poolItemTitle}>{item.title}</span>
                                                <span className={styles.poolItemMeta}>
                                                    {item.duration}s • {item.clientId || 'No client'}
                                                </span>
                                            </div>
                                            
                                            <div className={styles.poolItemStats}>
                                                <div className={styles.statBox}>
                                                    <span className={styles.statLabel}>Weight</span>
                                                    <span className={styles.statValue}>{weight.toFixed(0)}</span>
                                                </div>
                                                <div className={styles.statBox}>
                                                    <span className={styles.statLabel}>Prob</span>
                                                    <span className={styles.statValue}>{probability.toFixed(1)}%</span>
                                                </div>
                                                <div className={styles.statBox}>
                                                    <span className={styles.statLabel}>Plays</span>
                                                    <span className={styles.statValue}>{stats?.totalPlays || 0}</span>
                                                </div>
                                                <div className={styles.statBox}>
                                                    <span className={styles.statLabel}>Delivery</span>
                                                    <span className={`${styles.statValue} ${deliveryRate >= 100 ? styles.complete : deliveryRate < 50 ? styles.behind : ''}`}>
                                                        {deliveryRate.toFixed(0)}%
                                                    </span>
                                                </div>
                                            </div>
                                        </div>
                                        
                                        {/* Probability bar */}
                                        <div className={styles.probBar}>
                                            <div 
                                                className={styles.probBarFill}
                                                style={{ width: `${probability}%` }}
                                            />
                                        </div>
                                        
                                        {!eligible && (
                                            <div className={styles.ineligibleReason}>
                                                <AlertTriangle size={12} />
                                                {reason}
                                            </div>
                                        )}
                                    </div>
                                ))
                            )}
                        </div>
                    </div>
                )}
                
                {activeTab === 'stats' && (
                    <div className={styles.statsView}>
                        <div className={styles.statsGrid}>
                            {Array.from(playStats.values())
                                .sort((a, b) => b.totalPlays - a.totalPlays)
                                .slice(0, 20)
                                .map(stat => {
                                    const item = adPool.find(a => a.id === stat.mediaId);
                                    return (
                                        <div key={stat.mediaId} className={styles.statCard}>
                                            <div className={styles.statCardHeader}>
                                                <span className={styles.statCardTitle}>
                                                    {item?.title || stat.mediaId}
                                                </span>
                                                <span className={styles.statCardPlays}>
                                                    {stat.totalPlays} plays
                                                </span>
                                            </div>
                                            
                                            <div className={styles.statCardBody}>
                                                <div className={styles.statRow}>
                                                    <span>Today</span>
                                                    <span>{stat.todayPlays}</span>
                                                </div>
                                                <div className={styles.statRow}>
                                                    <span>This Hour</span>
                                                    <span>{stat.thisHourPlays}</span>
                                                </div>
                                                <div className={styles.statRow}>
                                                    <span>Completion</span>
                                                    <span>{(stat.avgCompletionRate * 100).toFixed(0)}%</span>
                                                </div>
                                                <div className={styles.statRow}>
                                                    <span>Last Played</span>
                                                    <span>
                                                        {stat.lastPlayed 
                                                            ? new Date(stat.lastPlayed).toLocaleTimeString()
                                                            : 'Never'}
                                                    </span>
                                                </div>
                                            </div>
                                            
                                            {/* Daypart breakdown */}
                                            <div className={styles.daypartBreakdown}>
                                                {Object.entries(stat.playsByDaypart).map(([dp, count]) => (
                                                    <div key={dp} className={styles.daypartChip}>
                                                        <span>{dp}</span>
                                                        <span>{count}</span>
                                                    </div>
                                                ))}
                                            </div>
                                        </div>
                                    );
                                })}
                        </div>
                    </div>
                )}
                
                {activeTab === 'log' && (
                    <div className={styles.logView}>
                        <div className={styles.logList}>
                            {playLogs.length === 0 ? (
                                <div className={styles.emptyLog}>No play history yet.</div>
                            ) : (
                                playLogs
                                    .slice()
                                    .reverse()
                                    .slice(0, 100)
                                    .map(log => (
                                        <div key={log.id} className={styles.logItem}>
                                            <div className={styles.logTime}>
                                                {new Date(log.timestamp).toLocaleTimeString()}
                                            </div>
                                            <div className={`${styles.logClass} ${styles[log.contentClass]}`}>
                                                {log.contentClass}
                                            </div>
                                            <div className={styles.logMedia}>
                                                {log.mediaId}
                                            </div>
                                            <div className={styles.logDuration}>
                                                {log.duration}s
                                            </div>
                                            <div className={styles.logPosition}>
                                                {log.position}
                                            </div>
                                            <div className={`${styles.logStatus} ${log.completed ? styles.complete : styles.incomplete}`}>
                                                {log.completed ? <CheckCircle size={12} /> : <XCircle size={12} />}
                                            </div>
                                        </div>
                                    ))
                            )}
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

