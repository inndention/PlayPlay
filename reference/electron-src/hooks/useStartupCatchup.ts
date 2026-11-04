import { useEffect, useRef } from 'react';
import { PlayoutChannel, PlaylistItem } from '../lib/types';
import { channelScheduleKey } from '../lib/channelConfig';
import { localCalendarDateKey } from '../lib/utils';
import { buildCasparRunlistForBossClock } from '../lib/casparBossClock';

interface Options {
    /** When false, skip boss-clock catch-up pushes (Caspar transport may still be used manually). */
    enabled?: boolean;
    channels: PlayoutChannel[];
    casparRunning: boolean;
}

/**
 * When Caspar AMCP comes up, push the current hour from each channel's saved schedule
 * with **wall-clock join-in-progress** (boss clock). Runs again after each Caspar stop→start
 * so a late or restarted engine re-syncs to the hour.
 */
export function useStartupCatchup({ enabled = true, channels, casparRunning }: Options) {
    const offSessionRef = useRef(0);
    const hasPushedForSessionRef = useRef(false);

    useEffect(() => {
        if (!enabled) return;
        if (!casparRunning) {
            offSessionRef.current += 1;
            hasPushedForSessionRef.current = false;
            return;
        }

        // Only push once per Caspar "up" session. Re-pushing on every
        // channels-array mutation would clear the currently playing output.
        if (hasPushedForSessionRef.current) return;

        const sessionAtStart = offSessionRef.current;

        const catchup = async () => {
            const now = new Date();
            const hour = now.getHours();
            const dateStr = localCalendarDateKey(now);
            const dayOfWeek = now.getDay();

            const hourStart = new Date(now);
            hourStart.setMinutes(0, 0, 0);
            const hourStartMs = hourStart.getTime();

            const api = (window as any).electronAPI;
            if (!api?.casparPlayMultiSync && !api?.casparPlayPlaylist) return;

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
                    if (synced.length > 0) {
                        channelItems.push({ channel: casparCh, items: synced });
                    }
                } catch (e) {
                    console.error(`[StartupCatchup] channel ${channel.id} failed:`, e);
                }
            }

            if (!channelItems.length) return;
            if (sessionAtStart !== offSessionRef.current) return;

            try {
                if (channelItems.length > 1 && api?.casparPlayMultiSync) {
                    await api.casparPlayMultiSync(channelItems);
                    console.log(`[StartupCatchup] Synced ${channelItems.length} channels to boss clock (hour anchor)`);
                } else if (api?.casparPlayPlaylist) {
                    for (const { channel, items } of channelItems) {
                        await api.casparPlayPlaylist(channel, items);
                        console.log(`[StartupCatchup] Synced CH${channel} to boss clock`);
                    }
                }
            } catch (e) {
                console.error('[StartupCatchup] push failed:', e);
            }
        };

        void (async () => {
            await catchup();
            hasPushedForSessionRef.current = true;
        })();
    }, [enabled, casparRunning, channels]);
}
