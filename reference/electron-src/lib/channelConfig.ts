/**
 * Per-channel output configuration — 8 slots, first 4 enabled by default.
 * Persisted to localStorage independently from PlayoutChannel / multiChannelState.
 */

export interface ChannelOutputConfig {
    id: string;          // 'ch-1' … 'ch-8'
    number: number;      // 1-8
    name: string;
    enabled: boolean;    // active output; disabled channels show in UI but don't push
    casparChannel: number; // 1-based CasparCG channel index
    ndi: { enabled: boolean; name: string };
    srt: { enabled: boolean; url: string; bitrate: number; codec: string };
    rtmp: { enabled: boolean; url: string; key: string };
    hls: { enabled: boolean; url: string };
}

export const STORAGE_KEY = 'playplay_channel_configs';

export function defaultChannelConfig(n: number): ChannelOutputConfig {
    const i = n - 1;
    return {
        id: `ch-${n}`,
        number: n,
        name: `Channel ${n}`,
        enabled: n <= 4,
        casparChannel: n,
        ndi: { enabled: n <= 4, name: `PlayPlay CH${n}` },
        srt: { enabled: false, url: `srt://0.0.0.0:${9000 + n}`, bitrate: 4000, codec: 'libx264' },
        rtmp: { enabled: false, url: n <= 4 ? 'rtmp://a.rtmp.youtube.com/live2/' : '', key: '' },
        hls: { enabled: false, url: '' },
    };
    void i;
}

export const DEFAULT_CHANNEL_CONFIGS: ChannelOutputConfig[] =
    Array.from({ length: 8 }, (_, i) => defaultChannelConfig(i + 1));

export function loadChannelConfigs(): ChannelOutputConfig[] {
    try {
        const stored = localStorage.getItem(STORAGE_KEY);
        if (stored) {
            const parsed: ChannelOutputConfig[] = JSON.parse(stored);
            if (Array.isArray(parsed) && parsed.length > 0) {
                // Back-fill any new fields added since last save
                return Array.from({ length: 8 }, (_, i) => ({
                    ...defaultChannelConfig(i + 1),
                    ...(parsed[i] || {}),
                }));
            }
        }
    } catch { /* */ }
    return DEFAULT_CHANNEL_CONFIGS.map(c => ({ ...c }));
}

export function saveChannelConfigs(configs: ChannelOutputConfig[]): void {
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(configs)); } catch { /* */ }
}

/** localStorage key for a channel's weekly schedule (mirrors storage.ts logic). */
export function channelScheduleKey(channelId: string): string {
    if (!channelId || channelId === 'ch-1' || channelId === 'default') return 'playplay_weekly';
    return 'playplay_weekly__' + channelId.replace(/[^A-Za-z0-9_-]/g, '_');
}
