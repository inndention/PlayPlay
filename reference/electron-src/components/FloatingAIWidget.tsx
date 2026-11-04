import React, { useMemo, useState } from 'react';
import { Bot, ChevronUp, ChevronDown, Loader2, CalendarClock, FileSearch, ListChecks } from 'lucide-react';
import { runSchedulingAgent } from '../lib/aiSchedulingAgent';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { loadAsPlayedLog, loadLibraryMedia, loadLocalMedia, loadWeeklySchedule } from '../lib/storage';
import type { AsPlayedEntry, MediaItem } from '../lib/types';
import styles from './FloatingAIWidget.module.css';

interface FloatingAIWidgetProps {
    onOpenLibrary?: () => void;
    onOpenAnalytics?: () => void;
}

interface PlannedVsAsRunSummary {
    slotsWithContent: number;
    totalPlannedItems: number;
    asRunEntries24h: number;
    completed24h: number;
    avgStartDeltaSec: number;
}

export function FloatingAIWidget({ onOpenLibrary, onOpenAnalytics }: FloatingAIWidgetProps) {
    const [open, setOpen] = useState(true);
    const [busy, setBusy] = useState(false);
    const [prompt, setPrompt] = useState('Optimize current and next hour for ad separation and keep total at 59:30 max.');
    const [agentSummary, setAgentSummary] = useState<string>('');
    const [missingFiles, setMissingFiles] = useState<Array<{ title: string; filePath?: string }>>([]);
    const [report, setReport] = useState<PlannedVsAsRunSummary | null>(null);
    const [statusText, setStatusText] = useState('Ready');

    const missingPreview = useMemo(() => missingFiles.slice(0, 6), [missingFiles]);

    const runAgent = async (dryRun: boolean) => {
        if (!prompt.trim() || busy) return;
        setBusy(true);
        setStatusText(dryRun ? 'Running schedule dry-run...' : 'Applying schedule plan...');
        try {
            const result = await runSchedulingAgent(prompt.trim(), { dryRun });
            setAgentSummary(result.summary);
            setStatusText(result.success ? `AI complete (${result.appliedOperations.length} ops).` : `AI failed: ${result.error || 'unknown error'}`);
        } finally {
            setBusy(false);
        }
    };

    const scanMissingFiles = async () => {
        if (busy) return;
        setBusy(true);
        setStatusText('Scanning media library for missing files...');
        try {
            const [dbMedia, localMedia, libraryMedia] = await Promise.all([
                dbFetchMedia().catch(() => [] as MediaItem[]),
                Promise.resolve(loadLocalMedia()),
                Promise.resolve(loadLibraryMedia([])),
            ]);
            const byId = new Map<string, MediaItem>();
            [...libraryMedia, ...localMedia, ...dbMedia].forEach((m) => {
                if (m?.id && !byId.has(m.id)) byId.set(m.id, m);
            });
            const allMedia = Array.from(byId.values());
            const pathExists = (window as any).electronAPI?.pathExists;
            const missing: Array<{ title: string; filePath?: string }> = [];
            for (const media of allMedia) {
                if (!media.filePath || typeof pathExists !== 'function') continue;
                try {
                    const exists = await pathExists(media.filePath);
                    if (!exists) {
                        missing.push({ title: media.title, filePath: media.filePath });
                    }
                } catch {
                    // keep scan non-blocking
                }
            }
            setMissingFiles(missing);
            setStatusText(missing.length ? `Missing files found: ${missing.length}` : 'No missing files found.');
        } finally {
            setBusy(false);
        }
    };

    const buildReport = async () => {
        if (busy) return;
        setBusy(true);
        setStatusText('Building planned vs as-run report...');
        try {
            const schedule = await loadWeeklySchedule(true);
            const slots = Object.values(schedule.slots || {});
            const slotsWithContent = slots.filter((s) => (s.items || []).length > 0).length;
            const totalPlannedItems = slots.reduce((sum, slot) => sum + (slot.items?.length || 0), 0);

            const cutoff = Date.now() - (24 * 60 * 60 * 1000);
            const asPlayed = loadAsPlayedLog().filter((entry: AsPlayedEntry) => entry.timestamp >= cutoff);
            const completed24h = asPlayed.filter((e) => e.completed).length;
            const deltas = asPlayed
                .map((e) => {
                    const planned = e.scheduledStart;
                    const actual = e.actualStart || e.timestamp;
                    return typeof planned === 'number' ? Math.abs((actual - planned) / 1000) : null;
                })
                .filter((v): v is number => v !== null);
            const avgStartDeltaSec = deltas.length > 0 ? (deltas.reduce((a, b) => a + b, 0) / deltas.length) : 0;

            setReport({
                slotsWithContent,
                totalPlannedItems,
                asRunEntries24h: asPlayed.length,
                completed24h,
                avgStartDeltaSec: Number(avgStartDeltaSec.toFixed(2)),
            });
            setStatusText(`Report ready (${asPlayed.length} as-run entries in last 24h).`);
        } finally {
            setBusy(false);
        }
    };

    return (
        <div className={styles.widget}>
            <button className={styles.header} onClick={() => setOpen((v) => !v)} title="Toggle AI assistant widget">
                <span className={styles.headerLeft}><Bot size={15} /> AI Ops</span>
                {open ? <ChevronDown size={14} /> : <ChevronUp size={14} />}
            </button>
            {open && (
                <div className={styles.body}>
                    <div className={styles.section}>
                        <div className={styles.sectionTitle}><CalendarClock size={14} /> Scheduling</div>
                        <textarea
                            className={styles.prompt}
                            rows={3}
                            value={prompt}
                            onChange={(e) => setPrompt(e.target.value)}
                            placeholder="Describe scheduling request..."
                        />
                        <div className={styles.row}>
                            <button className={styles.btn} onClick={() => void runAgent(true)} disabled={busy}>Dry Run</button>
                            <button className={styles.btnPrimary} onClick={() => void runAgent(false)} disabled={busy}>Apply</button>
                        </div>
                        {agentSummary && <div className={styles.note}>{agentSummary}</div>}
                    </div>

                    <div className={styles.section}>
                        <div className={styles.sectionTitle}><FileSearch size={14} /> Media Imports / Missing</div>
                        <div className={styles.row}>
                            <button className={styles.btn} onClick={() => void scanMissingFiles()} disabled={busy}>Scan Missing</button>
                            <button className={styles.btn} onClick={onOpenLibrary} disabled={busy}>Open Library</button>
                        </div>
                        {missingFiles.length > 0 && (
                            <div className={styles.list}>
                                {missingPreview.map((item) => (
                                    <div key={`${item.title}-${item.filePath || ''}`} className={styles.listItem} title={item.filePath || ''}>
                                        {item.title}
                                    </div>
                                ))}
                            </div>
                        )}
                    </div>

                    <div className={styles.section}>
                        <div className={styles.sectionTitle}><ListChecks size={14} /> Reporting</div>
                        <div className={styles.row}>
                            <button className={styles.btn} onClick={() => void buildReport()} disabled={busy}>Build Report</button>
                            <button className={styles.btn} onClick={onOpenAnalytics} disabled={busy}>Open Analytics</button>
                        </div>
                        {report && (
                            <div className={styles.report}>
                                <div>Slots with content: {report.slotsWithContent}</div>
                                <div>Planned items: {report.totalPlannedItems}</div>
                                <div>As-run last 24h: {report.asRunEntries24h}</div>
                                <div>Completed last 24h: {report.completed24h}</div>
                                <div>Avg start delta: {report.avgStartDeltaSec}s</div>
                            </div>
                        )}
                    </div>

                    <div className={styles.footer}>
                        {busy ? <Loader2 size={12} className={styles.spin} /> : null}
                        <span>{statusText}</span>
                    </div>
                </div>
            )}
        </div>
    );
}

