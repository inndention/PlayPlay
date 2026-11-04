/**
 * Reports & Analytics – comprehensive yet simple.
 * As-run log, discrepancy report, summary stats, export.
 */

import React, { useState, useEffect, useCallback, useMemo } from 'react';
import { Link } from 'react-router-dom';
import {
    ArrowLeft, FileText, RefreshCw, Download, BarChart2, Calendar, Clock,
    CheckCircle, AlertTriangle, Filter, TrendingUp
} from 'lucide-react';
import styles from './AsPlayedReportPage.module.css';

const api = typeof window !== 'undefined' ? (window as any).electronAPI : null;

interface AsRunEntry {
    eventId: string;
    plannedStart: number;
    actualStart: number;
    engineId: string;
    assetId?: string;
    status: string;
    reason?: string;
    metadata?: Record<string, unknown>;
    timestamp: number;
}

const STATUS_OPTIONS = ['all', 'ON_TIME', 'LATE_START', 'OVERRUN', 'DESYNC', 'FAILED', 'CANCELLED'];

export function AsPlayedReportPage() {
    const [asRunLog, setAsRunLog] = useState<AsRunEntry[]>([]);
    const [discrepancy, setDiscrepancy] = useState<any[]>([]);
    const [loading, setLoading] = useState(true);
    const [tab, setTab] = useState<'asplayed' | 'discrepancy' | 'summary'>('summary');
    const [dateStart, setDateStart] = useState(() => {
        const d = new Date();
        d.setDate(d.getDate() - 7);
        return d.toISOString().split('T')[0];
    });
    const [dateEnd, setDateEnd] = useState(() => new Date().toISOString().split('T')[0]);
    const [statusFilter, setStatusFilter] = useState<string>('all');

    const load = useCallback(async () => {
        if (!api?.controlGetAsRunLog) {
            setAsRunLog([]);
            setDiscrepancy([]);
            setLoading(false);
            return;
        }
        setLoading(true);
        try {
            const res = await api.controlGetAsRunLog({ limit: 2000 });
            const log = res?.log ?? res;
            setAsRunLog(Array.isArray(log) ? log : []);

            if (api.controlGetDiscrepancyReport && dateStart && dateEnd) {
                const startMs = new Date(dateStart).getTime();
                const endMs = new Date(dateEnd).getTime() + 86400000;
                const repRes = await api.controlGetDiscrepancyReport(startMs, endMs);
                const report = repRes?.report ?? repRes;
                setDiscrepancy(Array.isArray(report) ? report : []);
            }
        } catch (e) {
            console.error('[AsPlayedReport]', e);
            setAsRunLog([]);
            setDiscrepancy([]);
        } finally {
            setLoading(false);
        }
    }, [dateStart, dateEnd]);

    useEffect(() => { void load(); }, [load]);

    const formatTime = (ms: number) => {
        if (ms == null) return '—';
        const d = new Date(ms);
        return d.toLocaleString();
    };

    const startMs = useMemo(() => new Date(dateStart).getTime(), [dateStart]);
    const endMs = useMemo(() => new Date(dateEnd).getTime() + 86400000, [dateEnd]);

    const filteredLog = useMemo(() => {
        let list = asRunLog.filter(e => e.actualStart >= startMs && e.actualStart < endMs);
        if (statusFilter !== 'all') list = list.filter(e => e.status === statusFilter);
        return list.reverse();
    }, [asRunLog, startMs, endMs, statusFilter]);

    const stats = useMemo(() => {
        const inRange = asRunLog.filter(e => e.actualStart >= startMs && e.actualStart < endMs);
        const total = inRange.length;
        const onTime = inRange.filter(e => e.status === 'ON_TIME').length;
        const late = inRange.filter(e => e.status === 'LATE_START').length;
        const failed = inRange.filter(e => e.status === 'FAILED' || e.status === 'CANCELLED').length;
        const overrun = inRange.filter(e => e.status === 'OVERRUN').length;
        return {
            total,
            onTime,
            onTimePct: total ? Math.round((onTime / total) * 100) : 0,
            late,
            failed,
            overrun,
        };
    }, [asRunLog, startMs, endMs]);

    const exportCsv = () => {
        const headers = ['Actual Time', 'Planned Start', 'Event', 'Asset', 'Status', 'Reason'];
        const rows = filteredLog.map(e => [
            formatTime(e.actualStart),
            formatTime(e.plannedStart),
            e.eventId,
            e.assetId ?? '',
            e.status,
            (e.reason ?? '').replace(/"/g, '""'),
        ]);
        const csv = [headers.join(','), ...rows.map(r => r.map(c => `"${c}"`).join(','))].join('\n');
        const blob = new Blob([csv], { type: 'text/csv;charset=utf-8' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `as-played-${dateStart}-to-${dateEnd}.csv`;
        a.click();
        URL.revokeObjectURL(url);
    };

    return (
        <div className={styles.page}>
            <header className={styles.header}>
                <Link to="/" className={styles.back}>
                    <ArrowLeft size={20} /> Back
                </Link>
                <h1 className={styles.title}>
                    <BarChart2 size={24} /> Reports & Analytics
                </h1>
                <div className={styles.headerActions}>
                    <button className={styles.refresh} onClick={() => void load()} disabled={loading}>
                        <RefreshCw size={18} className={loading ? styles.spin : ''} /> Refresh
                    </button>
                </div>
            </header>

            <div className={styles.filters}>
                <label className={styles.filterLabel}>
                    <Calendar size={14} /> From
                    <input type="date" value={dateStart} onChange={(e) => setDateStart(e.target.value)} />
                </label>
                <label className={styles.filterLabel}>
                    To
                    <input type="date" value={dateEnd} onChange={(e) => setDateEnd(e.target.value)} />
                </label>
                {tab === 'asplayed' && (
                    <label className={styles.filterLabel}>
                        <Filter size={14} /> Status
                        <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
                            {STATUS_OPTIONS.map(s => (
                                <option key={s} value={s}>{s === 'all' ? 'All' : s.replace('_', ' ')}</option>
                            ))}
                        </select>
                    </label>
                )}
            </div>

            <div className={styles.tabs}>
                <button
                    className={tab === 'summary' ? styles.tabActive : styles.tab}
                    onClick={() => setTab('summary')}
                >
                    <TrendingUp size={16} /> Summary
                </button>
                <button
                    className={tab === 'asplayed' ? styles.tabActive : styles.tab}
                    onClick={() => setTab('asplayed')}
                >
                    <FileText size={16} /> As-Played ({filteredLog.length})
                </button>
                <button
                    className={tab === 'discrepancy' ? styles.tabActive : styles.tab}
                    onClick={() => setTab('discrepancy')}
                >
                    <AlertTriangle size={16} /> Discrepancies
                </button>
            </div>

            {loading ? (
                <div className={styles.loading}>Loading…</div>
            ) : tab === 'summary' ? (
                <div className={styles.summarySection}>
                    <div className={styles.cards}>
                        <div className={styles.card}>
                            <Clock size={20} className={styles.cardIcon} />
                            <div className={styles.cardValue}>{stats.total}</div>
                            <div className={styles.cardLabel}>Total plays</div>
                        </div>
                        <div className={styles.card}>
                            <CheckCircle size={20} className={styles.cardIconSuccess} />
                            <div className={styles.cardValue}>{stats.onTimePct}%</div>
                            <div className={styles.cardLabel}>On time</div>
                        </div>
                        <div className={styles.card}>
                            <AlertTriangle size={20} className={styles.cardIconWarn} />
                            <div className={styles.cardValue}>{stats.late}</div>
                            <div className={styles.cardLabel}>Late starts</div>
                        </div>
                        <div className={styles.card}>
                            <AlertTriangle size={20} className={styles.cardIconDanger} />
                            <div className={styles.cardValue}>{stats.failed + stats.overrun}</div>
                            <div className={styles.cardLabel}>Failed / Overrun</div>
                        </div>
                    </div>
                    <div className={styles.summaryTableWrap}>
                        <h3 className={styles.sectionTitle}>By status (date range)</h3>
                        <table className={styles.table}>
                            <thead>
                                <tr>
                                    <th>Status</th>
                                    <th>Count</th>
                                    <th>%</th>
                                </tr>
                            </thead>
                            <tbody>
                                {[
                                    { status: 'ON_TIME', count: stats.onTime },
                                    { status: 'LATE_START', count: stats.late },
                                    { status: 'OVERRUN', count: stats.overrun },
                                    { status: 'FAILED / CANCELLED', count: stats.failed },
                                ].filter(r => r.count > 0).map(r => (
                                    <tr key={r.status}>
                                        <td>{r.status.replace('_', ' ')}</td>
                                        <td>{r.count}</td>
                                        <td>{stats.total ? Math.round((r.count / stats.total) * 100) : 0}%</td>
                                    </tr>
                                ))}
                                {stats.total === 0 && (
                                    <tr><td colSpan={3} className={styles.empty}>No data in range</td></tr>
                                )}
                            </tbody>
                        </table>
                    </div>
                </div>
            ) : tab === 'asplayed' ? (
                <div className={styles.tableSection}>
                    <div className={styles.exportRow}>
                        <button className={styles.exportBtn} onClick={exportCsv} disabled={filteredLog.length === 0}>
                            <Download size={16} /> Export CSV
                        </button>
                    </div>
                    <div className={styles.tableWrap}>
                        <table className={styles.table}>
                            <thead>
                                <tr>
                                    <th>Actual time</th>
                                    <th>Planned start</th>
                                    <th>Event</th>
                                    <th>Asset</th>
                                    <th>Status</th>
                                    <th>Reason</th>
                                </tr>
                            </thead>
                            <tbody>
                                {filteredLog.length === 0 ? (
                                    <tr><td colSpan={6} className={styles.empty}>No as-run entries in range</td></tr>
                                ) : (
                                    filteredLog.map((e) => (
                                        <tr key={e.eventId + String(e.timestamp)}>
                                            <td>{formatTime(e.actualStart)}</td>
                                            <td>{formatTime(e.plannedStart)}</td>
                                            <td>{e.eventId}</td>
                                            <td>{e.assetId ?? '—'}</td>
                                            <td><span className={styles.status}>{e.status}</span></td>
                                            <td>{e.reason ?? '—'}</td>
                                        </tr>
                                    ))
                                )}
                            </tbody>
                        </table>
                    </div>
                </div>
            ) : (
                <div className={styles.tableWrap}>
                    {discrepancy.length === 0 ? (
                        <p className={styles.empty}>No discrepancies in range</p>
                    ) : (
                        <table className={styles.table}>
                            <thead>
                                <tr>
                                    <th>Time</th>
                                    <th>Details</th>
                                </tr>
                            </thead>
                            <tbody>
                                {discrepancy.map((row: any, i: number) => (
                                    <tr key={i}>
                                        <td>{row.time != null ? formatTime(row.time) : '—'}</td>
                                        <td>{typeof row === 'object' ? JSON.stringify(row) : String(row)}</td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                    )}
                </div>
            )}
        </div>
    );
}
