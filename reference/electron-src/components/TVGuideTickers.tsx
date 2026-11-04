import React, { useState, useEffect, useRef, useMemo } from 'react';
import { TapeFeedItem, SiteLocation } from '../lib/types';
import { loadWeeklySchedule } from '../lib/storage';
import { Radio, Globe } from 'lucide-react';
import styles from './TVGuideTickers.module.css';

const SITE_LOCATIONS: SiteLocation[] = [
    { id: 'wgsr', name: 'WGSR', type: 'radio' },
    { id: 'wmdv', name: 'WMDV', type: 'radio' },
    { id: 'wyat', name: 'WYAT', type: 'radio' },
    { id: 'starnews', name: 'StarNews.plus', type: 'website' },
    { id: 'happyradio', name: 'HappyRadio.plus', type: 'website' },
    { id: 'remote-node', name: 'Remote Node', type: 'website' },
];

export function TVGuideTickers() {
    const [now, setNow] = useState(Date.now());
    const [schedule, setSchedule] = useState<any>(null);
    const [zoom, setZoom] = useState(2); // minutes per pixel
    const tickerRefs = useRef<Map<string, HTMLDivElement>>(new Map());

    useEffect(() => {
        loadWeeklySchedule().then(setSchedule);
    }, []);

    // Update time less frequently to reduce re-renders
    useEffect(() => {
        const interval = setInterval(() => {
            setNow(Date.now());
        }, 1000); // Update every second instead of every 100ms
        return () => clearInterval(interval);
    }, []);

    const buildTapeFeed = (siteId: string): TapeFeedItem[] => {
        if (!schedule) return [];

        const today = new Date(now);
        const day = today.getDay();
        const items: TapeFeedItem[] = [];

        // Build feed for next 4 hours
        for (let hourOffset = -1; hourOffset < 4; hourOffset++) {
            const targetHour = today.getHours() + hourOffset;
            const slotId = `${day}-${targetHour}`;
            const slot = schedule.slots[slotId];

            if (slot && slot.items) {
                let currentTime = new Date(today);
                currentTime.setHours(targetHour, 0, 0, 0);

                slot.items.forEach((item: any, idx: number) => {
                    const startTime = currentTime.getTime();
                    const duration = (item.media?.duration || 0) * 1000;
                    const endTime = startTime + duration;

                    items.push({
                        id: `${item.id}_${idx}`,
                        siteId,
                        startTime,
                        endTime,
                        title: item.media?.title || 'Unknown',
                        type: item.media?.type || 'gap',
                        isPlaying: startTime <= now && endTime > now,
                        isCompliant: true, // TODO: Check compliance
                        complianceIssues: [],
                    });

                    currentTime = new Date(endTime);
                });
            }
        }

        return items;
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>TV Guide Tickers</div>
                <div className={styles.controls}>
                    <button onClick={() => setZoom(Math.max(1, zoom - 0.5))}>-</button>
                    <span>{zoom} min/px</span>
                    <button onClick={() => setZoom(Math.min(10, zoom + 0.5))}>+</button>
                </div>
            </div>
            <div className={styles.tickers}>
                {SITE_LOCATIONS.map(site => {
                    const feed = buildTapeFeed(site.id);
                    const pxPerMin = zoom;
                    const windowStart = now - 60 * 60 * 1000; // 1 hour ago
                    const windowEnd = now + 4 * 60 * 60 * 1000; // 4 hours ahead

                    return (
                        <div key={site.id} className={styles.tickerRow}>
                            <div className={styles.siteLabel}>
                                {site.type === 'radio' ? <Radio size={14} /> : <Globe size={14} />}
                                {site.name}
                            </div>
                            <div 
                                ref={(el) => {
                                    if (el) tickerRefs.current.set(site.id, el);
                                }}
                                className={styles.tickerFeed}
                            >
                                {feed
                                    .filter(item => item.endTime > windowStart && item.startTime < windowEnd)
                                    .map(item => {
                                        const x = ((item.startTime - windowStart) / (60 * 1000)) * pxPerMin;
                                        const width = ((item.endTime - item.startTime) / (60 * 1000)) * pxPerMin;
                                        const isCurrent = item.startTime <= now && item.endTime > now;

                                        return (
                                            <div
                                                key={item.id}
                                                className={`${styles.feedItem} ${isCurrent ? styles.playing : ''} ${!item.isCompliant ? styles.nonCompliant : ''}`}
                                                style={{
                                                    left: `${x}px`,
                                                    width: `${Math.max(20, width)}px`,
                                                }}
                                                title={`${item.title} - ${new Date(item.startTime).toLocaleTimeString()}`}
                                            >
                                                <div className={styles.itemTitle}>{item.title}</div>
                                            </div>
                                        );
                                    })}
                                <div 
                                    className={styles.nowLine}
                                    style={{ left: `${((now - windowStart) / (60 * 1000)) * pxPerMin}px` }}
                                />
                            </div>
                        </div>
                    );
                })}
            </div>
        </div>
    );
}




