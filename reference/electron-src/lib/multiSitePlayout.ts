import { HourSlot, MediaItem, PlaylistItem, PlayoutSiteProfile, WeeklySchedule } from './types';

export const PLAYOUT_SITE_PROFILES: PlayoutSiteProfile[] = [
    {
        id: 'martinsville',
        name: 'Martinsville / Henry',
        type: 'playout',
        schedulePrefix: 'site:martinsville',
        callSign: 'Star News Martinsville',
        city: 'Martinsville',
        state: 'VA',
        timezone: 'America/New_York',
        regionTags: ['martinsville', 'henry', 'patrick', 'va', 'southside'],
        defaultShiftHours: 0,
        adRegionWeight: 1.2,
        enabled: true,
    },
    {
        id: 'danville',
        name: 'Danville / Pittsylvania',
        type: 'playout',
        schedulePrefix: 'site:danville',
        callSign: 'Star News Danville',
        city: 'Danville',
        state: 'VA',
        timezone: 'America/New_York',
        regionTags: ['danville', 'pittsylvania', 'southside', 'va'],
        defaultShiftHours: 0,
        adRegionWeight: 1.15,
        enabled: true,
    },
    {
        id: 'rockingham',
        name: 'Rockingham / Eden / Reidsville',
        type: 'playout',
        schedulePrefix: 'site:rockingham',
        callSign: 'Star News Rockingham',
        city: 'Eden',
        state: 'NC',
        timezone: 'America/New_York',
        regionTags: ['rockingham', 'eden', 'reidsville', 'ncdot', 'nc'],
        defaultShiftHours: 0,
        adRegionWeight: 1.1,
        enabled: true,
    },
];

export interface ScheduleShiftOptions {
    sourceSiteId?: string;
    targetSiteId: string;
    sourceDay: number;
    sourceHour: number;
    targetDay?: number;
    targetHour?: number;
    shiftHours?: number;
    mode: 'copy' | 'move';
    itemTypes?: Array<MediaItem['type'] | 'all'>;
    preservePrograms?: boolean;
    regionalizeAds?: boolean;
    mediaLibrary?: MediaItem[];
}

export function getPlayoutSite(siteId: string): PlayoutSiteProfile | undefined {
    return PLAYOUT_SITE_PROFILES.find(site => site.id === siteId);
}

export function makeSiteSlotId(siteId: string, day: number, hour: number): string {
    return `site:${siteId}:${day}-${hour}`;
}

export function parseSiteSlotId(slotId: string): { siteId?: string; day?: number; hour?: number; baseSlotId: string } {
    const match = slotId.match(/^site:([^:]+):(\d+)-(\d+)$/);
    if (!match) return { baseSlotId: slotId };
    return {
        siteId: match[1],
        day: Number(match[2]),
        hour: Number(match[3]),
        baseSlotId: `${match[2]}-${match[3]}`,
    };
}

export function getSlotForSite(schedule: WeeklySchedule, siteId: string, day: number, hour: number): HourSlot | null {
    const siteSlotId = makeSiteSlotId(siteId, day, hour);
    return schedule.slots[siteSlotId] || schedule.slots[`${day}-${hour}`] || null;
}

function clonePlaylistItem(item: PlaylistItem): PlaylistItem {
    return {
        ...item,
        id: `ms_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`,
    };
}

function mediaMatchesRegion(media: MediaItem, site: PlayoutSiteProfile): boolean {
    if (media.applicableSiteIds?.length) return media.applicableSiteIds.includes(site.id);
    if (!media.regionTags?.length) return true;
    const siteTags = new Set((site.regionTags || []).map(tag => tag.toLowerCase()));
    return media.regionTags.some(tag => siteTags.has(tag.toLowerCase()));
}

