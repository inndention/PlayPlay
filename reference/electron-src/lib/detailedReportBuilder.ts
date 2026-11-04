import { AsPlayedEntry, HourSlot, MediaItem, WeeklySchedule } from './types';
import { loadAsPlayedLog, loadClients, loadLibraryMedia, loadLocalMedia, loadWeeklySchedule } from './storage';
import { dbFetchMedia } from './mediaDbClient';
import { requestOpenRouterJson } from './aiHelper';

export interface ReportRange {
    startDate: string;
    endDate: string;
    clientId?: string;
}

export interface DetailedAsPlayedRow {
    date: string;
    time: string;
    isoTime: string;
    mediaId: string;
    title: string;
    type: string;
    clientId?: string;
    clientName?: string;
    durationSeconds: number;
    scheduledStart?: string;
    actualStart?: string;
    actualEnd?: string;
    varianceSeconds?: number;
    completed: boolean;
    status: 'full' | 'partial';
    hourSlot?: string;
    itemId?: string;
    source?: string;
}

export interface DetailedScheduledRow {
    slotId: string;
    dayOfWeek?: number;
    date?: string;
    hour: number;
    startLabel: string;
    blockType?: string;
    showTitle?: string;
    itemIndex: number;
    mediaId: string;
    title: string;
    type: string;
    clientId?: string;
    clientName?: string;
    durationSeconds: number;
    startOffsetSeconds: number;
    scheduledStart?: string;
    fixedTime?: string;
    paused?: boolean;
    availableStart?: string;
    availableEnd?: string;
}

export interface ClientReportSummary {
    clientId: string;
    clientName: string;
    scheduledCount: number;
    asPlayedCount: number;
    completedCount: number;
    partialCount: number;
    scheduledSeconds: number;
    asPlayedSeconds: number;
}

export interface DetailedReportBundle {
    generatedAt: string;
    range: ReportRange;
    source: 'local';
    totals: {
        asPlayedCount: number;
        scheduledCount: number;
        completedCount: number;
        partialCount: number;
        asPlayedSeconds: number;
        scheduledSeconds: number;
        uniqueClients: number;
    };
    byClient: ClientReportSummary[];
    asPlayed: DetailedAsPlayedRow[];
    scheduled: DetailedScheduledRow[];
}

