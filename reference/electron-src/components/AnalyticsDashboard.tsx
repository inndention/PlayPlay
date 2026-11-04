import React, { useEffect, useMemo, useState } from 'react';
import { BarChart2, Clock3, Database, FolderOpen, Layers, Play, RefreshCw } from 'lucide-react';
import type { AnalyticsEventRecord, AnalyticsSummary, MediaItem, MediaStats } from '../lib/types';
import { loadLibraryMedia, loadLocalMedia, loadMediaStats } from '../lib/storage';
import { dbFetchMedia, dbGetAnalyticsSummary } from '../lib/mediaDbClient';
import styles from './AnalyticsDashboard.module.css';

type AnalyticsTab = 'overview' | 'playout' | 'filehub' | 'cg';

interface AnalyticsDashboardProps {
    focus?: Record<string, unknown> | null;
}

interface SummaryCard {
    id: string;
    label: string;
    value: number;
}

const TAB_TO_FAMILY: Record<Exclude<AnalyticsTab, 'overview'>, string> = {
    playout: 'playout',
    filehub: 'filehub',
    cg: 'cg',
};

function hasAnalyticsData(summary: AnalyticsSummary | null | undefined) {
    if (!summary) {
        return false;
    }
    return Object.values(summary.totalsByFamily || {}).some((total) => total > 0);
}

function detectFocusTab(focus?: Record<string, unknown> | null): AnalyticsTab {
    const targetType = String(focus?.targetType || focus?.family || '');
    if (targetType === 'starfile' || targetType === 'filehub') {
        return 'filehub';
    }
    if (targetType === 'cg' || targetType === 'cg_template') {
        return 'cg';
    }
    if (targetType === 'playlist' || targetType === 'media' || targetType === 'playout' || targetType === 'hour' || targetType === 'schedule') {
        return 'playout';
    }
    return 'overview';
}

function detectFocusEntityId(focus?: Record<string, unknown> | null) {
    const targetType = String(focus?.targetType || focus?.family || '');
    if (targetType === 'hour' || targetType === 'schedule') {
        return '';
    }
    return String(focus?.targetId || focus?.entityId || '');
}

function formatBucket(bucket: string) {
    const date = new Date(bucket);
    if (Number.isNaN(date.getTime())) {
        return bucket;
    }
    return `${date.toLocaleDateString()} ${date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}`;
}

function formatRelativeTimestamp(timestamp: string) {
    const date = new Date(timestamp);
    if (Number.isNaN(date.getTime())) {
        return timestamp;
    }
    return date.toLocaleString();
}

function buildLegacySummary(allStats: Record<string, MediaStats>, mediaItems: MediaItem[]): AnalyticsSummary {
    const mediaById = new Map(mediaItems.map((item) => [item.id, item]));
    const totalsByAction: AnalyticsSummary['totalsByAction'] = {
        playout: [],
    };
    const timeBucketMap = new Map<string, number>();
    const recentEvents: AnalyticsEventRecord[] = [];
    const topEntities = Object.entries(allStats)
        .map(([mediaId, stats]) => ({
            entityId: mediaId,
            entityLabel: mediaById.get(mediaId)?.title || mediaId,
            total: stats.playCount,
        }))
        .filter((entry) => entry.total > 0)
        .sort((left, right) => right.total - left.total)
        .slice(0, 24);

    let totalPlays = 0;

    Object.entries(allStats).forEach(([mediaId, stats]) => {
        totalPlays += stats.playCount;
        stats.playHistory.forEach((event, index) => {
            const bucket = new Date(event.timestamp);
            bucket.setMinutes(0, 0, 0);
            const bucketKey = bucket.toISOString();
            timeBucketMap.set(bucketKey, (timeBucketMap.get(bucketKey) || 0) + 1);

            recentEvents.push({
                id: Number(`${event.timestamp}${index}`),
                family: 'playout',
                action: 'legacy_play',
                entityId: mediaId,
                entityType: 'media',
                entityLabel: mediaById.get(mediaId)?.title || mediaId,
                createdAt: new Date(event.timestamp).toISOString(),
                payload: {
                    duration: event.duration,
                    legacy: true,
                },
            });
        });
    });

    totalsByAction.playout.push({
        action: 'legacy_play',
        total: totalPlays,
    });

    return {
        totalsByFamily: {
            playout: totalPlays,
        },
        totalsByAction,
        topEntities: {
            playout: topEntities,
        },
        timeBuckets: Array.from(timeBucketMap.entries())
            .map(([bucket, total]) => ({
                bucket,
                family: 'playout',
                total,
            }))
            .sort((left, right) => right.bucket.localeCompare(left.bucket)),
        recentEvents: recentEvents
            .sort((left, right) => right.createdAt.localeCompare(left.createdAt))
            .slice(0, 200),
    };
}

