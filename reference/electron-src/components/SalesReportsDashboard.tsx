// ============================================
// SALES REPORTS DASHBOARD
// Easy-to-use interface for sales team to generate and view reports
// ============================================

import React, { useState, useEffect, useCallback } from 'react';
import {
    FileText, Download, Calendar, Users, TrendingUp,
    DollarSign, Clock, BarChart2, CheckCircle, AlertCircle,
    RefreshCw, Filter, ChevronDown, Copy, ExternalLink,
    FileSpreadsheet, FileCode, Printer
} from 'lucide-react';
import styles from './SalesReportsDashboard.module.css';

interface ReportType {
    id: string;
    name: string;
    description: string;
    icon: React.ReactNode;
}

interface ReportParams {
    startDate?: string;
    endDate?: string;
    clientId?: string;
    campaignId?: string;
    dayPart?: string;
    format?: 'json' | 'csv' | 'html';
}

interface GeneratedReport {
    type: string;
    generatedAt: string;
    data: any[];
    summary?: Record<string, any>;
    [key: string]: any;
}

const REPORT_TYPES: ReportType[] = [
    { id: 'ad_performance', name: 'Ad Performance', description: 'Detailed ad play breakdown', icon: <TrendingUp size={18} /> },
    { id: 'client_summary', name: 'Client Summary', description: 'Client contracts overview', icon: <Users size={18} /> },
    { id: 'campaign_tracking', name: 'Campaign Tracking', description: 'Campaign progress & pacing', icon: <BarChart2 size={18} /> },
    { id: 'billing_summary', name: 'Billing Summary', description: 'Monthly billing report', icon: <DollarSign size={18} /> },
    { id: 'affidavit', name: 'Affidavit', description: 'Legal play verification', icon: <CheckCircle size={18} /> },
    { id: 'time_slot_analysis', name: 'Time Slot Analysis', description: 'Daypart performance', icon: <Clock size={18} /> },
];

const DAY_PARTS = [
    'Early Morning',
    'Morning',
    'Afternoon',
    'Early Fringe',
    'Prime Time',
    'Late Night',
];

