import React, { useState, useMemo, useCallback } from 'react';
import { Grid, Calendar, Copy, RefreshCw, Merge } from 'lucide-react';
import { HourGridCell, SiteLocation } from '../lib/types';
import { loadWeeklySchedule, loadClients } from '../lib/storage';
import { HourOperationsMenu } from './HourOperationsMenu';
import styles from './MultiGridView.module.css';

const SITE_LOCATIONS: SiteLocation[] = [
    { id: 'wgsr', name: 'WGSR', type: 'radio' },
    { id: 'wmdv', name: 'WMDV', type: 'radio' },
    { id: 'wyat', name: 'WYAT', type: 'radio' },
    { id: 'starnews', name: 'StarNews.plus', type: 'website' },
    { id: 'happyradio', name: 'HappyRadio.plus', type: 'website' },
    { id: 'remote-node', name: 'Remote Node', type: 'website' },
];

const DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

interface MultiGridViewProps {
    onHourClick?: (siteId: string, day: number, hour: number) => void;
    onHourContextMenu?: (e: React.MouseEvent, siteId: string, day: number, hour: number) => void;
}

export function MultiGridView({ onHourClick, onHourContextMenu }: MultiGridViewProps) {
    const [viewMode, setViewMode] = useState<'matrix' | 'tabbed'>('matrix');
    const [selectedSite, setSelectedSite] = useState<string | null>(null);
    const [schedule, setSchedule] = useState<any>(null);
    const [contextMenu, setContextMenu] = useState<{ x: number; y: number; siteId: string; day: number; hour: number } | null>(null);

    React.useEffect(() => {
        loadWeeklySchedule().then(setSchedule);
    }, []);

    const trafficMaxAdSlotsPerHour = useMemo(() => {
        const clients = loadClients();
        const caps = clients
            .filter(c => c.status !== 'cancelled' && c.status !== 'paused')
            .map(c => c.rules?.maxPlaysPerHour ?? 0)
            .filter((n): n is number => typeof n === 'number' && n > 0);
        return caps.length ? Math.max(...caps) : 0;
    }, [schedule]);

    const calculateHourData = useCallback((siteId: string, day: number, hour: number): HourGridCell => {
        if (!schedule) {
            return {
                siteId,
                day,
                hour,
                filledDuration: 0,
                itemCount: 0,
                adCount: 0,
                adDurationSec: 0,
                targetAdSlotsPerHour: trafficMaxAdSlotsPerHour,
                adSlotCoveragePct: null,
                isEmpty: true,
            };
        }

        const slotId = `${day}-${hour}`;
        const slot = schedule.slots[slotId];
        
        if (!slot || !slot.items) {
            return {
                siteId,
                day,
                hour,
                filledDuration: 0,
                itemCount: 0,
                adCount: 0,
                adDurationSec: 0,
                targetAdSlotsPerHour: trafficMaxAdSlotsPerHour,
                adSlotCoveragePct: null,
                isEmpty: true,
            };
        }

        const filledDuration = slot.items.reduce((sum: number, item: any) => {
            return sum + (item.media?.duration || 0);
        }, 0);

        const adItems = slot.items.filter((item: any) => item.media?.type === 'ad');
        const adCount = adItems.length;
        const adDurationSec = adItems.reduce(
            (sum: number, item: any) => sum + (item.media?.duration || 0),
            0,
        );
        const target = trafficMaxAdSlotsPerHour;
        const adSlotCoveragePct =
            target > 0 ? Math.min(100, Math.round((100 * adCount) / target)) : null;

        return {
            siteId,
            day,
            hour,
            filledDuration,
            itemCount: slot.items.length,
            adCount,
            adDurationSec,
            targetAdSlotsPerHour: target,
            adSlotCoveragePct,
            isEmpty: slot.items.length === 0,
        };
    }, [schedule, trafficMaxAdSlotsPerHour]);

    const getFillPercentage = (cell: HourGridCell): number => {
        const maxDuration = 3600; // 1 hour in seconds
        return Math.min(100, (cell.filledDuration / maxDuration) * 100);
    };

    const getFillColor = (percentage: number): string => {
        if (percentage === 0) return 'transparent';
        if (percentage < 50) return '#ef4444'; // Red - underfilled
        if (percentage < 90) return '#f59e0b'; // Yellow - partially filled
        if (percentage < 100) return '#10b981'; // Green - well filled
        return '#3b82f6'; // Blue - overfilled
    };

    const handleHourDoubleClick = useCallback((siteId: string, day: number, hour: number) => {
        if (onHourClick) {
            onHourClick(siteId, day, hour);
        }
    }, [onHourClick]);

    const renderMatrixView = () => {
        return (
            <div className={styles.matrixContainer}>
                <div className={styles.gridHeader}>
                    <div className={styles.cornerCell}></div>
                    {SITE_LOCATIONS.map(site => (
                        <div key={site.id} className={styles.siteHeader}>
                            {site.name}
                        </div>
                    ))}
                </div>
                <div className={styles.gridBody}>
                    {Array.from({ length: 7 }, (_, day) => (
                        <React.Fragment key={day}>
                            {Array.from({ length: 24 }, (_, hour) => (
                                <React.Fragment key={hour}>
                                    {hour === 0 && (
                                        <div className={styles.dayLabel}>{DAYS[day]}</div>
                                    )}
                                    {SITE_LOCATIONS.map(site => {
                                        const cell = calculateHourData(site.id, day, hour);
                                        const fillPct = getFillPercentage(cell);
                                        const fillColor = getFillColor(fillPct);
                                        
                                        return (
                                            <div
                                                key={`${site.id}-${day}-${hour}`}
                                                className={`${styles.hourCell} ${cell.isEmpty ? styles.empty : ''}`}
                                                style={{
                                                    background: `linear-gradient(to top, ${fillColor} ${fillPct}%, transparent ${fillPct}%)`,
                                                }}
                                                onDoubleClick={() => handleHourDoubleClick(site.id, day, hour)}
                                                onContextMenu={(e) => {
                                                    e.preventDefault();
                                                    setContextMenu({ x: e.clientX, y: e.clientY, siteId: site.id, day, hour });
                                                }}
                                                title={`${site.name} - ${DAYS[day]} ${hour}:00 - ${cell.itemCount} items, ${Math.round(fillPct)}% filled`}
                                            >
                                                <div className={styles.cellContent}>
                                                    <div className={styles.cellTime}>{hour}:00</div>
                                                    {!cell.isEmpty && (
                                                        <>
                                                            <div className={styles.cellCount}>{cell.itemCount}</div>
                                                            {cell.adCount > 0 && (
                                                                <div className={styles.cellAds}>{cell.adCount} ads</div>
                                                            )}
                                                        </>
                                                    )}
                                                </div>
                                            </div>
                                        );
                                    })}
                                </React.Fragment>
                            ))}
                        </React.Fragment>
                    ))}
                </div>
            </div>
        );
    };

    const renderTabbedView = () => {
        const sitesToShow = selectedSite 
            ? SITE_LOCATIONS.filter(s => s.id === selectedSite)
            : SITE_LOCATIONS;

        return (
            <div className={styles.tabbedContainer}>
                <div className={styles.siteTabs}>
                    <button
                        className={`${styles.siteTab} ${selectedSite === null ? styles.activeTab : ''}`}
                        onClick={() => setSelectedSite(null)}
                    >
                        All Sites
                    </button>
                    {SITE_LOCATIONS.map(site => (
                        <button
                            key={site.id}
                            className={`${styles.siteTab} ${selectedSite === site.id ? styles.activeTab : ''}`}
                            onClick={() => setSelectedSite(site.id)}
                        >
                            {site.name}
                        </button>
                    ))}
                </div>
                <div className={styles.tabbedGrid}>
                    {sitesToShow.map(site => (
                        <div key={site.id} className={styles.siteGrid}>
                            <h3 className={styles.siteTitle}>{site.name}</h3>
                            <div className={styles.hoursGrid}>
                                {Array.from({ length: 7 }, (_, day) => (
                                    <div key={day} className={styles.dayColumn}>
                                        <div className={styles.dayHeader}>{DAYS[day]}</div>
                                        {Array.from({ length: 24 }, (_, hour) => {
                                            const cell = calculateHourData(site.id, day, hour);
                                            const fillPct = getFillPercentage(cell);
                                            const fillColor = getFillColor(fillPct);
                                            
                                            return (
                                                <div
                                                    key={hour}
                                                    className={`${styles.hourCell} ${cell.isEmpty ? styles.empty : ''}`}
                                                    style={{
                                                        background: `linear-gradient(to top, ${fillColor} ${fillPct}%, transparent ${fillPct}%)`,
                                                    }}
                                                    onDoubleClick={() => handleHourDoubleClick(site.id, day, hour)}
                                                    onContextMenu={(e) => {
                                                    e.preventDefault();
                                                    setContextMenu({ x: e.clientX, y: e.clientY, siteId: site.id, day, hour });
                                                }}
                                                    title={`${hour}:00 - ${cell.itemCount} items, ${Math.round(fillPct)}% filled`}
                                                >
                                                    <div className={styles.cellTime}>{hour}</div>
                                                    {!cell.isEmpty && (
                                                        <div className={styles.cellCount}>{cell.itemCount}</div>
                                                    )}
                                                </div>
                                            );
                                        })}
                                    </div>
                                ))}
                            </div>
                        </div>
                    ))}
                </div>
            </div>
        );
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>
                    <Grid size={18} />
                    Multi-Site Grid View
                </div>
                <div className={styles.controls}>
                    <button
                        className={`${styles.viewBtn} ${viewMode === 'matrix' ? styles.active : ''}`}
                        onClick={() => setViewMode('matrix')}
                    >
                        Matrix
                    </button>
                    <button
                        className={`${styles.viewBtn} ${viewMode === 'tabbed' ? styles.active : ''}`}
                        onClick={() => setViewMode('tabbed')}
                    >
                        Tabbed
                    </button>
                </div>
            </div>
            <div className={styles.content}>
                {viewMode === 'matrix' ? renderMatrixView() : renderTabbedView()}
            </div>
            {contextMenu && (
                <HourOperationsMenu
                    x={contextMenu.x}
                    y={contextMenu.y}
                    siteId={contextMenu.siteId}
                    day={contextMenu.day}
                    hour={contextMenu.hour}
                    onClose={() => setContextMenu(null)}
                    onCopy={(siteId, day, hour) => {
                        console.log('Copy hour:', siteId, day, hour);
                    }}
                    onRedeployAds={(siteId, day, hour) => {
                        console.log('Redeploy ads:', siteId, day, hour);
                    }}
                    onFillWithIds={(siteId, day, hour) => {
                        console.log('Fill with IDs:', siteId, day, hour);
                    }}
                    onFillWithBumpers={(siteId, day, hour) => {
                        console.log('Fill with bumpers:', siteId, day, hour);
                    }}
                    onRecalculateAds={(siteId, day, hour) => {
                        console.log('Recalculate ads:', siteId, day, hour);
                    }}
                    onMerge={(siteId, day, hour) => {
                        console.log('Merge hours:', siteId, day, hour);
                    }}
                />
            )}
        </div>
    );
}

