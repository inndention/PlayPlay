// ============================================
// Comprehensive Campaign Management Page
// For sales team to manage ad campaigns, clients, scheduling, and reporting
// ============================================

import React, { useState, useEffect, useMemo, useCallback } from 'react';
import { AdClient, MediaItem, AdMetadata, AsPlayedEntry } from '../lib/types';
import { loadClients, loadAsPlayedLog, loadWeeklySchedule } from '../lib/storage';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { formatDuration } from '../lib/utils';
import { publishSnapshot } from '../lib/reportApi';
import {
    Users, Briefcase, Calendar, BarChart2, DollarSign, Clock,
    Plus, Edit2, Trash2, Search, Filter, Download, Upload,
    ChevronRight, ChevronDown, Play, Pause, CheckCircle, AlertCircle,
    Target, TrendingUp, Eye,     FileText, Settings, RefreshCw,
    PieChart, Activity, Zap, Mail, Phone, MapPin, Globe, ExternalLink
} from 'lucide-react';
import styles from './CampaignManagementPage.module.css';

type TabType = 'dashboard' | 'campaigns' | 'clients' | 'assets' | 'schedule' | 'reports';

interface Campaign {
    id: string;
    name: string;
    clientId: string;
    status: 'draft' | 'active' | 'paused' | 'completed';
    startDate: string;
    endDate: string;
    budget: number;
    spent: number;
    targetImpressions: number;
    actualImpressions: number;
    assets: string[]; // Media IDs
    schedule: {
        sites: string[];
        dayparts: string[]; // 'morning', 'afternoon', 'evening', 'overnight'
        frequency: number; // plays per hour
    };
    createdAt: number;
    updatedAt: number;
}

const generateId = () => Math.random().toString(36).substr(2, 9);

