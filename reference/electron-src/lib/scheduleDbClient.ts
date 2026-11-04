import type { WeeklySchedule, HourSlot, FreePlaylist } from './types';

const isElectron = typeof window !== 'undefined' && !!(window as any).electronAPI;

export async function dbGetScheduleMeta(): Promise<Record<string, string>> {
    if (!isElectron) return {};
    try {
        return await (window as any).electronAPI!.scheduleGetMeta();
    } catch (e) {
        console.error('[ScheduleDB] getMeta failed:', e);
        return {};
    }
}

export async function dbSetScheduleMeta(key: string, value: string): Promise<void> {
    if (!isElectron) return;
    try {
        await (window as any).electronAPI!.scheduleSetMeta(key, value);
    } catch (e) {
        console.error('[ScheduleDB] setMeta failed:', e);
    }
}

export async function dbGetAllScheduleSlots(): Promise<HourSlot[]> {
    if (!isElectron) return [];
    try {
        const raw = await (window as any).electronAPI!.scheduleGetAllSlots();
        return raw.map((row: any) => ({
            id: row.id,
            dayOfWeek: row.dayOfWeek,
            hour: row.hour,
            items: JSON.parse(row.items || '[]'),
            isLooping: row.isLooping === 1,
            randomizeAds: row.randomizeAds === 1,
            keepProgramOrder: row.keepProgramOrder === 1,
        }));
    } catch (e) {
        console.error('[ScheduleDB] getAllSlots failed:', e);
        return [];
    }
}

export async function dbSaveScheduleSlot(slot: HourSlot): Promise<void> {
    if (!isElectron) return;
    try {
        await (window as any).electronAPI!.scheduleSaveSlot(slot);
    } catch (e) {
        console.error('[ScheduleDB] saveSlot failed:', e);
    }
}

export async function dbSaveWeeklySchedule(schedule: WeeklySchedule): Promise<void> {
    if (!isElectron) return;
    try {
        const api = (window as any).electronAPI!;
        if (api.scheduleSaveWeekly) {
            const res = await api.scheduleSaveWeekly({
                schedule,
                meta: {
                    version: schedule.version || 1,
                    lastModified: schedule.lastModified || Date.now(),
                },
            });
            if (!res?.ok) {
                throw new Error(res?.error || 'scheduleSaveWeekly failed');
            }
            return;
        }

        // Fallback for older runtimes: save meta + all slots one by one.
        await dbSetScheduleMeta('version', String(schedule.version || 1));
        await dbSetScheduleMeta('lastModified', String(schedule.lastModified || Date.now()));
        for (const slot of Object.values(schedule.slots || {})) {
            await dbSaveScheduleSlot(slot);
        }
    } catch (e) {
        console.error('[ScheduleDB] saveWeeklySchedule failed:', e);
        throw e;
    }
}

export async function dbGetAllFreePlaylists(): Promise<FreePlaylist[]> {
    if (!isElectron) return [];
    try {
        const raw = await (window as any).electronAPI!.scheduleGetAllPlaylists();
        return raw.map((row: any) => ({
            id: row.id,
            name: row.name,
            items: JSON.parse(row.items || '[]'),
            isLocked: row.isLocked === 1,
            createdAt: row.createdAt,
            createdFrom: row.createdFrom || undefined,
        }));
    } catch (e) {
        console.error('[ScheduleDB] getAllPlaylists failed:', e);
        return [];
    }
}

export async function dbSaveFreePlaylist(playlist: FreePlaylist): Promise<void> {
    if (!isElectron) return;
    try {
        await (window as any).electronAPI!.scheduleSavePlaylist(playlist);
    } catch (e) {
        console.error('[ScheduleDB] savePlaylist failed:', e);
    }
}

export async function dbDeleteFreePlaylist(id: string): Promise<void> {
    if (!isElectron) return;
    try {
        await (window as any).electronAPI!.scheduleDeletePlaylist(id);
    } catch (e) {
        console.error('[ScheduleDB] deletePlaylist failed:', e);
    }
}



