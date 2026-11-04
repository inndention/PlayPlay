import React, { useState, useEffect } from 'react';
import { trackerApi } from '../lib/trackerApi';
import { AsPlayedEntry, AdClient } from '../lib/types';
import { loadClients } from '../lib/storage';
import { Search, Calendar, MapPin, Play, Clock, Download, BarChart2 } from 'lucide-react';
import styles from './SalesInterfacePage.module.css';

export function SalesInterfacePage() {
    const [clients, setClients] = useState<AdClient[]>([]);
    const [selectedClient, setSelectedClient] = useState<string>('all');
    const [reports, setReports] = useState<any[]>([]);
    const [loading, setLoading] = useState(false);
    const [dateRange, setDateRange] = useState({
        start: new Date(Date.now() - 30 * 24 * 60 * 60 * 1000).toISOString().split('T')[0],
        end: new Date().toISOString().split('T')[0]
    });

    useEffect(() => {
        setClients(loadClients());
    }, []);

    const fetchReports = async () => {
        setLoading(true);
        try {
            const response = await trackerApi.getAsPlayedReports(
                selectedClient === 'all' ? null : selectedClient,
                dateRange.start,
                dateRange.end
            );
            setReports(response.reports || []);
        } catch (error) {
            console.error('Failed to fetch reports:', error);
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        fetchReports();
    }, [selectedClient, dateRange]);

    const handleExport = () => {
        const headers = ['Date', 'Time', 'Asset Title', 'Location', 'Duration', 'Status'];
        const csv = [
            headers.join(','),
            ...reports.map(r => [
                new Date(r.timestamp).toLocaleDateString(),
                new Date(r.timestamp).toLocaleTimeString(),
                `"${r.mediaTitle}"`,
                `"${r.hostname}"`,
                r.duration,
                r.completed ? 'Full Play' : 'Partial'
            ].join(','))
        ].join('\n');

        const blob = new Blob([csv], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `StarNews-Report-${selectedClient}-${dateRange.start}.csv`;
        a.click();
    };

    return (
        <div className={styles.container}>
            <header className={styles.header}>
                <div className={styles.logo}>
                    <span className={styles.star}>★</span> Star News
                    <span className={styles.interface}>Sales Team Interface</span>
                </div>
                <div className={styles.stats}>
                    <div className={styles.statCard}>
                        <Play size={16} />
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>{reports.length}</span>
                            <span className={styles.statLabel}>Total Plays</span>
                        </div>
                    </div>
                    <div className={styles.statCard}>
                        <Clock size={16} />
                        <div className={styles.statInfo}>
                            <span className={styles.statValue}>
                                {Math.round(reports.reduce((acc, r) => acc + (r.duration || 0), 0) / 60)}m
                            </span>
                            <span className={styles.statLabel}>Total Airtime</span>
                        </div>
                    </div>
                </div>
            </header>

            <div className={styles.controls}>
                <div className={styles.filterGroup}>
                    <label><Search size={16} /> Client</label>
                    <select value={selectedClient} onChange={(e) => setSelectedClient(e.target.value)}>
                        <option value="all">All Clients</option>
                        {clients.map(c => (
                            <option key={c.id} value={c.id}>{c.name}</option>
                        ))}
                    </select>
                </div>

                <div className={styles.filterGroup}>
                    <label><Calendar size={16} /> Date Range</label>
                    <div className={styles.dateInputs}>
                        <input 
                            type="date" 
                            value={dateRange.start} 
                            onChange={(e) => setDateRange({...dateRange, start: e.target.value})}
                        />
                        <span>to</span>
                        <input 
                            type="date" 
                            value={dateRange.end} 
                            onChange={(e) => setDateRange({...dateRange, end: e.target.value})}
                        />
                    </div>
                </div>

                <button className={styles.exportBtn} onClick={handleExport}>
                    <Download size={16} /> Export CSV
                </button>
            </div>

            <div className={styles.content}>
                {loading ? (
                    <div className={styles.loading}>Loading Star News Reports...</div>
                ) : (
                    <table className={styles.reportTable}>
                        <thead>
                            <tr>
                                <th><Calendar size={14} /> Date & Time</th>
                                <th><BarChart2 size={14} /> Asset</th>
                                <th><MapPin size={14} /> Location</th>
                                <th><Clock size={14} /> Duration</th>
                                <th>Status</th>
                            </tr>
                        </thead>
                        <tbody>
                            {reports.map((report, idx) => (
                                <tr key={idx}>
                                    <td>
                                        <div className={styles.dateTime}>
                                            <span className={styles.date}>{new Date(report.timestamp).toLocaleDateString()}</span>
                                            <span className={styles.time}>{new Date(report.timestamp).toLocaleTimeString()}</span>
                                        </div>
                                    </td>
                                    <td>
                                        <div className={styles.assetInfo}>
                                            <span className={styles.assetTitle}>{report.mediaTitle}</span>
                                            <span className={styles.assetId}>{report.mediaId}</span>
                                        </div>
                                    </td>
                                    <td>
                                        <span className={styles.locationTag}>
                                            {report.hostname || 'Unknown Location'}
                                        </span>
                                    </td>
                                    <td>{report.duration}s</td>
                                    <td>
                                        <span className={report.completed ? styles.statusPlayed : styles.statusPartial}>
                                            {report.completed ? 'As Played' : 'Partial'}
                                        </span>
                                    </td>
                                </tr>
                            ))}
                        </tbody>
                    </table>
                )}
            </div>
        </div>
    );
}

