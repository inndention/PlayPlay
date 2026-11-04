import { useEffect, useRef } from 'react';
import { PlayoutChannel, PlaylistItem } from '../lib/types';
import { channelScheduleKey } from '../lib/channelConfig';
import { localCalendarDateKey } from '../lib/utils';
import { buildCasparRunlistForBossClock } from '../lib/casparBossClock';

interface Options {
    /** Master on/off for auto-push */
    enabled: boolean;
    /** All sidebar channels (`casparChannel` per row else row index + 1) */
    channels: PlayoutChannel[];
    /** How many seconds past the hour boundary to allow a late push (default 30) */
    graceSeconds?: number;
}

/**
 * At the top of each hour (within graceSeconds), loads each channel's scheduled
 * HourSlot from localStorage and pushes it to CasparCG.
 *
 * Uses template slots ("dayOfWeek-hour") with date-specific overrides ("YYYY-MM-DD-hour").
 * Runlist is **wall-clock joined** (same boss-clock math as snap / PUSH) so a late Caspar
 * start within the grace window still seeks into the correct spot in the hour.
 */
export function useHourAutoPush({ enabled, channels, graceSeconds = 30 }: Options) {
    const lastPushedHourRef = useRef<number>(-1);
    const lastPushedDateRef = useRef<string>('');

    useEffect(() => {
        if (!enabled) return;

        const tick = async () => {
            const now = new Date();
            const hour = now.getHours();
            const minutes = now.getMinutes();
            const seconds = now.getSeconds();
            const dateStr = localCalendarDateKey(now);

            // Only fire within graceSeconds after the hour boundary
            if (minutes !== 0 || seconds > graceSeconds) return;
            // Don't fire twice for the same hour on the same day
            if (lastPushedHourRef.current === hour && lastPushedDateRef.current === dateStr) return;

            lastPushedHourRef.current = hour;
            lastPushedDateRef.current = dateStr;

            const dayOfWeek = now.getDay();
            const api = (window as any).electronAPI;

            const hourStart = new Date(now);
            hourStart.setMinutes(0, 0, 0);
            const hourStartMs = hourStart.getTime();

            // Collect all channels that have content this hour
            const channelItems: Array<{ channel: number; items: PlaylistItem[] }> = [];
            for (let i = 0; i < channels.length; i++) {
                const channel = channels[i];
                const rawCas = (channel as any).casparChannel;
                const casparCh =
                    typeof rawCas === 'number' && Number.isFinite(rawCas) && rawCas >= 1
                        ? Math.floor(rawCas)
                        : i + 1;

                const schedKey = channelScheduleKey(channel.id);
                try {
                    const stored = localStorage.getItem(schedKey);
                    if (!stored) continue;
                    const schedule = JSON.parse(stored) as { slots?: Record<string, { items?: PlaylistItem[] }> };
                    if (!schedule?.slots) continue;

                    const dateSlotId     = `${dateStr}-${hour}`;
                    const templateSlotId = `${dayOfWeek}-${hour}`;
                    const slot = schedule.slots[dateSlotId] ?? schedule.slots[templateSlotId];
                    if (!slot?.items?.length) continue;

                    const synced = buildCasparRunlistForBossClock(slot.items, hourStartMs);
                    if (!synced.length) continue;

                    channelItems.push({ channel: casparCh, items: synced });
                } catch (e) {
                    console.error(`[AutoPush] channel ${channel.id} collect failed:`, e);
                }
            }

            if (!channelItems.length) return;

            try {
                if (channelItems.length > 1 && api?.casparPlayMultiSync) {
                    // Push all channels in one coordinated block — minimises sync offset.
                    await api.casparPlayMultiSync(channelItems);
                    console.log(`[AutoPush] ${dateStr} ${hour}:00 → ${channelItems.length} channels synced (boss clock)`);
                } else if (api?.casparPlayPlaylist) {
                    // Single channel or fallback
                    for (const { channel, items } of channelItems) {
                        await api.casparPlayPlaylist(channel, items);
                        console.log(`[AutoPush] ${dateStr} ${hour}:00 → CasparCG CH${channel} — ${items.length} items (boss clock)`);
                    }
                }
            } catch (e) {
                console.error('[AutoPush] push failed:', e);
            }
        };

        // Poll every 5 seconds; lightweight since it only acts within the first graceSeconds of each hour
        const id = setInterval(tick, 5000);
        return () => clearInterval(id);
    }, [enabled, channels, graceSeconds]);
}
