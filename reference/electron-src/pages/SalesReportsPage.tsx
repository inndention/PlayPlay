/**
 * Sales Reports - detailed as-played and scheduled reporting.
 */

import React, { useState, useEffect } from 'react';
import {
    BarChart2, Calendar, Clock, Download, FileText, Activity, Layers,
    Search, RefreshCw, Sparkles,
} from 'lucide-react';
import { loadClients } from '../lib/storage';
import {
    buildDetailedReportBundle,
    DetailedReportBundle,
    downloadDetailedReport,
    summarizeDetailedReportWithAI,
} from '../lib/detailedReportBuilder';
import styles from './SalesReportsPage.module.css';

type ReportTab = 'asplayed' | 'scheduled' | 'summary' | 'generate';

export function SalesReportsPage() {
    const [tab, setTab] = useState<ReportTab>('summary');
    const [bundle, setBundle] = useState<DetailedReportBundle | null>(null);
    const [clients, setClients] = useState<{ id: string; name: string }[]>([]);
    const [loading, setLoading] = useState(true);
    const [aiLoading, setAiLoading] = useState(false);
    const [aiSummary, setAiSummary] = useState('');
    const [dateStart, setDateStart] = useState(() => new Date(Date.now() - 30 * 86400000).toISOString().split('T')[0]);
    const [dateEnd, setDateEnd] = useState(() => new Date().toISOString().split('T')[0]);
    const [clientFilter, setClientFilter] = useState<string>('all');

    const refreshReport = async () => {
        setLoading(true);
        try {
            const nextBundle = await buildDetailedReportBundle({
                startDate: dateStart,
                endDate: dateEnd,
                clientId: clientFilter === 'all' ? undefined : clientFilter,
            });
            setBundle(nextBundle);
            setClients(loadClients().map(client => ({ id: client.id, name: client.name })));
            setAiSummary('');
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        refreshReport();
    }, [dateStart, dateEnd, clientFilter]);

    const handleAiSummary = async () => {
        if (!bundle) return;
        setAiLoading(true);
        try {
            setAiSummary(await summarizeDetailedReportWithAI(bundle));
            setTab('summary');
        } finally {
            setAiLoading(false);
        }
    };

    const totals = bundle?.totals;

    return (
        <div className={styles.container}>
            <header className={styles.header}>
                <div className={styles.logo}>
                    <BarChart2 size={24} />
                    <span>Detailed Reports</span>
                    <span className={styles.badge}>In-app + AI-ready</span>
                </div>
                <div className={styles.stats}>
                    <div className={styles.statCard}>
                        <Activity size={16} />
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>{totals?.asPlayedCount || 0}</span>
                            <span className={styles.statLabel}>As-Played</span>
                        </div>
                    </div>
                    <div className={styles.statCard}>
                        <Layers size={16} />
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>{totals?.scheduledCount || 0}</span>
                            <span className={styles.statLabel}>Scheduled</span>
                        </div>
                    </div>
                    <div className={styles.statCard}>
                        <Clock size={16} />
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>{Math.round((totals?.asPlayedSeconds || 0) / 60)}m</span>
                            <span className={styles.statLabel}>Airtime</span>
                        </div>
                    </div>
                </div>
            </header>

            <div className={styles.tabs}>
                <button type="button" className={tab === 'summary' ? styles.tabActive : ''} onClick={() => setTab('summary')}>
                    <BarChart2 size={16} /> Summary
                </button>
                <button type="button" className={tab === 'asplayed' ? styles.tabActive : ''} onClick={() => setTab('asplayed')}>
                    <Activity size={16} /> As-Played
                </button>
                <button type="button" className={tab === 'scheduled' ? styles.tabActive : ''} onClick={() => setTab('scheduled')}>
                    <Calendar size={16} /> Scheduled
                </button>
                <button type="button" className={tab === 'generate' ? styles.tabActive : ''} onClick={() => setTab('generate')}>
                    <FileText size={16} /> Export
                </button>
            </div>

            <div className={styles.controls}>
                <div className={styles.filterGroup}>
                    <label><Calendar size={14} /> Date range</label>
                    <input type="date" value={dateStart} onChange={(e) => setDateStart(e.target.value)} />
                    <span>to</span>
                    <input type="date" value={dateEnd} onChange={(e) => setDateEnd(e.target.value)} />
                </div>
                <div className={styles.filterGroup}>
                    <label><Search size={14} /> Client</label>
                    <select value={clientFilter} onChange={(e) => setClientFilter(e.target.value)}>
                        <option value="all">All</option>
                        {clients.map(client => (
                            <option key={client.id} value={client.id}>{client.name}</option>
                        ))}
                    </select>
                </div>
                <button type="button" className={styles.refreshBtn} onClick={refreshReport} disabled={loading}>
                    <RefreshCw size={14} /> Refresh
                </button>
                <button type="button" className={styles.refreshBtn} onClick={handleAiSummary} disabled={loading || aiLoading || !bundle}>
                    <Sparkles size={14} /> {aiLoading ? 'Analyzing...' : 'AI Summary'}
                </button>
            </div>

            <div className={styles.content}>
                {loading || !bundle ? (
                    <div className={styles.loading}>Loading report data...</div>
                ) : (
                    <>
                        {tab === 'summary' && (
                            <div className={styles.summaryGrid}>
                                <section className={styles.summaryPanel}>
                                    <h3>Report Totals</h3>
                                    <dl>
                                        <dt>Generated</dt><dd>{new Date(bundle.generatedAt).toLocaleString()}</dd>
                                        <dt>Completed Plays</dt><dd>{bundle.totals.completedCount}</dd>
                                        <dt>Partial Plays</dt><dd>{bundle.totals.partialCount}</dd>
                                        <dt>Scheduled Airtime</dt><dd>{Math.round(bundle.totals.scheduledSeconds / 60)}m</dd>
                                        <dt>Unique Clients</dt><dd>{bundle.totals.uniqueClients}</dd>
                                    </dl>
                                </section>
                                <section className={styles.summaryPanel}>
                                    <h3>Client Delivery</h3>
                                    <div className={styles.tableWrapCompact}>
                                        <table className={styles.table}>
                                            <thead>
                                                <tr><th>Client</th><th>Played</th><th>Scheduled</th><th>Partial</th><th>Airtime</th></tr>
                                            </thead>
                                            <tbody>
                                                {bundle.byClient.slice(0, 25).map(row => (
                                                    <tr key={row.clientId}>
                                                        <td>{row.clientName}</td>
                                                        <td>{row.asPlayedCount}</td>
                                                        <td>{row.scheduledCount}</td>
                                                        <td>{row.partialCount}</td>
                                                        <td>{Math.round(row.asPlayedSeconds / 60)}m</td>
                                                    </tr>
                                                ))}
                                            </tbody>
                                        </table>
                                    </div>
                                </section>
                                <section className={styles.aiPanel}>
                                    <h3>AI Report Notes</h3>
                                    <pre>{aiSummary || 'Use AI Summary to generate delivery notes, risks, and follow-up recommendations from this report.'}</pre>
                                </section>
                            </div>
                        )}

                        {tab === 'asplayed' && (
                            <>
                                <div className={styles.actions}>
                                    <button type="button" className={styles.exportBtn} onClick={() => downloadDetailedReport(bundle, 'asplayed-csv')}>
                                        <Download size={14} /> Export Detailed As-Played CSV
                                    </button>
                                </div>
                                <div className={styles.tableWrap}>
                                    <table className={styles.table}>
                                        <thead>
                                            <tr>
                                                <th>Date & Time</th><th>Asset</th><th>Type</th><th>Client</th><th>Duration</th><th>Status</th><th>Variance</th><th>Slot</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {bundle.asPlayed.slice(0, 800).map((row, idx) => (
                                                <tr key={`${row.mediaId}-${row.isoTime}-${idx}`}>
                                                    <td>{row.isoTime}</td>
                                                    <td>{row.title}<br /><code>{row.mediaId}</code></td>
                                                    <td>{row.type}</td>
                                                    <td>{row.clientName || '-'}</td>
                                                    <td>{row.durationSeconds}s</td>
                                                    <td>{row.status}</td>
                                                    <td>{row.varianceSeconds ?? '-'}</td>
                                                    <td>{row.hourSlot || '-'}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                                {bundle.asPlayed.length > 800 && <p className={styles.note}>Showing first 800 of {bundle.asPlayed.length}.</p>}
                            </>
                        )}

                        {tab === 'scheduled' && (
                            <>
                                <div className={styles.actions}>
                                    <button type="button" className={styles.exportBtn} onClick={() => downloadDetailedReport(bundle, 'scheduled-csv')}>
                                        <Download size={14} /> Export Detailed Scheduled CSV
                                    </button>
                                </div>
                                <div className={styles.tableWrap}>
                                    <table className={styles.table}>
                                        <thead>
                                            <tr>
                                                <th>Slot</th><th>Start</th><th>Show/Block</th><th>Asset</th><th>Type</th><th>Client</th><th>Duration</th><th>Flags</th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {bundle.scheduled.slice(0, 1000).map((row, idx) => (
                                                <tr key={`${row.slotId}-${row.mediaId}-${idx}`}>
                                                    <td><code>{row.slotId}</code></td>
                                                    <td>{row.startLabel} +{row.startOffsetSeconds}s</td>
                                                    <td>{row.showTitle || row.blockType || '-'}</td>
                                                    <td>{row.title}<br /><code>{row.mediaId}</code></td>
                                                    <td>{row.type}</td>
                                                    <td>{row.clientName || '-'}</td>
                                                    <td>{row.durationSeconds}s</td>
                                                    <td>{row.paused ? 'Paused' : '-'}</td>
                                                </tr>
                                            ))}
                                        </tbody>
                                    </table>
                                </div>
                                {bundle.scheduled.length > 1000 && <p className={styles.note}>Showing first 1000 of {bundle.scheduled.length}.</p>}
                            </>
                        )}

                        {tab === 'generate' && (
                            <div className={styles.generateSection}>
                                <p>Export the same detailed report data used by the interface and AI helper.</p>
                                <div className={styles.exportStack}>
                                    <button type="button" className={styles.primaryBtn} onClick={() => downloadDetailedReport(bundle, 'html')}>
                                        <Download size={16} /> Download Detailed HTML
                                    </button>
                                    <button type="button" className={styles.primaryBtn} onClick={() => downloadDetailedReport(bundle, 'json')}>
                                        <Download size={16} /> Download Report JSON
                                    </button>
                                    <button type="button" className={styles.primaryBtn} onClick={() => downloadDetailedReport(bundle, 'asplayed-csv')}>
                                        <Download size={16} /> Download As-Played CSV
                                    </button>
                                    <button type="button" className={styles.primaryBtn} onClick={() => downloadDetailedReport(bundle, 'scheduled-csv')}>
                                        <Download size={16} /> Download Scheduled CSV
                                    </button>
                                </div>
                                <p className={styles.help}>
                                    JSON is the handoff format for AI review. CSV is for traffic, billing, and client delivery records.
                                </p>
                            </div>
                        )}
                    </>
                )}
            </div>
        </div>
    );
}