function csvEscape(value: unknown): string {
    const text = value === undefined || value === null ? '' : String(value);
    return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

function downloadText(filename: string, content: string, type: string): void {
    const blob = new Blob([content], { type });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
}

function inRange(timestamp: number, range: ReportRange): boolean {
    const start = new Date(range.startDate).getTime();
    const end = new Date(range.endDate).getTime() + 86400000;
    return timestamp >= start && timestamp < end;
}

function dateLabel(timestamp: number): { date: string; time: string; isoTime: string } {
    const date = new Date(timestamp);
    return {
        date: date.toLocaleDateString(),
        time: date.toLocaleTimeString(),
        isoTime: date.toISOString(),
    };
}

function buildMediaIndex(media: MediaItem[]): Map<string, MediaItem> {
    const index = new Map<string, MediaItem>();
    media.forEach(item => index.set(item.id, item));
    return index;
}

function inferClientId(media?: MediaItem): string | undefined {
    return media?.client || media?.adMetadata?.client || media?.clientGroupId;
}

function clientName(clientId: string | undefined, clients: Map<string, string>): string | undefined {
    if (!clientId) return undefined;
    return clients.get(clientId) || clientId;
}

async function loadAllMedia(): Promise<MediaItem[]> {
    const [dbMedia, localMedia, libraryMedia] = await Promise.all([
        dbFetchMedia().catch(() => []),
        Promise.resolve(loadLocalMedia()),
        Promise.resolve(loadLibraryMedia([])),
    ]);

    return Array.from(new Map([...libraryMedia, ...localMedia, ...dbMedia].map(item => [item.id, item])).values());
}

function slotStartLabel(slot: HourSlot): string {
    if (slot.date) return `${slot.date} ${String(slot.hour).padStart(2, '0')}:00`;
    const days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    return `${days[slot.dayOfWeek ?? 0] || 'Day'} ${String(slot.hour).padStart(2, '0')}:00`;
}

function buildScheduledRows(schedule: WeeklySchedule, mediaIndex: Map<string, MediaItem>, clients: Map<string, string>, range: ReportRange): DetailedScheduledRow[] {
    const rows: DetailedScheduledRow[] = [];

    Object.values(schedule.slots || {}).forEach(slot => {
        let offset = 0;
        (slot.items || []).forEach((item, itemIndex) => {
            const media = mediaIndex.get(item.mediaId) || item.media;
            const clientId = inferClientId(media);
            if (range.clientId && clientId !== range.clientId) {
                offset += media?.duration || 0;
                return;
            }

            const fixedTime = item.fixedTime ? new Date(item.fixedTime).toISOString() : undefined;
            rows.push({
                slotId: slot.id,
                dayOfWeek: slot.dayOfWeek,
                date: slot.date,
                hour: slot.hour,
                startLabel: slotStartLabel(slot),
                blockType: slot.blockType,
                showTitle: slot.showBlock?.title,
                itemIndex,
                mediaId: item.mediaId,
                title: media?.title || item.media?.title || 'Untitled',
                type: media?.type || item.media?.type || 'video',
                clientId,
                clientName: clientName(clientId, clients),
                durationSeconds: media?.duration || item.media?.duration || 0,
                startOffsetSeconds: offset,
                scheduledStart: item.startTime ? new Date(item.startTime).toISOString() : undefined,
                fixedTime,
                paused: media?.paused,
                availableStart: media?.availableStart,
                availableEnd: media?.availableEnd,
            });
            offset += media?.duration || item.media?.duration || 0;
        });
    });

    return rows;
}

function buildAsPlayedRows(log: AsPlayedEntry[], mediaIndex: Map<string, MediaItem>, clients: Map<string, string>, range: ReportRange): DetailedAsPlayedRow[] {
    return log
        .filter(entry => inRange(entry.timestamp, range))
        .map(entry => {
            const media = mediaIndex.get(entry.mediaId);
            const clientId = inferClientId(media);
            const labels = dateLabel(entry.timestamp);
            const scheduled = entry.scheduledStart;
            const actual = entry.actualStart || entry.timestamp;
            return {
                ...labels,
                mediaId: entry.mediaId,
                title: entry.mediaTitle || media?.title || 'Untitled',
                type: media?.type || 'unknown',
                clientId,
                clientName: clientName(clientId, clients),
                durationSeconds: entry.duration,
                scheduledStart: scheduled ? new Date(scheduled).toISOString() : undefined,
                actualStart: actual ? new Date(actual).toISOString() : undefined,
                actualEnd: entry.actualEnd ? new Date(entry.actualEnd).toISOString() : undefined,
                varianceSeconds: scheduled ? Math.round((actual - scheduled) / 1000) : undefined,
                completed: entry.completed,
                status: entry.completed ? 'full' as const : 'partial' as const,
                hourSlot: entry.hourSlot,
                itemId: entry.itemId,
                source: entry.source,
            };
        })
        .filter(row => !range.clientId || row.clientId === range.clientId);
}

function summarizeByClient(asPlayed: DetailedAsPlayedRow[], scheduled: DetailedScheduledRow[]): ClientReportSummary[] {
    const map = new Map<string, ClientReportSummary>();
    const ensure = (clientId?: string, clientName?: string) => {
        const id = clientId || 'unassigned';
        const name = clientName || 'Unassigned';
        if (!map.has(id)) {
            map.set(id, {
                clientId: id,
                clientName: name,
                scheduledCount: 0,
                asPlayedCount: 0,
                completedCount: 0,
                partialCount: 0,
                scheduledSeconds: 0,
                asPlayedSeconds: 0,
            });
        }
        return map.get(id)!;
    };

    scheduled.forEach(row => {
        const summary = ensure(row.clientId, row.clientName);
        summary.scheduledCount++;
        summary.scheduledSeconds += row.durationSeconds;
    });

    asPlayed.forEach(row => {
        const summary = ensure(row.clientId, row.clientName);
        summary.asPlayedCount++;
        summary.asPlayedSeconds += row.durationSeconds;
        if (row.completed) summary.completedCount++;
        else summary.partialCount++;
    });

    return Array.from(map.values()).sort((a, b) => b.asPlayedCount + b.scheduledCount - (a.asPlayedCount + a.scheduledCount));
}

export async function buildDetailedReportBundle(range: ReportRange): Promise<DetailedReportBundle> {
    const [media, schedule] = await Promise.all([
        loadAllMedia(),
        loadWeeklySchedule(),
    ]);
    const mediaIndex = buildMediaIndex(media);
    const clients = new Map(loadClients().map(client => [client.id, client.name]));
    const asPlayed = buildAsPlayedRows(loadAsPlayedLog(), mediaIndex, clients, range);
    const scheduled = buildScheduledRows(schedule, mediaIndex, clients, range);
    const completedCount = asPlayed.filter(row => row.completed).length;

    return {
        generatedAt: new Date().toISOString(),
        range,
        source: 'local',
        totals: {
            asPlayedCount: asPlayed.length,
            scheduledCount: scheduled.length,
            completedCount,
            partialCount: asPlayed.length - completedCount,
            asPlayedSeconds: asPlayed.reduce((sum, row) => sum + row.durationSeconds, 0),
            scheduledSeconds: scheduled.reduce((sum, row) => sum + row.durationSeconds, 0),
            uniqueClients: new Set([...asPlayed.map(row => row.clientId), ...scheduled.map(row => row.clientId)].filter(Boolean)).size,
        },
        byClient: summarizeByClient(asPlayed, scheduled),
        asPlayed,
        scheduled,
    };
}

export function asPlayedToCsv(rows: DetailedAsPlayedRow[]): string {
    const headers = ['Date', 'Time', 'ISO Time', 'Asset', 'Media ID', 'Type', 'Client', 'Duration Seconds', 'Scheduled Start', 'Actual Start', 'Actual End', 'Variance Seconds', 'Status', 'Slot', 'Source'];
    const body = rows.map(row => [
        row.date,
        row.time,
        row.isoTime,
        row.title,
        row.mediaId,
        row.type,
        row.clientName,
        row.durationSeconds,
        row.scheduledStart,
        row.actualStart,
        row.actualEnd,
        row.varianceSeconds,
        row.status,
        row.hourSlot,
        row.source,
    ].map(csvEscape).join(','));
    return [headers.join(','), ...body].join('\n');
}

export function scheduledToCsv(rows: DetailedScheduledRow[]): string {
    const headers = ['Slot ID', 'Start', 'Hour', 'Block Type', 'Show', 'Index', 'Asset', 'Media ID', 'Type', 'Client', 'Duration Seconds', 'Offset Seconds', 'Fixed Time', 'Paused', 'Available Start', 'Available End'];
    const body = rows.map(row => [
        row.slotId,
        row.startLabel,
        row.hour,
        row.blockType,
        row.showTitle,
        row.itemIndex,
        row.title,
        row.mediaId,
        row.type,
        row.clientName,
        row.durationSeconds,
        row.startOffsetSeconds,
        row.fixedTime,
        row.paused ? 'Yes' : '',
        row.availableStart,
        row.availableEnd,
    ].map(csvEscape).join(','));
    return [headers.join(','), ...body].join('\n');
}

export function reportBundleToHtml(bundle: DetailedReportBundle): string {
    const esc = (value: unknown) => String(value ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    const minutes = (seconds: number) => `${Math.round(seconds / 60)}m`;
    return `<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"/><title>PLAY PLAY Report ${esc(bundle.range.startDate)} to ${esc(bundle.range.endDate)}</title>
<style>
body{font-family:system-ui,sans-serif;margin:1rem;background:#f8fafc;color:#0f172a}h1,h2{margin:.5rem 0}.meta{color:#64748b;margin-bottom:1rem}.cards{display:flex;gap:.75rem;flex-wrap:wrap;margin:1rem 0}.card{background:white;border:1px solid #e2e8f0;border-radius:8px;padding:.75rem 1rem}.card b{display:block;font-size:1.35rem}table{border-collapse:collapse;width:100%;margin:1rem 0;background:white}th,td{border:1px solid #e2e8f0;padding:.45rem .65rem;text-align:left;font-size:.85rem}th{background:#1e40af;color:white}code{font-size:.8rem}
</style></head>
<body>
<h1>Detailed As-Played &amp; Scheduled Report</h1>
<p class="meta">Generated ${esc(bundle.generatedAt)} | Range ${esc(bundle.range.startDate)} to ${esc(bundle.range.endDate)} | Source ${esc(bundle.source)}</p>
<div class="cards">
<div class="card"><b>${bundle.totals.asPlayedCount}</b>As-played rows</div>
<div class="card"><b>${bundle.totals.scheduledCount}</b>Scheduled items</div>
<div class="card"><b>${minutes(bundle.totals.asPlayedSeconds)}</b>As-played airtime</div>
<div class="card"><b>${bundle.totals.partialCount}</b>Partial plays</div>
<div class="card"><b>${bundle.totals.uniqueClients}</b>Clients</div>
</div>
<h2>Client Summary</h2>
<table><thead><tr><th>Client</th><th>As-Played</th><th>Scheduled</th><th>Completed</th><th>Partial</th><th>As-Played Time</th><th>Scheduled Time</th></tr></thead><tbody>
${bundle.byClient.map(row => `<tr><td>${esc(row.clientName)}</td><td>${row.asPlayedCount}</td><td>${row.scheduledCount}</td><td>${row.completedCount}</td><td>${row.partialCount}</td><td>${minutes(row.asPlayedSeconds)}</td><td>${minutes(row.scheduledSeconds)}</td></tr>`).join('\n')}
</tbody></table>
<h2>As-Played Log</h2>
<table><thead><tr><th>Date &amp; Time</th><th>Asset</th><th>Type</th><th>Client</th><th>Duration</th><th>Status</th><th>Variance</th><th>Slot</th></tr></thead><tbody>
${bundle.asPlayed.slice(0, 1500).map(row => `<tr><td>${esc(row.isoTime)}</td><td>${esc(row.title)}<br/><code>${esc(row.mediaId)}</code></td><td>${esc(row.type)}</td><td>${esc(row.clientName || '')}</td><td>${row.durationSeconds}s</td><td>${esc(row.status)}</td><td>${row.varianceSeconds ?? ''}</td><td>${esc(row.hourSlot || '')}</td></tr>`).join('\n')}
</tbody></table>
<h2>Scheduled Items</h2>
<table><thead><tr><th>Slot</th><th>Start</th><th>Show</th><th>Asset</th><th>Type</th><th>Client</th><th>Duration</th><th>Flags</th></tr></thead><tbody>
${bundle.scheduled.slice(0, 2500).map(row => `<tr><td><code>${esc(row.slotId)}</code></td><td>${esc(row.startLabel)} +${row.startOffsetSeconds}s</td><td>${esc(row.showTitle || row.blockType || '')}</td><td>${esc(row.title)}<br/><code>${esc(row.mediaId)}</code></td><td>${esc(row.type)}</td><td>${esc(row.clientName || '')}</td><td>${row.durationSeconds}s</td><td>${row.paused ? 'Paused' : ''}</td></tr>`).join('\n')}
</tbody></table>
</body></html>`;
}

export function downloadDetailedReport(bundle: DetailedReportBundle, format: 'html' | 'json' | 'asplayed-csv' | 'scheduled-csv'): void {
    const suffix = `${bundle.range.startDate}-to-${bundle.range.endDate}`;
    if (format === 'html') downloadText(`detailed-report-${suffix}.html`, reportBundleToHtml(bundle), 'text/html');
    if (format === 'json') downloadText(`detailed-report-${suffix}.json`, JSON.stringify(bundle, null, 2), 'application/json');
    if (format === 'asplayed-csv') downloadText(`detailed-as-played-${suffix}.csv`, asPlayedToCsv(bundle.asPlayed), 'text/csv');
    if (format === 'scheduled-csv') downloadText(`detailed-scheduled-${suffix}.csv`, scheduledToCsv(bundle.scheduled), 'text/csv');
}

export async function summarizeDetailedReportWithAI(bundle: DetailedReportBundle): Promise<string> {
    const compact = {
        range: bundle.range,
        totals: bundle.totals,
        byClient: bundle.byClient.slice(0, 25),
        partialExamples: bundle.asPlayed.filter(row => !row.completed).slice(0, 20),
        topScheduled: bundle.scheduled.slice(0, 40),
    };

    const result = await requestOpenRouterJson<{ summary?: string; risks?: string[]; recommendations?: string[] }>({
        system: 'You are a broadcast traffic reporting assistant. Return JSON with summary, risks, and recommendations.',
        prompt: `Create a detailed but concise as-played and scheduled report analysis from this PLAY PLAY report JSON:\n${JSON.stringify(compact)}`,
        temperature: 0.2,
        maxTokens: 1200,
    });

    if (!result) {
        return 'AI summary unavailable. Set OPENROUTER_API_KEY to enable report analysis.';
    }

    if (typeof result === 'string') return result;
    const lines = [
        result.summary || '',
        ...(result.risks?.length ? ['Risks:', ...result.risks.map(item => `- ${item}`)] : []),
        ...(result.recommendations?.length ? ['Recommendations:', ...result.recommendations.map(item => `- ${item}`)] : []),
    ].filter(Boolean);

    return lines.join('\n');
}