export function SalesReportsDashboard() {
    const [selectedReport, setSelectedReport] = useState<string>('ad_performance');
    const [params, setParams] = useState<ReportParams>({
        startDate: new Date(Date.now() - 7 * 24 * 60 * 60 * 1000).toISOString().split('T')[0],
        endDate: new Date().toISOString().split('T')[0],
        format: 'json',
    });
    const [clients, setClients] = useState<Array<{ id: string; name: string }>>([]);
    const [campaigns, setCampaigns] = useState<Array<{ id: string; name: string }>>([]);
    const [report, setReport] = useState<GeneratedReport | null>(null);
    const [isLoading, setIsLoading] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [showFilters, setShowFilters] = useState(true);

    // Load clients and campaigns
    useEffect(() => {
        loadClientsAndCampaigns();
    }, []);

    const loadClientsAndCampaigns = async () => {
        const api = (window as any).electronAPI;
        if (!api?.getClients) return;

        try {
            const loadedClients = await api.getClients();
            setClients(loadedClients || []);
            
            const loadedCampaigns = await api.getCampaigns();
            setCampaigns(loadedCampaigns || []);
        } catch (e) {
            console.error('Failed to load clients/campaigns:', e);
        }
    };

    const generateReport = useCallback(async () => {
        const api = (window as any).electronAPI;
        if (!api?.generateSalesReport) {
            setError('Sales reporting not available');
            return;
        }

        setIsLoading(true);
        setError(null);

        try {
            const result = await api.generateSalesReport({
                type: selectedReport,
                params: {
                    ...params,
                    startDate: params.startDate ? new Date(params.startDate).getTime() : undefined,
                    endDate: params.endDate ? new Date(params.endDate + 'T23:59:59').getTime() : undefined,
                },
            });

            if (result.error) {
                throw new Error(result.error);
            }

            setReport(result);
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Failed to generate report');
        } finally {
            setIsLoading(false);
        }
    }, [selectedReport, params]);

    const exportReport = useCallback(async (format: 'csv' | 'html' | 'json') => {
        if (!report) return;

        const api = (window as any).electronAPI;
        if (!api?.exportReport) {
            // Fallback: download as file
            let content: string;
            let mimeType: string;
            let extension: string;

            if (format === 'csv') {
                content = convertToCSV(report.data);
                mimeType = 'text/csv';
                extension = 'csv';
            } else if (format === 'html') {
                content = generateHTMLReport(report);
                mimeType = 'text/html';
                extension = 'html';
            } else {
                content = JSON.stringify(report, null, 2);
                mimeType = 'application/json';
                extension = 'json';
            }

            const blob = new Blob([content], { type: mimeType });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `${selectedReport}_${new Date().toISOString().split('T')[0]}.${extension}`;
            a.click();
            URL.revokeObjectURL(url);
            return;
        }

        try {
            await api.exportReport({
                report,
                format,
                filename: `${selectedReport}_${new Date().toISOString().split('T')[0]}`,
            });
        } catch (e) {
            setError('Failed to export report');
        }
    }, [report, selectedReport]);

    const copyToClipboard = useCallback(() => {
        if (!report) return;
        navigator.clipboard.writeText(JSON.stringify(report, null, 2));
    }, [report]);

    const printReport = useCallback(() => {
        if (!report) return;
        const html = generateHTMLReport(report);
        const printWindow = window.open('', '_blank');
        if (printWindow) {
            printWindow.document.write(html);
            printWindow.document.close();
            printWindow.print();
        }
    }, [report]);

    return (
        <div className={styles.container}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <FileText size={24} />
                    <h2>Sales Reports</h2>
                </div>
                <div className={styles.headerActions}>
                    <button
                        className={styles.filterToggle}
                        onClick={() => setShowFilters(!showFilters)}
                    >
                        <Filter size={16} />
                        {showFilters ? 'Hide Filters' : 'Show Filters'}
                    </button>
                </div>
            </div>

            <div className={styles.content}>
                {/* Report Type Selection */}
                <div className={styles.reportTypeGrid}>
                    {REPORT_TYPES.map((rt) => (
                        <button
                            key={rt.id}
                            className={`${styles.reportTypeCard} ${selectedReport === rt.id ? styles.selected : ''}`}
                            onClick={() => setSelectedReport(rt.id)}
                        >
                            <div className={styles.reportTypeIcon}>{rt.icon}</div>
                            <div className={styles.reportTypeInfo}>
                                <span className={styles.reportTypeName}>{rt.name}</span>
                                <span className={styles.reportTypeDesc}>{rt.description}</span>
                            </div>
                        </button>
                    ))}
                </div>

                {/* Filters Panel */}
                {showFilters && (
                    <div className={styles.filtersPanel}>
                        <h3>Report Parameters</h3>
                        <div className={styles.filtersGrid}>
                            <div className={styles.filterGroup}>
                                <label>Start Date</label>
                                <input
                                    type="date"
                                    value={params.startDate || ''}
                                    onChange={(e) => setParams({ ...params, startDate: e.target.value })}
                                    className={styles.input}
                                />
                            </div>
                            <div className={styles.filterGroup}>
                                <label>End Date</label>
                                <input
                                    type="date"
                                    value={params.endDate || ''}
                                    onChange={(e) => setParams({ ...params, endDate: e.target.value })}
                                    className={styles.input}
                                />
                            </div>
                            <div className={styles.filterGroup}>
                                <label>Client</label>
                                <select
                                    value={params.clientId || ''}
                                    onChange={(e) => setParams({ ...params, clientId: e.target.value || undefined })}
                                    className={styles.select}
                                >
                                    <option value="">All Clients</option>
                                    {clients.map((c) => (
                                        <option key={c.id} value={c.id}>{c.name}</option>
                                    ))}
                                </select>
                            </div>
                            <div className={styles.filterGroup}>
                                <label>Campaign</label>
                                <select
                                    value={params.campaignId || ''}
                                    onChange={(e) => setParams({ ...params, campaignId: e.target.value || undefined })}
                                    className={styles.select}
                                >
                                    <option value="">All Campaigns</option>
                                    {campaigns.map((c) => (
                                        <option key={c.id} value={c.id}>{c.name}</option>
                                    ))}
                                </select>
                            </div>
                            <div className={styles.filterGroup}>
                                <label>Day Part</label>
                                <select
                                    value={params.dayPart || ''}
                                    onChange={(e) => setParams({ ...params, dayPart: e.target.value || undefined })}
                                    className={styles.select}
                                >
                                    <option value="">All Day Parts</option>
                                    {DAY_PARTS.map((dp) => (
                                        <option key={dp} value={dp}>{dp}</option>
                                    ))}
                                </select>
                            </div>
                        </div>
                        <div className={styles.generateActions}>
                            <button
                                className={styles.generateBtn}
                                onClick={generateReport}
                                disabled={isLoading}
                            >
                                {isLoading ? (
                                    <>
                                        <RefreshCw size={16} className={styles.spinning} />
                                        Generating...
                                    </>
                                ) : (
                                    <>
                                        <BarChart2 size={16} />
                                        Generate Report
                                    </>
                                )}
                            </button>
                        </div>
                    </div>
                )}

                {/* Error Display */}
                {error && (
                    <div className={styles.errorBanner}>
                        <AlertCircle size={18} />
                        {error}
                    </div>
                )}

                {/* Report Results */}
                {report && (
                    <div className={styles.reportResults}>
                        <div className={styles.reportHeader}>
                            <div className={styles.reportMeta}>
                                <h3>{REPORT_TYPES.find(r => r.id === report.type)?.name || report.type}</h3>
                                <span className={styles.generatedAt}>
                                    Generated: {new Date(report.generatedAt).toLocaleString()}
                                </span>
                            </div>
                            <div className={styles.exportActions}>
                                <button onClick={() => exportReport('csv')} title="Export CSV">
                                    <FileSpreadsheet size={16} />
                                    CSV
                                </button>
                                <button onClick={() => exportReport('html')} title="Export HTML">
                                    <FileCode size={16} />
                                    HTML
                                </button>
                                <button onClick={() => exportReport('json')} title="Export JSON">
                                    <Download size={16} />
                                    JSON
                                </button>
                                <button onClick={copyToClipboard} title="Copy to Clipboard">
                                    <Copy size={16} />
                                </button>
                                <button onClick={printReport} title="Print">
                                    <Printer size={16} />
                                </button>
                            </div>
                        </div>

                        {/* Summary Cards */}
                        {report.summary && (
                            <div className={styles.summaryCards}>
                                {Object.entries(report.summary).map(([key, value]) => (
                                    <div key={key} className={styles.summaryCard}>
                                        <div className={styles.summaryValue}>
                                            {typeof value === 'number' ? value.toLocaleString() : value}
                                        </div>
                                        <div className={styles.summaryLabel}>
                                            {formatLabel(key)}
                                        </div>
                                    </div>
                                ))}
                            </div>
                        )}

                        {/* Data Table */}
                        {report.data && report.data.length > 0 && (
                            <div className={styles.tableWrapper}>
                                <table className={styles.dataTable}>
                                    <thead>
                                        <tr>
                                            {Object.keys(report.data[0]).map((key) => (
                                                <th key={key}>{formatLabel(key)}</th>
                                            ))}
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {report.data.slice(0, 100).map((row, i) => (
                                            <tr key={i}>
                                                {Object.values(row).map((val, j) => (
                                                    <td key={j}>{formatValue(val)}</td>
                                                ))}
                                            </tr>
                                        ))}
                                    </tbody>
                                </table>
                                {report.data.length > 100 && (
                                    <div className={styles.tableNote}>
                                        Showing first 100 rows of {report.data.length} total
                                    </div>
                                )}
                            </div>
                        )}

                        {(!report.data || report.data.length === 0) && (
                            <div className={styles.noData}>
                                <FileText size={48} strokeWidth={1} />
                                <p>No data available for the selected criteria</p>
                            </div>
                        )}
                    </div>
                )}

                {/* Empty State */}
                {!report && !isLoading && (
                    <div className={styles.emptyState}>
                        <BarChart2 size={64} strokeWidth={1} />
                        <h3>Generate a Report</h3>
                        <p>Select a report type and set your parameters, then click Generate Report</p>
                    </div>
                )}
            </div>
        </div>
    );
}

