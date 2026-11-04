import React, { useEffect, useMemo, useRef, useState } from 'react';
import styles from './ScheduleTicker.module.css';
import { buildDayTimeline, type ScheduleEvent } from '../lib/automationEngine';
import { loadWeeklySchedule, saveWeeklySchedule, loadOBSCommands } from '../lib/storage';
import type { PlaylistItem, WeeklySchedule } from '../lib/types';
import type { ContextMenuItem } from '../lib/types';
import { ContextMenu, useContextMenu } from './ContextMenu';
import { Layers, Radio, Facebook, Youtube } from 'lucide-react';

type ZoomLevel = 'xs' | 'sm' | 'md' | 'lg';

const ZOOM_PX_PER_MIN: Record<ZoomLevel, number> = {
    xs: 8,
    sm: 14,
    md: 22,
    lg: 34,
};

function formatTime(t: number) {
    const d = new Date(t);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function getBadges(item: PlaylistItem): string[] {
    const badges: string[] = [];
    if ((item.cgOnStart?.length || 0) > 0 || (item.cgOnEnd?.length || 0) > 0) badges.push('CG');
    if ((item.obsCommands?.length || 0) > 0) badges.push('OBS');
    if ((item.actions || []).length > 0) badges.push('ACT');
    if ((item.media?.segments || []).length > 0) badges.push('SEG');
    if (item.media?.type === 'live') badges.push('LIVE');
    return badges;
}

function hasOverlayAd(item: PlaylistItem): boolean {
    // Check if item has overlay ad metadata
    // In real implementation, this would check against OverlayAdsScheduler
    return (item as any).overlayAdId !== undefined;
}

function getLiveStatus(): { facebook: boolean; youtube: boolean } {
    // In real implementation, this would check LiveProducerDashboard state
    // For now, return mock data
    return { facebook: false, youtube: false };
}

function typeColor(type: string): string {
    switch (type) {
        case 'ad': return '#f97316';
        case 'promo': return '#a855f7';
        case 'station_id': return '#22c55e';
        case 'bumper': return '#eab308';
        case 'program': return '#3b82f6';
        case 'paid_program': return '#60a5fa';
        case 'image': return '#f472b6';
        case 'video': return '#93c5fd';
        case 'live': return '#ef4444';
        case 'gap': return '#64748b';
        default: return '#94a3b8';
    }
}

function asPlaylistItem(ev: ScheduleEvent): PlaylistItem {
    // buildDayTimeline stores trim points on media object; reconstruct a PlaylistItem-like shape for badges.
    const m: any = ev.media as any;
    return {
        id: ev.id,
        mediaId: ev.mediaId,
        media: ev.media,
        timeIn: m.timeIn,
        timeOut: m.timeOut,
    } as PlaylistItem;
}

interface ScheduleTickerProps {
    scheduleStartTime?: number; // Actual playout schedule start time (for accurate positioning)
    channelId?: string; // Channel ID for multi-channel support
    scheduleId?: string; // Specific schedule ID to load (if different from default)
}

export function ScheduleTicker({ scheduleStartTime, channelId, scheduleId }: ScheduleTickerProps = {}) {
    const [schedule, setSchedule] = useState<WeeklySchedule | null>(null);
    const [now, setNow] = useState<number>(Date.now());
    const [zoom, setZoom] = useState<ZoomLevel>('md');
    const [liveStatus, setLiveStatus] = useState<{ facebook: boolean; youtube: boolean }>({ facebook: false, youtube: false });
    const wrapRef = useRef<HTMLDivElement | null>(null);
    const { menuState, showMenu, hideMenu } = useContextMenu();

    // Load schedule once, and reload on schedule updates
    // For multi-channel, each channel can have its own schedule
    useEffect(() => {
        let mounted = true;
        const load = async () => {
            // If scheduleId is provided, load that specific schedule
            // Otherwise, load the default schedule
            // TODO: Implement channel-specific schedule loading when storage supports it
            const s = await loadWeeklySchedule().catch(() => null);
            if (mounted) setSchedule(s);
        };
        load();
        const onUpd = () => {
            // Only reload if this ticker is for the channel that was updated
            // For now, reload all tickers (can be optimized later)
            load();
        };
        window.addEventListener('schedule-updated', onUpd as any);
        // Listen for channel-specific updates
        if (channelId) {
            const channelEventName = `schedule-updated-${channelId}`;
            window.addEventListener(channelEventName, onUpd as any);
            return () => {
                mounted = false;
                window.removeEventListener('schedule-updated', onUpd as any);
                window.removeEventListener(channelEventName, onUpd as any);
            };
        }
        return () => {
            mounted = false;
            window.removeEventListener('schedule-updated', onUpd as any);
        };
    }, [channelId, scheduleId]);

    // Real-time ticks — wall-clock aligned, drift-corrected.
    // setInterval(500) drifts under GC/throttling; instead we schedule the next
    // tick to fire at the exact next 500ms wall-clock boundary. After hours of
    // running the displayed time stays locked to the system clock.
    useEffect(() => {
        let cancelled = false;
        let timeoutId: number | null = null;
        let tickCount = 0;

        const scheduleNext = () => {
            const now = Date.now();
            // align to next 500ms boundary
            const delay = 500 - (now % 500);
            timeoutId = window.setTimeout(() => {
                if (cancelled) return;
                const t = Date.now();
                setNow(t);
                // liveStatus is expensive — refresh once per ~5s, not every tick
                if (tickCount % 10 === 0) {
                    setLiveStatus(getLiveStatus());
                }
                tickCount++;
                scheduleNext();
            }, delay);
        };

        scheduleNext();
        return () => {
            cancelled = true;
            if (timeoutId !== null) window.clearTimeout(timeoutId);
        };
    }, []);

    // Re-tick on tab visibility return — browser throttles timers in hidden tabs
    useEffect(() => {
        const onVisible = () => {
            if (!document.hidden) {
                setNow(Date.now());
                setLiveStatus(getLiveStatus());
            }
        };
        document.addEventListener('visibilitychange', onVisible);
        return () => document.removeEventListener('visibilitychange', onVisible);
    }, []);

    const pxPerMin = ZOOM_PX_PER_MIN[zoom];

    // Build a rolling window: now-5min .. now+90min
    const windowMs = 90 * 60 * 1000;
    const windowStart = now - 5 * 60 * 1000;
    const windowEnd = now + windowMs;

    const events = useMemo(() => {
        if (!schedule) return [] as ScheduleEvent[];
        const today = new Date();
        const day = today.getDay();
        const timeline = buildDayTimeline(schedule, day, today);
        
        // If scheduleStartTime is provided, adjust event times to match actual playout schedule
        // The playout system uses scheduleStartTime as the reference, so we need to align the ticker
        if (scheduleStartTime) {
            const scheduleHourStart = new Date(scheduleStartTime);
            scheduleHourStart.setMinutes(0, 0, 0);
            const scheduleHour = scheduleHourStart.getHours();
            const scheduleDay = scheduleHourStart.getDay();
            
            // Adjust timeline events to align with actual playout start time
            const adjustedTimeline = timeline.map(ev => {
                const eventDate = new Date(ev.startTime);
                const eventHour = eventDate.getHours();
                const eventDay = eventDate.getDay();
                
                // If this event is in the same day/hour as the schedule start, adjust it
                if (eventDay === scheduleDay && eventHour === scheduleHour) {
                    // Calculate offset from the hour start
                    const eventHourStart = new Date(eventDate);
                    eventHourStart.setMinutes(0, 0, 0);
                    const offsetFromHourStart = ev.startTime - eventHourStart.getTime();
                    
                    // Apply the same offset from scheduleStartTime
                    return {
                        ...ev,
                        startTime: scheduleStartTime + offsetFromHourStart,
                        endTime: scheduleStartTime + offsetFromHourStart + ev.duration
                    };
                }
                
                return ev;
            });
            
            return adjustedTimeline
                .filter(ev => ev.endTime > windowStart && ev.startTime < windowEnd)
                .slice(0, 250);
        }
        
        return timeline
            .filter(ev => ev.endTime > windowStart && ev.startTime < windowEnd)
            .slice(0, 250);
    }, [schedule, windowStart, windowEnd, scheduleStartTime]);

    const attachFacebookPost = async (eventId: string) => {
        const { safePrompt } = await import('../lib/promptPolyfill');
        const message = await safePrompt('Facebook Post message:', '');
        if (!message) return;
        const link = (await safePrompt('Optional link URL:', '')) || undefined;
        const s = await loadWeeklySchedule();
        let updated = false;
        for (const slot of Object.values(s.slots)) {
            const idx = slot.items.findIndex(i => i.id === eventId);
            if (idx >= 0) {
                const item = slot.items[idx];
                const actions = item.actions || [];
                actions.push({
                    id: `fb_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
                    type: 'facebook_post',
                    createdAt: Date.now(),
                    enabled: true,
                    message,
                    link: link?.trim() || undefined,
                });
                slot.items[idx] = { ...item, actions };
                updated = true;
                break;
            }
        }
        if (updated) {
            await saveWeeklySchedule(s);
            window.dispatchEvent(new CustomEvent('schedule-updated', { detail: { slotId: 'ticker' } }));
        }
    };

    const attachOBSCommand = async (eventId: string) => {
        const commands = loadOBSCommands();
        if (commands.length === 0) {
            alert('No OBS commands saved yet. Create them in OBS Control first.');
            return;
        }
        const { safePrompt } = await import('../lib/promptPolyfill');
        const choice = await safePrompt(
            `Enter OBS command number (1-${commands.length}):`,
            '1'
        );
        const idxChoice = Math.max(1, parseInt(choice || '0', 10) || 0) - 1;
        const cmd = commands[idxChoice];
        if (!cmd) return;

        const s = await loadWeeklySchedule();
        let updated = false;
        for (const slot of Object.values(s.slots)) {
            const idx = slot.items.findIndex(i => i.id === eventId);
            if (idx >= 0) {
                const item = slot.items[idx];
                const obsCommands = item.obsCommands || [];
                if (!obsCommands.includes(cmd.id)) obsCommands.push(cmd.id);
                slot.items[idx] = { ...item, obsCommands };
                updated = true;
                break;
            }
        }
        if (updated) {
            await saveWeeklySchedule(s);
            window.dispatchEvent(new CustomEvent('schedule-updated', { detail: { slotId: 'ticker' } }));
        }
    };

    const wrapWidth = useMemo(() => {
        // fallback width for SSR / early render
        return wrapRef.current?.clientWidth || 800;
    }, [wrapRef.current]);

    // Compute pixel offset so "now" is centered
    const msPerPx = (60 * 1000) / pxPerMin;
    const leftTimeAtX0 = now - (wrapWidth / 2) * msPerPx;

    const ticks = useMemo(() => {
        // Draw ticks for each minute in view, major every 5 minutes
        const count = Math.ceil(wrapWidth / pxPerMin) + 20;
        const startMin = Math.floor(leftTimeAtX0 / (60 * 1000));
        const list: Array<{ x: number; major: boolean }> = [];
        for (let i = 0; i < count; i++) {
            const min = startMin + i;
            const t = min * 60 * 1000;
            const x = ((t - leftTimeAtX0) / (60 * 1000)) * pxPerMin;
            list.push({ x, major: min % 5 === 0 });
        }
        return list;
    }, [wrapWidth, pxPerMin, leftTimeAtX0]);

    return (
        <>
        <div className={styles.tickerBar}>
            <div className={styles.leftControls}>
                <div className={styles.title}>Schedule Ticker</div>
                <div className={styles.zoomBtns}>
                    <button className={styles.btn} onClick={() => setZoom(z => (z === 'xs' ? 'xs' : z === 'sm' ? 'xs' : z === 'md' ? 'sm' : 'md'))} title="Zoom out">−</button>
                    <button className={styles.btn} onClick={() => setZoom(z => (z === 'lg' ? 'lg' : z === 'md' ? 'lg' : z === 'sm' ? 'md' : 'sm'))} title="Zoom in">+</button>
                </div>
            </div>

            <div className={styles.timelineWrap} ref={wrapRef}>
                <div className={styles.nowNeedle} />
                <div className={styles.nowLabel}>
                    {formatTime(now)}
                    {liveStatus.facebook && (
                        <span className={styles.liveStatusIcon} title="Facebook Live">
                            <Facebook size={12} />
                        </span>
                    )}
                    {liveStatus.youtube && (
                        <span className={styles.liveStatusIcon} title="YouTube Live">
                            <Youtube size={12} />
                        </span>
                    )}
                </div>

                <div className={styles.timeline}>
                    <div className={styles.ticks}>
                        {ticks.map((t, idx) => (
                            <div
                                key={idx}
                                className={`${styles.tick} ${t.major ? styles.tickMajor : ''}`}
                                style={{ left: `${t.x}px` }}
                            />
                        ))}
                    </div>

                    <div className={styles.itemsLayer}>
                        {events.map(ev => {
                            const start = ev.startTime;
                            const end = ev.endTime;
                            const x = ((start - leftTimeAtX0) / (60 * 1000)) * pxPerMin;
                            const w = Math.max(14, ((end - start) / (60 * 1000)) * pxPerMin);
                            const item = asPlaylistItem(ev);
                            const badges = getBadges(item);
                            return (
                                <div
                                    key={ev.id + '_' + ev.startTime}
                                    className={styles.item}
                                    style={{ left: `${x}px`, width: `${w}px`, ['--itemColor' as any]: typeColor(ev.media.type) }}
                                    title={`${new Date(ev.startTime).toLocaleTimeString()} • ${ev.media.title}`}
                                    onContextMenu={(e) => {
                                        e.preventDefault();
                                        const items: ContextMenuItem[] = [
                                            {
                                                id: 'attach_fb_post',
                                                label: 'Attach: Facebook Post',
                                                action: () => attachFacebookPost(ev.id),
                                            },
                                            {
                                                id: 'attach_obs_cmd',
                                                label: 'Attach: OBS Command',
                                                action: () => attachOBSCommand(ev.id),
                                            },
                                        ];
                                        showMenu(e, items);
                                    }}
                                >
                                    {w >= 90 && (ev.media.thumbnail ? (
                                        <img
                                            className={styles.itemThumb}
                                            src={ev.media.thumbnail}
                                            alt=""
                                            draggable={false}
                                        />
                                    ) : (
                                        <div className={styles.itemThumbPlaceholder}>▶</div>
                                    ))}
                                    <div className={styles.itemContent}>
                                        <div className={styles.itemTitle}>
                                            {ev.media.title}
                                            {hasOverlayAd(item) && (
                                                <span className={styles.overlayAdIcon} title="Overlay Ad">
                                                    <Layers size={12} />
                                                </span>
                                            )}
                                        </div>
                                        <div className={styles.itemMeta}>
                                            <span className={styles.badge}>{new Date(ev.startTime).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}</span>
                                            {badges.slice(0, 4).map(b => (
                                                <span key={b} className={styles.badge}>{b}</span>
                                            ))}
                                        </div>
                                    </div>
                                </div>
                            );
                        })}
                    </div>
                </div>
            </div>
        </div>
        {menuState.visible && (
            <ContextMenu
                x={menuState.x}
                y={menuState.y}
                items={menuState.items}
                onClose={hideMenu}
            />
        )}
        </>
    );
}


