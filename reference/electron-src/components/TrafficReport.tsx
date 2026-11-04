import React, { useState, useEffect } from 'react';
import { MediaStats, AdClient, PlayEvent, MediaItem } from '../lib/types';
import { loadMediaStats, loadClients, exportToJson, loadLibraryMedia } from '../lib/storage';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { FileText, Download, Printer, Filter, Calendar } from 'lucide-react';
import { formatDuration } from '../lib/utils';
import styles from './TrafficReport.module.css';

interface ReportRow {
    id: string;
    mediaItem: MediaItem;
    client?: AdClient;
    timestamp: number;
    duration: number;
    completed: boolean;
    expectedDuration: number;
}

export function TrafficReport() {
    const [stats, setStats] = useState<Record<string, MediaStats>>({});
    const [clients, setClients] = useState<AdClient[]>([]);
    const [reportData, setReportData] = useState<ReportRow[]>([]);
    const [mediaIndex, setMediaIndex] = useState<MediaItem[]>([]);

    // Filters
    const [dateRange, setDateRange] = useState({ start: '', end: '' });
    const [selectedClient, setSelectedClient] = useState<string>('all');
    const [selectedType, setSelectedType] = useState<string>('ad');

    useEffect(() => {
        setStats(loadMediaStats());
        setClients(loadClients());
        (async () => {
            const dbMedia = await dbFetchMedia().catch(() => [] as MediaItem[]);
            const library = loadLibraryMedia([]);
            const combined = [...dbMedia, ...library];
            const seen = new Set<string>();
            const out: MediaItem[] = [];
            for (const m of combined) {
                if (!m?.id) continue;
                if (seen.has(m.id)) continue;
                seen.add(m.id);
                out.push(m);
            }
            setMediaIndex(out);
        })();

        // Default date range: Last 7 days to Today
        const end = new Date();
        const start = new Date();
        start.setDate(start.getDate() - 7);
        setDateRange({
            start: start.toISOString().split('T')[0],
            end: end.toISOString().split('T')[0]
        });
    }, []);

    useEffect(() => {
        generateReport();
    }, [stats, dateRange, selectedClient, selectedType, mediaIndex]);

    const generateReport = () => {
        const rows: ReportRow[] = [];
        const combinedMedia = mediaIndex.length > 0 ? mediaIndex : loadLibraryMedia([]);

        const startDate = new Date(dateRange.start).getTime();
        const endDate = new Date(dateRange.end).getTime() + 86400000; // End of day

        Object.entries(stats).forEach(([mediaId, mediaStats]) => {
            const mediaItem = combinedMedia.find(m => m.id === mediaId);
            if (!mediaItem) return;

            // Type Filter
            if (selectedType !== 'all' && mediaItem.type !== selectedType) return;

            // Client Filter
            let client: AdClient | undefined;
            if (mediaItem.client) {
                client = clients.find(c => c.name === mediaItem.client || c.id === mediaItem.client);
            }

            if (selectedClient !== 'all') {
                if (!client) return; // If filtered by client but item has no client
                if (client.id !== selectedClient) return;
            }

            // Process History
            mediaStats.playHistory.forEach(event => {
                if (event.timestamp >= startDate && event.timestamp < endDate) {
                    rows.push({
                        id: `${mediaId}_${event.timestamp}`,
                        mediaItem,
                        client,
                        timestamp: event.timestamp,
                        duration: event.duration,
                        completed: event.completed,
                        expectedDuration: mediaItem.duration
                    });
                }
            });
        });

        // Sort by time
        rows.sort((a, b) => b.timestamp - a.timestamp);
        setReportData(rows);
    };

    const handleExport = () => {
        const csvContent = [
            "Date,Time,Title,Type,Client,Duration,Status",
            ...reportData.map(row => {
                const date = new Date(row.timestamp);
                return [
                    date.toLocaleDateString(),
                    date.toLocaleTimeString(),
                    `"${row.mediaItem.title}"`,
                    row.mediaItem.type,
                    `"${row.client ? row.client.name : (row.mediaItem.client || '')}"`,
                    row.duration,
                    row.completed ? 'Complete' : 'Partial'
                ].join(',');
            })
        ].join('\n');

        const blob = new Blob([csvContent], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `traffic_report_${dateRange.start}_${dateRange.end}.csv`;
        a.click();
    };

    const totalDuration = reportData.reduce((acc, row) => acc + row.duration, 0);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>
                    <FileText size={20} />
                    <span>Traffic & Affidavit Report</span>
                </div>
            </div>

            <div className={styles.controls}>
                <div className={styles.controlGroup}>
                    <span className={styles.label}>Date Range</span>
                    <div style={{ display: 'flex', gap: '0.5rem' }}>
                        <input
                            type="date"
                            className={styles.input}
                            value={dateRange.start}
                            onChange={e => setDateRange({ ...dateRange, start: e.target.value })}
                        />
                        <input
                            type="date"
                            className={styles.input}
                            value={dateRange.end}
                            onChange={e => setDateRange({ ...dateRange, end: e.target.value })}
                        />
                    </div>
                </div>

                <div className={styles.controlGroup}>
                    <span className={styles.label}>Client</span>
                    <select
                        className={styles.select}
                        value={selectedClient}
                        onChange={e => setSelectedClient(e.target.value)}
                    >
                        <option value="all">All Clients</option>
                        {clients.map(c => (
                            <option key={c.id} value={c.id}>{c.name}</option>
                        ))}
                    </select>
                </div>

                <div className={styles.controlGroup}>
                    <span className={styles.label}>Media Type</span>
                    <select
                        className={styles.select}
                        value={selectedType}
                        onChange={e => setSelectedType(e.target.value)}
                    >
                        <option value="all">All Types</option>
                        <option value="ad">Ads Only</option>
                        <option value="program">Programs</option>
                        <option value="promo">Promos</option>
                    </select>
                </div>

                <div className={styles.actions}>
                    <button className={styles.btn} onClick={() => window.print()}>
                        <Printer size={16} /> Print
                    </button>
                    <button className={`${styles.btn} ${styles.btnPrimary}`} onClick={handleExport}>
                        <Download size={16} /> Export CSV
                    </button>
                </div>
            </div>

            <div className={styles.reportContent}>
                <table className={styles.table}>
                    <thead>
                        <tr>
                            <th>Date / Time</th>
                            <th>Title</th>
                            <th>Type</th>
                            <th>Client</th>
                            <th>Duration</th>
                            <th>Status</th>
                        </tr>
                    </thead>
                    <tbody>
                        {reportData.map(row => (
                            <tr key={row.id}>
                                <td>
                                    {new Date(row.timestamp).toLocaleDateString()}
                                    <br />
                                    <span style={{ color: 'var(--text-muted)', fontSize: '0.8em' }}>
                                        {new Date(row.timestamp).toLocaleTimeString()}
                                    </span>
                                </td>
                                <td>{row.mediaItem.title}</td>
                                <td style={{ textTransform: 'capitalize' }}>{row.mediaItem.type.replace('_', ' ')}</td>
                                <td>
                                    {row.client ? (
                                        <div className={styles.clientBadge} style={{ border: `1px solid ${row.client.color}` }}>
                                            <div style={{ width: 8, height: 8, borderRadius: '50%', background: row.client.color }} />
                                            {row.client.name}
                                        </div>
                                    ) : (
                                        row.mediaItem.client || '-'
                                    )}
                                </td>
                                <td>{formatDuration(row.duration)} / {formatDuration(row.expectedDuration)}</td>
                                <td className={row.completed ? styles.statusComplete : styles.statusPartial}>
                                    {row.completed ? 'Full Play' : 'Partial'}
                                </td>
                            </tr>
                        ))}
                        {reportData.length === 0 && (
                            <tr>
                                <td colSpan={6}>
                                    <div className={styles.emptyState}>No records found matching filters</div>
                                </td>
                            </tr>
                        )}
                    </tbody>
                </table>
            </div>

            <div className={styles.summary}>
                <span>Total Plays: {reportData.length}</span>
                <span>Total Duration: {formatDuration(totalDuration)}</span>
            </div>
        </div>
    );
}
