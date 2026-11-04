/**
 * Report API client for external access: fetch as-played and scheduled from API,
 * or publish snapshot from playout app.
 */

const DEFAULT_API_BASE = typeof window !== 'undefined' ? '' : 'http://localhost:3000';

export interface AsPlayedReportRow {
    mediaId: string;
    timestamp: number;
    duration: number;
    completed: boolean;
    mediaTitle?: string;
    hourSlot?: string;
    itemId?: string;
    scheduledStart?: number;
    actualStart?: number;
    actualEnd?: number;
    source?: string;
}

export interface ScheduleSlotRow {
    id: string;
    dayOfWeek?: number;
    date?: string;
    hour: number;
    itemCount: number;
    itemTitles: string[];
}

export interface ReportsSnapshotPayload {
    asPlayed: AsPlayedReportRow[];
    scheduleSlots: ScheduleSlotRow[];
    clients?: { id: string; name: string }[];
    exportedAt?: number;
}

function getBase(): string {
    if (typeof window === 'undefined') return process.env.REPORT_API_BASE || DEFAULT_API_BASE;
    return (window as any).__REPORT_API_BASE__ || '';
}

export async function fetchAsPlayed(start?: string, end?: string, client?: string): Promise<AsPlayedReportRow[]> {
    const base = getBase();
    const params = new URLSearchParams();
    if (start) params.set('start', start);
    if (end) params.set('end', end);
    if (client) params.set('client', client);
    const url = `${base}/api/v1/reports/as-played${params.toString() ? '?' + params.toString() : ''}`;
    const res = await fetch(url);
    if (!res.ok) return [];
    const data = await res.json();
    return data.asPlayed || [];
}

export async function fetchScheduled(): Promise<ScheduleSlotRow[]> {
    const base = getBase();
    const res = await fetch(`${base}/api/v1/reports/scheduled`);
    if (!res.ok) return [];
    const data = await res.json();
    return data.scheduleSlots || [];
}

export async function fetchSnapshot(): Promise<ReportsSnapshotPayload | null> {
    const base = getBase();
    const res = await fetch(`${base}/api/v1/reports/snapshot`);
    if (res.status === 404 || !res.ok) return null;
    return res.json();
}

export async function publishSnapshot(payload: ReportsSnapshotPayload): Promise<boolean> {
    const base = getBase();
    const res = await fetch(`${base}/api/v1/reports/snapshot`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ...payload, exportedAt: Date.now() }),
    });
    return res.ok;
}

export async function reportApiAvailable(): Promise<boolean> {
    const base = getBase();
    if (!base) return false;
    try {
        const res = await fetch(`${base}/api/v1/status`, { method: 'GET' });
        return res.ok;
    } catch {
        return false;
    }
}