export function CampaignManagementPage() {
    const [activeTab, setActiveTab] = useState<TabType>('dashboard');
    const [clients, setClients] = useState<AdClient[]>([]);
    const [campaigns, setCampaigns] = useState<Campaign[]>([]);
    const [assets, setAssets] = useState<MediaItem[]>([]);
    const [searchQuery, setSearchQuery] = useState('');
    const [selectedCampaign, setSelectedCampaign] = useState<Campaign | null>(null);
    const [selectedClient, setSelectedClient] = useState<AdClient | null>(null);
    const [showNewCampaignModal, setShowNewCampaignModal] = useState(false);
    const [showNewClientModal, setShowNewClientModal] = useState(false);
    const [asPlayedLog, setAsPlayedLog] = useState<AsPlayedEntry[]>([]);
    const [scheduleSlots, setScheduleSlots] = useState<{ id: string; hour: number; itemCount: number; itemTitles: string[] }[]>([]);
    const [reportDateStart, setReportDateStart] = useState(() => new Date(Date.now() - 30 * 86400000).toISOString().split('T')[0]);
    const [reportDateEnd, setReportDateEnd] = useState(() => new Date().toISOString().split('T')[0]);
    const [publishStatus, setPublishStatus] = useState<'idle' | 'publishing' | 'ok' | 'error'>('idle');

    // Load data
    useEffect(() => {
        setClients(loadClients());
        dbFetchMedia().then((media: MediaItem[]) => {
            setAssets(media.filter((m: MediaItem) => m.type === 'ad'));
        }).catch(console.error);
    }, []);

    // Load report data (as-played + schedule)
    useEffect(() => {
        setAsPlayedLog(loadAsPlayedLog());
        loadWeeklySchedule().then((schedule) => {
            const slots = Object.entries(schedule.slots || {}).map(([id, slot]) => ({
                id,
                hour: slot.hour,
                itemCount: (slot.items || []).length,
                itemTitles: (slot.items || []).map((i) => i.media?.title || 'Untitled').slice(0, 5),
            }));
            setScheduleSlots(slots);
        }).catch(() => setScheduleSlots([]));
    }, [activeTab]);

    // Dashboard stats
    const stats = useMemo(() => {
        const activeCampaigns = campaigns.filter(c => c.status === 'active').length;
        const totalBudget = campaigns.reduce((sum, c) => sum + c.budget, 0);
        const totalSpent = campaigns.reduce((sum, c) => sum + c.spent, 0);
        const totalImpressions = campaigns.reduce((sum, c) => sum + c.actualImpressions, 0);
        
        return { activeCampaigns, totalBudget, totalSpent, totalImpressions };
    }, [campaigns]);

    const getClientName = (clientId: string) => {
        const client = clients.find(c => c.id === clientId);
        return client?.name || 'Unknown Client';
    };

    const getStatusColor = (status: Campaign['status']) => {
        switch (status) {
            case 'active': return 'var(--success)';
            case 'paused': return 'var(--warning)';
            case 'completed': return 'var(--accent)';
            default: return 'var(--text-secondary)';
        }
    };

    // Render Dashboard
    const renderDashboard = () => (
        <div className={styles.dashboard}>
            <div className={styles.statsGrid}>
                <div className={styles.statCard}>
                    <div className={styles.statIcon} style={{ background: 'var(--accent)' }}>
                        <Zap size={24} />
                    </div>
                    <div className={styles.statInfo}>
                        <span className={styles.statValue}>{stats.activeCampaigns}</span>
                        <span className={styles.statLabel}>Active Campaigns</span>
                    </div>
                </div>
                <div className={styles.statCard}>
                    <div className={styles.statIcon} style={{ background: 'var(--success)' }}>
                        <DollarSign size={24} />
                    </div>
                    <div className={styles.statInfo}>
                        <span className={styles.statValue}>${stats.totalBudget.toLocaleString()}</span>
                        <span className={styles.statLabel}>Total Budget</span>
                    </div>
                </div>
                <div className={styles.statCard}>
                    <div className={styles.statIcon} style={{ background: 'var(--warning)' }}>
                        <TrendingUp size={24} />
                    </div>
                    <div className={styles.statInfo}>
                        <span className={styles.statValue}>${stats.totalSpent.toLocaleString()}</span>
                        <span className={styles.statLabel}>Total Spent</span>
                    </div>
                </div>
                <div className={styles.statCard}>
                    <div className={styles.statIcon} style={{ background: 'var(--info, #3b82f6)' }}>
                        <Eye size={24} />
                    </div>
                    <div className={styles.statInfo}>
                        <span className={styles.statValue}>{stats.totalImpressions.toLocaleString()}</span>
                        <span className={styles.statLabel}>Total Impressions</span>
                    </div>
                </div>
            </div>

            <div className={styles.dashboardGrid}>
                <div className={styles.recentCampaigns}>
                    <h3><Activity size={16} /> Recent Campaigns</h3>
                    <div className={styles.campaignList}>
                        {campaigns.slice(0, 5).map(campaign => (
                            <div 
                                key={campaign.id} 
                                className={styles.campaignItem}
                                onClick={() => { setSelectedCampaign(campaign); setActiveTab('campaigns'); }}
                            >
                                <div className={styles.campaignInfo}>
                                    <span className={styles.campaignName}>{campaign.name}</span>
                                    <span className={styles.campaignClient}>{getClientName(campaign.clientId)}</span>
                                </div>
                                <div className={styles.campaignMeta}>
                                    <span 
                                        className={styles.campaignStatus}
                                        style={{ color: getStatusColor(campaign.status) }}
                                    >
                                        {campaign.status}
                                    </span>
                                    <div className={styles.progressBar}>
                                        <div 
                                            className={styles.progressFill}
                                            style={{ width: `${(campaign.spent / campaign.budget) * 100}%` }}
                                        />
                                    </div>
                                </div>
                            </div>
                        ))}
                    </div>
                </div>

                <div className={styles.quickActions}>
                    <h3><Zap size={16} /> Quick Actions</h3>
                    <div className={styles.actionButtons}>
                        <button className={styles.actionBtn} onClick={() => setShowNewCampaignModal(true)}>
                            <Plus size={16} /> New Campaign
                        </button>
                        <button className={styles.actionBtn} onClick={() => setShowNewClientModal(true)}>
                            <Users size={16} /> Add Client
                        </button>
                        <button className={styles.actionBtn} onClick={() => setActiveTab('reports')}>
                            <BarChart2 size={16} /> Generate Report
                        </button>
                        <button className={styles.actionBtn} onClick={() => setActiveTab('schedule')}>
                            <Calendar size={16} /> Schedule Ads
                        </button>
                    </div>
                </div>

                <div className={styles.topClients}>
                    <h3><Users size={16} /> Top Clients</h3>
                    <div className={styles.clientList}>
                        {clients.slice(0, 5).map(client => {
                            const clientCampaigns = campaigns.filter(c => c.clientId === client.id);
                            const totalSpend = clientCampaigns.reduce((sum, c) => sum + c.spent, 0);
                            return (
                                <div 
                                    key={client.id} 
                                    className={styles.clientItem}
                                    onClick={() => { setSelectedClient(client); setActiveTab('clients'); }}
                                >
                                    <div className={styles.clientAvatar}>
                                        {client.name.charAt(0).toUpperCase()}
                                    </div>
                                    <div className={styles.clientInfo}>
                                        <span className={styles.clientName}>{client.name}</span>
                                        <span className={styles.clientStats}>
                                            {clientCampaigns.length} campaigns • ${totalSpend.toLocaleString()} spent
                                        </span>
                                    </div>
                                </div>
                            );
                        })}
                    </div>
                </div>
            </div>
        </div>
    );

    // Render Campaigns
    const renderCampaigns = () => (
        <div className={styles.section}>
            <div className={styles.sectionHeader}>
                <h2><Briefcase size={20} /> Campaigns</h2>
                <div className={styles.sectionActions}>
                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input 
                            type="text" 
                            placeholder="Search campaigns..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>
                    <button className={styles.primaryBtn} onClick={() => setShowNewCampaignModal(true)}>
                        <Plus size={16} /> New Campaign
                    </button>
                </div>
            </div>

            <div className={styles.tableContainer}>
                <table className={styles.dataTable}>
                    <thead>
                        <tr>
                            <th>Campaign</th>
                            <th>Client</th>
                            <th>Status</th>
                            <th>Duration</th>
                            <th>Budget</th>
                            <th>Spent</th>
                            <th>Impressions</th>
                            <th>Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        {campaigns.filter(c => 
                            c.name.toLowerCase().includes(searchQuery.toLowerCase()) ||
                            getClientName(c.clientId).toLowerCase().includes(searchQuery.toLowerCase())
                        ).map(campaign => (
                            <tr key={campaign.id}>
                                <td>
                                    <div className={styles.campaignCell}>
                                        <span className={styles.campaignTitle}>{campaign.name}</span>
                                        <span className={styles.campaignId}>{campaign.id}</span>
                                    </div>
                                </td>
                                <td>{getClientName(campaign.clientId)}</td>
                                <td>
                                    <span 
                                        className={styles.statusBadge}
                                        style={{ 
                                            background: `${getStatusColor(campaign.status)}20`,
                                            color: getStatusColor(campaign.status)
                                        }}
                                    >
                                        {campaign.status}
                                    </span>
                                </td>
                                <td>{campaign.startDate} - {campaign.endDate}</td>
                                <td>${campaign.budget.toLocaleString()}</td>
                                <td>
                                    <div className={styles.spentCell}>
                                        ${campaign.spent.toLocaleString()}
                                        <div className={styles.miniProgress}>
                                            <div 
                                                style={{ width: `${(campaign.spent / campaign.budget) * 100}%` }}
                                            />
                                        </div>
                                    </div>
                                </td>
                                <td>
                                    {campaign.actualImpressions.toLocaleString()} / {campaign.targetImpressions.toLocaleString()}
                                </td>
                                <td>
                                    <div className={styles.actionBtns}>
                                        <button title="Edit"><Edit2 size={14} /></button>
                                        <button title="View Report"><BarChart2 size={14} /></button>
                                        {campaign.status === 'active' ? (
                                            <button title="Pause"><Pause size={14} /></button>
                                        ) : (
                                            <button title="Activate"><Play size={14} /></button>
                                        )}
                                    </div>
                                </td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            </div>
        </div>
    );

    // Render Clients
    const renderClients = () => (
        <div className={styles.section}>
            <div className={styles.sectionHeader}>
                <h2><Users size={20} /> Clients</h2>
                <div className={styles.sectionActions}>
                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input 
                            type="text" 
                            placeholder="Search clients..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>
                    <button className={styles.primaryBtn} onClick={() => setShowNewClientModal(true)}>
                        <Plus size={16} /> Add Client
                    </button>
                </div>
            </div>

            <div className={styles.clientsGrid}>
                {clients.filter(c => 
                    c.name.toLowerCase().includes(searchQuery.toLowerCase())
                ).map(client => {
                    const clientCampaigns = campaigns.filter(c => c.clientId === client.id);
                    const activeCampaigns = clientCampaigns.filter(c => c.status === 'active').length;
                    const totalSpend = clientCampaigns.reduce((sum, c) => sum + c.spent, 0);
                    const clientAssets = assets.filter(a => a.client === client.id);
                    
                    return (
                        <div key={client.id} className={styles.clientCard}>
                            <div className={styles.clientCardHeader}>
                                <div className={styles.clientAvatar}>
                                    {client.name.charAt(0).toUpperCase()}
                                </div>
                                <div className={styles.clientCardInfo}>
                                    <h3>{client.name}</h3>
                                    <span className={styles.clientCode}>ID: {client.id}</span>
                                </div>
                                <div className={styles.clientCardActions}>
                                    <button><Edit2 size={14} /></button>
                                </div>
                            </div>
                            <div className={styles.clientCardBody}>
                                <div className={styles.clientStat}>
                                    <Briefcase size={14} />
                                    <span>{clientCampaigns.length} campaigns ({activeCampaigns} active)</span>
                                </div>
                                <div className={styles.clientStat}>
                                    <DollarSign size={14} />
                                    <span>${totalSpend.toLocaleString()} total spend</span>
                                </div>
                                <div className={styles.clientStat}>
                                    <FileText size={14} />
                                    <span>{clientAssets.length} assets</span>
                                </div>
                            </div>
                            <div className={styles.clientCardFooter}>
                                <button className={styles.viewBtn}>
                                    View Details <ChevronRight size={14} />
                                </button>
                            </div>
                        </div>
                    );
                })}
            </div>
        </div>
    );

    // Render Assets
    const renderAssets = () => (
        <div className={styles.section}>
            <div className={styles.sectionHeader}>
                <h2><FileText size={20} /> Ad Assets</h2>
                <div className={styles.sectionActions}>
                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input 
                            type="text" 
                            placeholder="Search assets..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>
                    <button className={styles.secondaryBtn}>
                        <Upload size={16} /> Upload Assets
                    </button>
                </div>
            </div>

            <div className={styles.assetsGrid}>
                {assets.filter(a => 
                    a.title.toLowerCase().includes(searchQuery.toLowerCase())
                ).map(asset => (
                    <div key={asset.id} className={styles.assetCard}>
                        <div className={styles.assetThumb}>
                            {asset.thumbnail ? (
                                <img src={asset.thumbnail} alt={asset.title} />
                            ) : (
                                <Play size={32} />
                            )}
                        </div>
                        <div className={styles.assetInfo}>
                            <h4>{asset.title}</h4>
                            <div className={styles.assetMeta}>
                                <span><Clock size={12} /> {formatDuration(asset.duration)}</span>
                                <span><Users size={12} /> {asset.client || 'Unassigned'}</span>
                            </div>
                        </div>
                    </div>
                ))}
            </div>
        </div>
    );

    // Render Schedule
    const renderSchedule = () => (
        <div className={styles.section}>
            <div className={styles.sectionHeader}>
                <h2><Calendar size={20} /> Ad Schedule</h2>
                <div className={styles.sectionActions}>
                    <button className={styles.secondaryBtn}>
                        <RefreshCw size={16} /> Auto-Schedule
                    </button>
                </div>
            </div>

            <div className={styles.scheduleGrid}>
                <div className={styles.scheduleInfo}>
                    <p>Configure when and where campaign ads appear in the schedule.</p>
                    <p>Use the main scheduler to place ads, or use auto-fill to distribute based on campaign parameters.</p>
                </div>
                
                <div className={styles.scheduleActions}>
                    <button className={styles.primaryBtn}>
                        <Calendar size={16} /> Open Scheduler
                    </button>
                    <button className={styles.secondaryBtn}>
                        <Settings size={16} /> Campaign Rules
                    </button>
                </div>
            </div>
        </div>
    );

    // Filter as-played by date range
    const filteredAsPlayed = useMemo(() => {
        const start = new Date(reportDateStart).getTime();
        const end = new Date(reportDateEnd).getTime() + 86400000;
        return asPlayedLog.filter((e) => e.timestamp >= start && e.timestamp < end);
    }, [asPlayedLog, reportDateStart, reportDateEnd]);

    const handleExportAsPlayedCsv = () => {
        const headers = ['Date', 'Time', 'Asset', 'Media ID', 'Duration (s)', 'Status', 'Slot'];
        const rows = filteredAsPlayed.map((e) => [
            new Date(e.timestamp).toLocaleDateString(),
            new Date(e.timestamp).toLocaleTimeString(),
            `"${(e.mediaTitle || '').replace(/"/g, '""')}"`,
            e.mediaId,
            e.duration,
            e.completed ? 'Full Play' : 'Partial',
            e.hourSlot || '',
        ]);
        const csv = [headers.join(','), ...rows.map((r) => r.join(','))].join('\n');
        const blob = new Blob([csv], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `as-played-${reportDateStart}-to-${reportDateEnd}.csv`;
        a.click();
        URL.revokeObjectURL(url);
    };

    const handleExportScheduledCsv = () => {
        const headers = ['Slot ID', 'Hour', 'Item Count', 'Sample Titles'];
        const rows = scheduleSlots.map((s) => [
            s.id,
            s.hour,
            s.itemCount,
            `"${s.itemTitles.join('; ').replace(/"/g, '""')}"`,
        ]);
        const csv = [headers.join(','), ...rows.map((r) => r.join(','))].join('\n');
        const blob = new Blob([csv], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `scheduled-${new Date().toISOString().split('T')[0]}.csv`;
        a.click();
        URL.revokeObjectURL(url);
    };

    const handlePublishToApi = async () => {
        setPublishStatus('publishing');
        try {
            const scheduleSlotSummaries = scheduleSlots.map((s) => ({
                id: s.id,
                hour: s.hour,
                itemCount: s.itemCount,
                itemTitles: s.itemTitles,
            }));
            const ok = await publishSnapshot({
                asPlayed: asPlayedLog.map((e) => ({
                    mediaId: e.mediaId,
                    timestamp: e.timestamp,
                    duration: e.duration,
                    completed: e.completed,
                    mediaTitle: e.mediaTitle,
                    hourSlot: e.hourSlot,
                    itemId: e.itemId,
                    scheduledStart: e.scheduledStart,
                    actualStart: e.actualStart,
                    actualEnd: e.actualEnd,
                    source: e.source,
                })),
                scheduleSlots: scheduleSlotSummaries,
                clients: clients.map((c) => ({ id: c.id, name: c.name })),
            });
            setPublishStatus(ok ? 'ok' : 'error');
        } catch {
            setPublishStatus('error');
        }
    };

    // Render Reports
    const renderReports = () => (
        <div className={styles.section}>
            <div className={styles.sectionHeader}>
                <h2><BarChart2 size={20} /> Reports & Analytics</h2>
                <div className={styles.sectionActions}>
                    <button className={styles.primaryBtn} onClick={handleExportAsPlayedCsv}>
                        <Download size={16} /> Export As-Played CSV
                    </button>
                    <button className={styles.secondaryBtn} onClick={handleExportScheduledCsv}>
                        <Download size={16} /> Export Scheduled CSV
                    </button>
                    <button
                        className={styles.secondaryBtn}
                        onClick={handlePublishToApi}
                        disabled={publishStatus === 'publishing'}
                    >
                        {publishStatus === 'publishing' ? 'Publishing…' : publishStatus === 'ok' ? 'Published' : publishStatus === 'error' ? 'Failed' : 'Publish to API (external access)'}
                    </button>
                    <a href="#/reports" target="_blank" rel="noopener noreferrer" className={styles.secondaryBtn} style={{ display: 'inline-flex', alignItems: 'center', gap: '0.5rem', textDecoration: 'none', color: 'inherit' }}>
                        <ExternalLink size={16} /> Sales Reports (browser)
                    </a>
                </div>
            </div>

            <div className={styles.reportFilters}>
                <label>Date range (as-played):</label>
                <input type="date" value={reportDateStart} onChange={(e) => setReportDateStart(e.target.value)} />
                <span>to</span>
                <input type="date" value={reportDateEnd} onChange={(e) => setReportDateEnd(e.target.value)} />
            </div>

            <h3 className={styles.reportSubhead}><Activity size={16} /> Asset As-Played Log</h3>
            <div className={styles.tableContainer}>
                <table className={styles.dataTable}>
                    <thead>
                        <tr>
                            <th>Date & Time</th>
                            <th>Asset</th>
                            <th>Media ID</th>
                            <th>Duration</th>
                            <th>Status</th>
                            <th>Slot</th>
                        </tr>
                    </thead>
                    <tbody>
                        {filteredAsPlayed.slice(0, 500).map((e, idx) => (
                            <tr key={idx}>
                                <td>{new Date(e.timestamp).toLocaleString()}</td>
                                <td>{e.mediaTitle || '—'}</td>
                                <td><code>{e.mediaId}</code></td>
                                <td>{e.duration}s</td>
                                <td>{e.completed ? 'Full Play' : 'Partial'}</td>
                                <td>{e.hourSlot || '—'}</td>
                            </tr>
                        ))}
                    </tbody>
                </table>
                {filteredAsPlayed.length > 500 && <p className={styles.reportNote}>Showing first 500 of {filteredAsPlayed.length}.</p>}
            </div>

            <h3 className={styles.reportSubhead}><Calendar size={16} /> Scheduled (by slot)</h3>
            <div className={styles.tableContainer}>
                <table className={styles.dataTable}>
                    <thead>
                        <tr>
                            <th>Slot ID</th>
                            <th>Hour</th>
                            <th>Items</th>
                            <th>Sample titles</th>
                        </tr>
                    </thead>
                    <tbody>
                        {scheduleSlots.map((s) => (
                            <tr key={s.id}>
                                <td><code>{s.id}</code></td>
                                <td>{s.hour}:00</td>
                                <td>{s.itemCount}</td>
                                <td>{s.itemTitles.join(', ') || '—'}</td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            </div>
        </div>
    );

    return (
        <div className={styles.container}>
            {/* Sidebar Navigation */}
            <aside className={styles.sidebar}>
                <div className={styles.sidebarHeader}>
                    <Briefcase size={24} />
                    <span>Campaign Manager</span>
                </div>
                <nav className={styles.nav}>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'dashboard' ? styles.active : ''}`}
                        onClick={() => setActiveTab('dashboard')}
                    >
                        <Activity size={18} /> Dashboard
                    </button>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'campaigns' ? styles.active : ''}`}
                        onClick={() => setActiveTab('campaigns')}
                    >
                        <Briefcase size={18} /> Campaigns
                    </button>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'clients' ? styles.active : ''}`}
                        onClick={() => setActiveTab('clients')}
                    >
                        <Users size={18} /> Clients
                    </button>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'assets' ? styles.active : ''}`}
                        onClick={() => setActiveTab('assets')}
                    >
                        <FileText size={18} /> Assets
                    </button>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'schedule' ? styles.active : ''}`}
                        onClick={() => setActiveTab('schedule')}
                    >
                        <Calendar size={18} /> Schedule
                    </button>
                    <button 
                        className={`${styles.navItem} ${activeTab === 'reports' ? styles.active : ''}`}
                        onClick={() => setActiveTab('reports')}
                    >
                        <BarChart2 size={18} /> Reports
                    </button>
                </nav>
            </aside>

            {/* Main Content */}
            <main className={styles.main}>
                {activeTab === 'dashboard' && renderDashboard()}
                {activeTab === 'campaigns' && renderCampaigns()}
                {activeTab === 'clients' && renderClients()}
                {activeTab === 'assets' && renderAssets()}
                {activeTab === 'schedule' && renderSchedule()}
                {activeTab === 'reports' && renderReports()}
            </main>
        </div>
    );
}