function flattenActionTotals(summary: AnalyticsSummary, families: string[]) {
    return families
        .flatMap((family) => (summary.totalsByAction[family] || []).map((entry) => ({ ...entry, family })))
        .sort((left, right) => right.total - left.total);
}

function flattenTopEntities(summary: AnalyticsSummary, families: string[]) {
    return families
        .flatMap((family) => (summary.topEntities[family] || []).map((entry) => ({ ...entry, family })))
        .sort((left, right) => right.total - left.total);
}

export function AnalyticsDashboard({ focus }: AnalyticsDashboardProps) {
    const [summary, setSummary] = useState<AnalyticsSummary | null>(null);
    const [legacySummary, setLegacySummary] = useState<AnalyticsSummary | null>(null);
    const [loading, setLoading] = useState(true);
    const [activeTab, setActiveTab] = useState<AnalyticsTab>(() => detectFocusTab(focus));
    const [selectedAction, setSelectedAction] = useState('all');
    const [selectedEntityId, setSelectedEntityId] = useState(() => detectFocusEntityId(focus));

    const loadSummary = async () => {
        setLoading(true);
        try {
            const dbSummary = await dbGetAnalyticsSummary();
            setSummary(dbSummary);

            if (!hasAnalyticsData(dbSummary)) {
                const [dbMedia, libraryMedia, localMedia] = await Promise.all([
                    dbFetchMedia().catch(() => [] as MediaItem[]),
                    Promise.resolve(loadLibraryMedia([])),
                    Promise.resolve(loadLocalMedia()),
                ]);
                const mergedMedia = [...dbMedia, ...libraryMedia, ...localMedia].filter(Boolean);
                const deduped = Array.from(new Map(mergedMedia.map((item) => [item.id, item])).values());
                setLegacySummary(buildLegacySummary(loadMediaStats(), deduped));
            } else {
                setLegacySummary(null);
            }
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        void loadSummary();
    }, []);

    useEffect(() => {
        setActiveTab(detectFocusTab(focus));
        setSelectedEntityId(detectFocusEntityId(focus));
    }, [focus]);

    const effectiveSummary = hasAnalyticsData(summary) ? summary : legacySummary;
    const usingLegacyFallback = !hasAnalyticsData(summary) && hasAnalyticsData(legacySummary);

    const activeFamilies = useMemo(() => {
        if (!effectiveSummary) {
            return [];
        }
        if (activeTab === 'overview') {
            return Object.keys(effectiveSummary.totalsByFamily);
        }
        return [TAB_TO_FAMILY[activeTab]];
    }, [activeTab, effectiveSummary]);

    const summaryCards = useMemo<SummaryCard[]>(() => {
        const totals = effectiveSummary?.totalsByFamily || {};
        return [
            { id: 'all', label: 'All Events', value: Object.values(totals).reduce((sum, total) => sum + total, 0) },
            { id: 'playout', label: 'Playout', value: totals.playout || 0 },
            { id: 'filehub', label: 'File Hub', value: totals.filehub || 0 },
            { id: 'cg', label: 'CG', value: totals.cg || 0 },
        ];
    }, [effectiveSummary]);

    const actionBreakdown = useMemo(
        () => effectiveSummary ? flattenActionTotals(effectiveSummary, activeFamilies) : [],
        [activeFamilies, effectiveSummary],
    );

    const topEntities = useMemo(
        () => effectiveSummary ? flattenTopEntities(effectiveSummary, activeFamilies) : [],
        [activeFamilies, effectiveSummary],
    );

    const bucketBreakdown = useMemo(() => {
        if (!effectiveSummary) {
            return [];
        }
        const buckets = new Map<string, number>();
        effectiveSummary.timeBuckets
            .filter((bucket) => activeFamilies.includes(bucket.family))
            .forEach((bucket) => {
                buckets.set(bucket.bucket, (buckets.get(bucket.bucket) || 0) + bucket.total);
            });

        return Array.from(buckets.entries())
            .map(([bucket, total]) => ({ bucket, total }))
            .sort((left, right) => right.bucket.localeCompare(left.bucket))
            .slice(0, 24);
    }, [activeFamilies, effectiveSummary]);

    const filteredEvents = useMemo(() => {
        if (!effectiveSummary) {
            return [];
        }
        return effectiveSummary.recentEvents.filter((event) => {
            if (!activeFamilies.includes(event.family)) {
                return false;
            }
            if (selectedAction !== 'all' && event.action !== selectedAction) {
                return false;
            }
            if (selectedEntityId && event.entityId !== selectedEntityId) {
                return false;
            }
            return true;
        });
    }, [activeFamilies, effectiveSummary, selectedAction, selectedEntityId]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <BarChart2 size={18} />
                    <div>
                        <strong>Operational Analytics</strong>
                        <span>SQLite-backed playout, file hub, and CG activity</span>
                    </div>
                </div>
                <div className={styles.headerActions}>
                    {usingLegacyFallback && <span className={styles.legacyBadge}>Legacy play stats fallback</span>}
                    <button type="button" className={styles.refreshButton} onClick={() => void loadSummary()}>
                        <RefreshCw size={14} />
                        Refresh
                    </button>
                </div>
            </div>

            <div className={styles.tabBar}>
                {(['overview', 'playout', 'filehub', 'cg'] as AnalyticsTab[]).map((tab) => (
                    <button
                        key={tab}
                        type="button"
                        className={`${styles.tabButton} ${activeTab === tab ? styles.tabButtonActive : ''}`}
                        onClick={() => {
                            setActiveTab(tab);
                            setSelectedAction('all');
                            setSelectedEntityId('');
                        }}
                    >
                        {tab === 'overview' ? 'Overview' : tab === 'filehub' ? 'File Hub' : tab.toUpperCase()}
                    </button>
                ))}
            </div>

            <div className={styles.summaryGrid}>
                {summaryCards.map((card) => (
                    <div key={card.id} className={styles.summaryCard}>
                        <div className={styles.summaryIcon}>
                            {card.id === 'playout' && <Play size={16} />}
                            {card.id === 'filehub' && <FolderOpen size={16} />}
                            {card.id === 'cg' && <Layers size={16} />}
                            {card.id === 'all' && <Database size={16} />}
                        </div>
                        <div>
                            <span className={styles.summaryLabel}>{card.label}</span>
                            <strong className={styles.summaryValue}>{card.value}</strong>
                        </div>
                    </div>
                ))}
            </div>

            <div className={styles.filterRow}>
                <label className={styles.filterField}>
                    <span>Action Type</span>
                    <select value={selectedAction} onChange={(event) => setSelectedAction(event.target.value)}>
                        <option value="all">All actions</option>
                        {actionBreakdown.map((entry) => (
                            <option key={`${entry.family}-${entry.action}`} value={entry.action}>
                                {entry.action} ({entry.total})
                            </option>
                        ))}
                    </select>
                </label>
                <label className={styles.filterField}>
                    <span>Asset / Template</span>
                    <select value={selectedEntityId} onChange={(event) => setSelectedEntityId(event.target.value)}>
                        <option value="">All assets</option>
                        {topEntities.map((entry) => (
                            <option key={`${entry.family}-${entry.entityId || entry.entityLabel}`} value={entry.entityId || ''}>
                                {entry.entityLabel} ({entry.total})
                            </option>
                        ))}
                    </select>
                </label>
            </div>

            {loading && <div className={styles.emptyState}>Loading analytics…</div>}
            {!loading && !effectiveSummary && <div className={styles.emptyState}>No analytics data is available yet.</div>}

            {!loading && effectiveSummary && (
                <>
                    <div className={styles.panelGrid}>
                        <section className={styles.panel}>
                            <div className={styles.panelHeader}>
                                <Play size={14} />
                                <span>Action Breakdown</span>
                            </div>
                            <div className={styles.metricList}>
                                {actionBreakdown.slice(0, 8).map((entry) => (
                                    <div key={`${entry.family}-${entry.action}`} className={styles.metricRow}>
                                        <span>{entry.action}</span>
                                        <strong>{entry.total}</strong>
                                    </div>
                                ))}
                                {actionBreakdown.length === 0 && <div className={styles.emptyStateSmall}>No action data.</div>}
                            </div>
                        </section>

                        <section className={styles.panel}>
                            <div className={styles.panelHeader}>
                                <Layers size={14} />
                                <span>Top Assets / Templates</span>
                            </div>
                            <div className={styles.metricList}>
                                {topEntities.slice(0, 8).map((entry) => (
                                    <button
                                        key={`${entry.family}-${entry.entityId || entry.entityLabel}`}
                                        type="button"
                                        className={`${styles.metricRowButton} ${selectedEntityId === (entry.entityId || '') ? styles.metricRowButtonActive : ''}`}
                                        onClick={() => setSelectedEntityId(entry.entityId || '')}
                                    >
                                        <span>{entry.entityLabel}</span>
                                        <strong>{entry.total}</strong>
                                    </button>
                                ))}
                                {topEntities.length === 0 && <div className={styles.emptyStateSmall}>No entity data.</div>}
                            </div>
                        </section>

                        <section className={styles.panel}>
                            <div className={styles.panelHeader}>
                                <Clock3 size={14} />
                                <span>Time Buckets</span>
                            </div>
                            <div className={styles.metricList}>
                                {bucketBreakdown.slice(0, 8).map((entry) => (
                                    <div key={entry.bucket} className={styles.metricRow}>
                                        <span>{formatBucket(entry.bucket)}</span>
                                        <strong>{entry.total}</strong>
                                    </div>
                                ))}
                                {bucketBreakdown.length === 0 && <div className={styles.emptyStateSmall}>No bucket data.</div>}
                            </div>
                        </section>
                    </div>

                    <section className={styles.eventsPanel}>
                        <div className={styles.panelHeader}>
                            <Database size={14} />
                            <span>Recent Events</span>
                        </div>
                        <div className={styles.eventsTable}>
                            <div className={styles.eventsHeader}>
                                <span>Time</span>
                                <span>Family</span>
                                <span>Action</span>
                                <span>Entity</span>
                            </div>
                            {filteredEvents.slice(0, 40).map((event) => (
                                <div key={event.id} className={styles.eventsRow}>
                                    <span>{formatRelativeTimestamp(event.createdAt)}</span>
                                    <span>{event.family}</span>
                                    <span>{event.action}</span>
                                    <span>{event.entityLabel || event.entityId || '—'}</span>
                                </div>
                            ))}
                            {filteredEvents.length === 0 && (
                                <div className={styles.emptyState}>No events match the current drill-down.</div>
                            )}
                        </div>
                    </section>
                </>
            )}
        </div>
    );
}
