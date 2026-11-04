/**
 * Server-side report data: read/write JSON file for external API access.
 * Electron or a sync job can POST snapshot; sales team GET from API.
 */

import path from 'path';
import fs from 'fs-extra';

const DEFAULT_PATH = path.join(process.cwd(), 'data', 'reports.json');

export interface AsPlayedReportEntry {
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

export interface ScheduleSlotSummary {
    id: string;
    dayOfWeek?: number;
    date?: string;
    hour: number;
    itemCount: number;
    itemTitles: string[];
}

export interface ReportsSnapshot {
    asPlayed: AsPlayedReportEntry[];
    scheduleSlots: ScheduleSlotSummary[];
    clients?: { id: string; name: string }[];
    exportedAt: number;
}

function getDataPath(): string {
    return process.env.REPORTS_DATA_PATH || DEFAULT_PATH;
}

export async function readReportsSnapshot(): Promise<ReportsSnapshot | null> {
    const filePath = getDataPath();
    try {
        if (!(await fs.pathExists(filePath))) return null;
        const raw = await fs.readFile(filePath, 'utf-8');
        const data = JSON.parse(raw) as ReportsSnapshot;
        return data && typeof data.exportedAt === 'number' ? data : null;
    } catch {
        return null;
    }
}

export async function writeReportsSnapshot(snapshot: ReportsSnapshot): Promise<void> {
    const filePath = getDataPath();
    await fs.ensureDir(path.dirname(filePath));
    await fs.writeFile(filePath, JSON.stringify(snapshot, null, 2), 'utf-8');
}