export function rankRegionalMedia(candidates: MediaItem[], siteId: string): MediaItem[] {
    const site = getPlayoutSite(siteId);
    if (!site) return candidates;
    const siteTags = new Set((site.regionTags || []).map(tag => tag.toLowerCase()));

    return [...candidates].sort((a, b) => {
        const score = (media: MediaItem) => {
            let value = 0;
            if (media.applicableSiteIds?.includes(site.id)) value += 100;
            (media.regionTags || []).forEach(tag => {
                if (siteTags.has(tag.toLowerCase())) value += 12;
            });
            if (media.type === 'ad') value += site.adRegionWeight || 1;
            return value;
        };
        return score(b) - score(a);
    });
}

function regionalizeAdItem(item: PlaylistItem, siteId: string, mediaLibrary: MediaItem[]): PlaylistItem {
    if (item.media?.type !== 'ad') return item;
    if (mediaMatchesRegion(item.media, getPlayoutSite(siteId)!)) return item;

    const replacement = rankRegionalMedia(
        mediaLibrary.filter(media => media.type === 'ad' && mediaMatchesRegion(media, getPlayoutSite(siteId)!)),
        siteId,
    )[0];

    if (!replacement) return item;
    return {
        ...item,
        mediaId: replacement.id,
        media: replacement,
        isResolved: true,
    };
}

function filterItems(items: PlaylistItem[], itemTypes?: Array<MediaItem['type'] | 'all'>): PlaylistItem[] {
    if (!itemTypes?.length || itemTypes.includes('all')) return items;
    return items.filter(item => itemTypes.includes(item.media?.type));
}

export function shiftScheduleBlock(schedule: WeeklySchedule, options: ScheduleShiftOptions): WeeklySchedule {
    const sourceSiteId = options.sourceSiteId || options.targetSiteId;
    const sourceSlot = getSlotForSite(schedule, sourceSiteId, options.sourceDay, options.sourceHour);
    if (!sourceSlot) return schedule;

    const shiftHours = options.shiftHours || 0;
    const rawTargetHour = options.targetHour ?? (options.sourceHour + shiftHours);
    const targetDay = options.targetDay ?? ((options.sourceDay + Math.floor(rawTargetHour / 24) + 7) % 7);
    const targetHour = ((rawTargetHour % 24) + 24) % 24;
    const targetSlotId = makeSiteSlotId(options.targetSiteId, targetDay, targetHour);
    const site = getPlayoutSite(options.targetSiteId);
    const sourceItems = filterItems(sourceSlot.items || [], options.itemTypes);
    const nextItems = sourceItems.map(clonePlaylistItem).map(item => {
        if (options.regionalizeAds && site && options.mediaLibrary) {
            return regionalizeAdItem(item, options.targetSiteId, options.mediaLibrary);
        }
        return item;
    });

    const nextSlots = { ...schedule.slots };
    nextSlots[targetSlotId] = {
        ...sourceSlot,
        id: targetSlotId,
        dayOfWeek: targetDay,
        date: undefined,
        hour: targetHour,
        items: nextItems,
    };

    if (options.mode === 'move') {
        const sourceSlotId = makeSiteSlotId(sourceSiteId, options.sourceDay, options.sourceHour);
        if (nextSlots[sourceSlotId]) {
            nextSlots[sourceSlotId] = {
                ...nextSlots[sourceSlotId],
                items: (nextSlots[sourceSlotId].items || []).filter(item => !sourceItems.some(source => source.id === item.id)),
            };
        }
    }

    return {
        ...schedule,
        slots: nextSlots,
        lastModified: Date.now(),
        version: (schedule.version || 0) + 1,
    };
}

export function describeScheduleShift(options: ScheduleShiftOptions): string {
    const source = options.sourceSiteId || options.targetSiteId;
    const targetHour = options.targetHour ?? (options.sourceHour + (options.shiftHours || 0));
    return `${options.mode} ${options.itemTypes?.join('/') || 'all'} from ${source} day ${options.sourceDay} ${options.sourceHour}:00 to ${options.targetSiteId} ${targetHour}:00`;
}