// Helper functions
function formatLabel(key: string): string {
    return key
        .replace(/_/g, ' ')
        .replace(/([A-Z])/g, ' $1')
        .replace(/^./, (s) => s.toUpperCase())
        .trim();
}

function formatValue(val: any): string {
    if (val === null || val === undefined) return '-';
    if (typeof val === 'number') {
        if (Number.isInteger(val)) return val.toLocaleString();
        return val.toFixed(2);
    }
    if (typeof val === 'boolean') return val ? '✓' : '✗';
    return String(val);
}

function convertToCSV(data: any[]): string {
    if (!data || data.length === 0) return '';
    const headers = Object.keys(data[0]);
    const rows = data.map((row) =>
        headers.map((h) => {
            const val = row[h];
            if (typeof val === 'string' && val.includes(',')) {
                return `"${val}"`;
            }
            return val ?? '';
        }).join(',')
    );
    return [headers.join(','), ...rows].join('\n');
}

function generateHTMLReport(report: GeneratedReport): string {
    const data = report.data || [];
    const headers = data.length > 0 ? Object.keys(data[0]) : [];

    return `
<!DOCTYPE html>
<html>
<head>
    <title>${report.type} Report - ${report.generatedAt}</title>
    <style>
        * { box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, sans-serif; 
            margin: 0; 
            padding: 20px;
            background: #f8fafc;
            color: #1e293b;
        }
        .container { 
            max-width: 1200px; 
            margin: 0 auto; 
            background: white; 
            padding: 30px; 
            border-radius: 12px; 
            box-shadow: 0 4px 20px rgba(0,0,0,0.08); 
        }
        h1 { 
            margin: 0 0 8px 0;
            font-size: 28px;
            color: #0f172a;
            font-weight: 700;
        }
        .meta { 
            color: #64748b; 
            margin-bottom: 24px; 
            font-size: 14px;
        }
        .summary { 
            display: flex; 
            gap: 16px; 
            margin-bottom: 24px; 
            flex-wrap: wrap;
        }
        .summary-card { 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            padding: 16px 24px; 
            border-radius: 8px;
            min-width: 140px;
        }
        .summary-value { 
            font-size: 28px; 
            font-weight: 700; 
            color: white; 
        }
        .summary-label { 
            font-size: 12px; 
            color: rgba(255,255,255,0.85); 
            text-transform: uppercase;
            letter-spacing: 0.5px;
            margin-top: 4px;
        }
        table { 
            width: 100%; 
            border-collapse: collapse; 
            margin-top: 20px; 
            font-size: 14px;
        }
        th { 
            background: #f1f5f9; 
            color: #475569; 
            padding: 12px 16px; 
            text-align: left; 
            font-weight: 600;
            text-transform: uppercase;
            font-size: 11px;
            letter-spacing: 0.5px;
            border-bottom: 2px solid #e2e8f0;
        }
        td { 
            padding: 12px 16px; 
            border-bottom: 1px solid #f1f5f9; 
        }
        tr:hover { background: #f8fafc; }
        .no-data {
            text-align: center;
            padding: 60px 20px;
            color: #94a3b8;
        }
        @media print {
            body { background: white; padding: 0; }
            .container { box-shadow: none; }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>${formatLabel(report.type)} Report</h1>
        <div class="meta">Generated: ${new Date(report.generatedAt).toLocaleString()}</div>
        
        ${report.summary ? `
        <div class="summary">
            ${Object.entries(report.summary).map(([key, value]) => `
                <div class="summary-card">
                    <div class="summary-value">${typeof value === 'number' ? value.toLocaleString() : value}</div>
                    <div class="summary-label">${formatLabel(key)}</div>
                </div>
            `).join('')}
        </div>
        ` : ''}
        
        ${data.length > 0 ? `
        <table>
            <thead>
                <tr>${headers.map(h => `<th>${formatLabel(h)}</th>`).join('')}</tr>
            </thead>
            <tbody>
                ${data.map(row => `
                    <tr>${headers.map(h => `<td>${formatValue(row[h])}</td>`).join('')}</tr>
                `).join('')}
            </tbody>
        </table>
        ` : `
        <div class="no-data">No data available</div>
        `}
    </div>
</body>
</html>`;
}

export default SalesReportsDashboard;


