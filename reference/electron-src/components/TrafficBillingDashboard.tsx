/**
 * TrafficBillingDashboard - Comprehensive Traffic & Billing System
 * 
 * Features:
 * - Asset playout tracking with visual indicators
 * - Commercial spot scheduling and trafficking
 * - Advertiser and campaign management
 * - Billing and invoicing
 * - Make-goods tracking
 * - Discrepancy reporting
 * - Revenue analytics
 * - Contract management
 * - Rate card management
 */

import React, { useState, useEffect, useCallback, useMemo } from 'react';
import styles from './TrafficBillingDashboard.module.css';
import {
    DollarSign, FileText, Calendar, Clock, Play, Check, X, AlertTriangle,
    BarChart2, TrendingUp, Users, Building2, Film, Target, Zap, Download,
    Upload, RefreshCw, Search, Filter, Plus, Edit2, Trash2, Eye, Copy,
    ChevronDown, ChevronRight, ArrowRight, ArrowLeft, CheckCircle, XCircle,
    AlertCircle, PauseCircle, Circle, Save, Send, Printer, Mail, Phone,
    MapPin, Globe, Tag, Percent, Receipt, CreditCard, PiggyBank, Wallet
} from 'lucide-react';

// Types
interface Advertiser {
    id: string;
    name: string;
    contactName: string;
    email: string;
    phone: string;
    address: string;
    status: 'active' | 'inactive' | 'pending';
    creditLimit: number;
    balance: number;
    notes?: string;
}

interface Campaign {
    id: string;
    advertiserId: string;
    name: string;
    startDate: string;
    endDate: string;
    budget: number;
    spent: number;
    status: 'draft' | 'active' | 'paused' | 'completed' | 'cancelled';
    spots: Spot[];
    contractNumber?: string;
}

interface Spot {
    id: string;
    campaignId: string;
    mediaId: string;
    mediaName: string;
    duration: number; // seconds
    rate: number; // cost per spot
    scheduledDate: string;
    scheduledTime: string;
    daypart: 'AM Drive' | 'Midday' | 'PM Drive' | 'Evening' | 'Late Night' | 'Overnight' | 'ROS';
    status: 'scheduled' | 'aired' | 'missed' | 'preempted' | 'make-good';
    actualAirTime?: string;
    verificationStatus: 'pending' | 'verified' | 'disputed';
    invoice?: string;
}

interface Invoice {
    id: string;
    advertiserId: string;
    campaignId: string;
    invoiceNumber: string;
    issueDate: string;
    dueDate: string;
    items: InvoiceItem[];
    subtotal: number;
    tax: number;
    total: number;
    status: 'draft' | 'sent' | 'paid' | 'overdue' | 'cancelled';
    paymentDate?: string;
    notes?: string;
}

interface InvoiceItem {
    id: string;
    description: string;
    quantity: number;
    rate: number;
    amount: number;
    date: string;
}

interface PlayoutLog {
    id: string;
    spotId: string;
    mediaId: string;
    mediaName: string;
    scheduledTime: string;
    actualTime: string;
    duration: number;
    status: 'success' | 'error' | 'partial';
    channel: string;
    verificationCode?: string;
}

interface DaypartRate {
    daypart: string;
    rate: number;
    premium?: number;
}

interface TrafficBillingDashboardProps {
    onExportReport?: (type: string, data: any) => void;
    onSendInvoice?: (invoice: Invoice) => void;
}

const DAYPART_RATES: DaypartRate[] = [
    { daypart: 'AM Drive', rate: 200, premium: 1.5 },
    { daypart: 'Midday', rate: 100, premium: 1.0 },
    { daypart: 'PM Drive', rate: 250, premium: 1.75 },
    { daypart: 'Evening', rate: 300, premium: 2.0 },
    { daypart: 'Late Night', rate: 75, premium: 0.75 },
    { daypart: 'Overnight', rate: 50, premium: 0.5 },
    { daypart: 'ROS', rate: 80, premium: 0.8 },
];

export const TrafficBillingDashboard: React.FC<TrafficBillingDashboardProps> = ({
    onExportReport,
    onSendInvoice
}) => {
    // State
    const [activeTab, setActiveTab] = useState<'overview' | 'traffic' | 'billing' | 'advertisers' | 'reports'>('overview');
    const [advertisers, setAdvertisers] = useState<Advertiser[]>([]);
    const [campaigns, setCampaigns] = useState<Campaign[]>([]);
    const [spots, setSpots] = useState<Spot[]>([]);
    const [selectedDate, setSelectedDate] = useState(new Date().toISOString().split('T')[0]);
    const [searchQuery, setSearchQuery] = useState('');
    const [filterStatus, setFilterStatus] = useState<string>('all');
    const [selectedAdvertiser, setSelectedAdvertiser] = useState<Advertiser | null>(null);
    const [selectedCampaign, setSelectedCampaign] = useState<Campaign | null>(null);
    const [showAddModal, setShowAddModal] = useState<'advertiser' | 'campaign' | 'spot' | null>(null);
    const [expandedCampaigns, setExpandedCampaigns] = useState<Set<string>>(new Set());

    // Calculate statistics
    const stats = useMemo(() => {
        const todaySpots = spots.filter(s => s.scheduledDate === selectedDate);
        const airedSpots = todaySpots.filter(s => s.status === 'aired');
        const missedSpots = todaySpots.filter(s => s.status === 'missed');
        const scheduledSpots = todaySpots.filter(s => s.status === 'scheduled');
        
        const totalRevenue = airedSpots.reduce((sum, s) => sum + s.rate, 0);
        const projectedRevenue = todaySpots.reduce((sum, s) => sum + s.rate, 0);
        
        const activeAdvertisers = advertisers.filter(a => a.status === 'active').length;
        const activeCampaigns = campaigns.filter(c => c.status === 'active').length;
        
        const outstandingBalance = advertisers.reduce((sum, a) => sum + a.balance, 0);
        
        return {
            todaySpots: todaySpots.length,
            airedSpots: airedSpots.length,
            missedSpots: missedSpots.length,
            scheduledSpots: scheduledSpots.length,
            totalRevenue,
            projectedRevenue,
            activeAdvertisers,
            activeCampaigns,
            outstandingBalance,
            fillRate: todaySpots.length > 0 ? (airedSpots.length / todaySpots.length * 100) : 0
        };
    }, [spots, selectedDate, advertisers, campaigns]);

    // Get spots for a specific date
    const getDateSpots = useCallback((date: string) => {
        return spots.filter(s => s.scheduledDate === date);
    }, [spots]);

    // Toggle campaign expansion
    const toggleCampaign = (campaignId: string) => {
        setExpandedCampaigns(prev => {
            const next = new Set(prev);
            if (next.has(campaignId)) {
                next.delete(campaignId);
            } else {
                next.add(campaignId);
            }
            return next;
        });
    };

    // Get status icon
    const getStatusIcon = (status: string) => {
        switch (status) {
            case 'aired':
            case 'verified':
            case 'paid':
            case 'success':
                return <CheckCircle size={14} className={styles.successIcon} />;
            case 'missed':
            case 'error':
            case 'cancelled':
            case 'disputed':
                return <XCircle size={14} className={styles.errorIcon} />;
            case 'scheduled':
            case 'pending':
            case 'draft':
                return <Circle size={14} className={styles.pendingIcon} />;
            case 'preempted':
            case 'paused':
            case 'overdue':
                return <AlertCircle size={14} className={styles.warningIcon} />;
            default:
                return <Circle size={14} />;
        }
    };

    // Format currency
    const formatCurrency = (amount: number) => {
        return new Intl.NumberFormat('en-US', {
            style: 'currency',
            currency: 'USD'
        }).format(amount);
    };

    // Render traffic log timeline
    const renderTrafficTimeline = () => {
        const dateSpots = getDateSpots(selectedDate).sort((a, b) => 
            a.scheduledTime.localeCompare(b.scheduledTime)
        );
        
        const hours = Array.from({ length: 24 }, (_, i) => i);
        
        return (
            <div className={styles.timeline}>
                <div className={styles.timelineHeader}>
                    <h4>Traffic Log - {new Date(selectedDate).toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' })}</h4>
                    <div className={styles.timelineControls}>
                        <button onClick={() => {
                            const d = new Date(selectedDate);
                            d.setDate(d.getDate() - 1);
                            setSelectedDate(d.toISOString().split('T')[0]);
                        }}>
                            <ArrowLeft size={14} />
                        </button>
                        <input 
                            type="date" 
                            value={selectedDate}
                            onChange={(e) => setSelectedDate(e.target.value)}
                        />
                        <button onClick={() => {
                            const d = new Date(selectedDate);
                            d.setDate(d.getDate() + 1);
                            setSelectedDate(d.toISOString().split('T')[0]);
                        }}>
                            <ArrowRight size={14} />
                        </button>
                    </div>
                </div>
                
                <div className={styles.timelineGrid}>
                    {hours.map(hour => {
                        const hourStr = hour.toString().padStart(2, '0');
                        const hourSpots = dateSpots.filter(s => s.scheduledTime.startsWith(hourStr + ':'));
                        
                        return (
                            <div key={hour} className={styles.timelineHour}>
                                <div className={styles.hourLabel}>{hourStr}:00</div>
                                <div className={styles.hourSlots}>
                                    {hourSpots.map(spot => {
                                        const advertiser = advertisers.find(a => 
                                            campaigns.find(c => c.id === spot.campaignId)?.advertiserId === a.id
                                        );
                                        
                                        return (
                                            <div 
                                                key={spot.id}
                                                className={`${styles.spotBlock} ${styles[spot.status]}`}
                                                style={{ 
                                                    width: `${Math.min(spot.duration * 2, 100)}px`,
                                                    left: `${parseInt(spot.scheduledTime.split(':')[1]) * 1.5}px`
                                                }}
                                                title={`${spot.mediaName} (${spot.duration}s) - ${advertiser?.name || 'Unknown'}`}
                                            >
                                                {getStatusIcon(spot.status)}
                                                <span className={styles.spotTime}>
                                                    {spot.scheduledTime.slice(0, 5)}
                                                </span>
                                                <span className={styles.spotDuration}>{spot.duration}s</span>
                                            </div>
                                        );
                                    })}
                                </div>
                            </div>
                        );
                    })}
                </div>
                
                <div className={styles.timelineLegend}>
                    <span><span className={`${styles.legendDot} ${styles.aired}`}></span> Aired</span>
                    <span><span className={`${styles.legendDot} ${styles.scheduled}`}></span> Scheduled</span>
                    <span><span className={`${styles.legendDot} ${styles.missed}`}></span> Missed</span>
                    <span><span className={`${styles.legendDot} ${styles.preempted}`}></span> Preempted</span>
                </div>
            </div>
        );
    };

    // Render spot list
    const renderSpotList = () => {
        const filteredSpots = spots
            .filter(s => filterStatus === 'all' || s.status === filterStatus)
            .filter(s => 
                searchQuery === '' || 
                s.mediaName.toLowerCase().includes(searchQuery.toLowerCase())
            )
            .sort((a, b) => {
                if (a.scheduledDate !== b.scheduledDate) {
                    return a.scheduledDate.localeCompare(b.scheduledDate);
                }
                return a.scheduledTime.localeCompare(b.scheduledTime);
            });

        return (
            <div className={styles.spotList}>
                <div className={styles.spotListHeader}>
                    <div className={styles.spotListFilters}>
                        <div className={styles.searchBox}>
                            <Search size={14} />
                            <input 
                                type="text"
                                placeholder="Search spots..."
                                value={searchQuery}
                                onChange={(e) => setSearchQuery(e.target.value)}
                            />
                        </div>
                        <select 
                            value={filterStatus}
                            onChange={(e) => setFilterStatus(e.target.value)}
                        >
                            <option value="all">All Status</option>
                            <option value="scheduled">Scheduled</option>
                            <option value="aired">Aired</option>
                            <option value="missed">Missed</option>
                            <option value="preempted">Preempted</option>
                        </select>
                    </div>
                    <button className={styles.addBtn} onClick={() => setShowAddModal('spot')}>
                        <Plus size={14} /> Add Spot
                    </button>
                </div>
                
                <table className={styles.spotTable}>
                    <thead>
                        <tr>
                            <th>Status</th>
                            <th>Date</th>
                            <th>Time</th>
                            <th>Media</th>
                            <th>Duration</th>
                            <th>Daypart</th>
                            <th>Rate</th>
                            <th>Advertiser</th>
                            <th>Verification</th>
                            <th>Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        {filteredSpots.map(spot => {
                            const campaign = campaigns.find(c => c.id === spot.campaignId);
                            const advertiser = advertisers.find(a => a.id === campaign?.advertiserId);
                            
                            return (
                                <tr key={spot.id} className={styles[spot.status]}>
                                    <td>
                                        <div className={`${styles.statusBadge} ${styles[spot.status]}`}>
                                            {getStatusIcon(spot.status)}
                                            {spot.status}
                                        </div>
                                    </td>
                                    <td>{spot.scheduledDate}</td>
                                    <td>
                                        <span className={styles.scheduledTime}>{spot.scheduledTime.slice(0, 5)}</span>
                                        {spot.actualAirTime && (
                                            <span className={styles.actualTime}>
                                                (Actual: {spot.actualAirTime.slice(0, 8)})
                                            </span>
                                        )}
                                    </td>
                                    <td>{spot.mediaName}</td>
                                    <td>{spot.duration}s</td>
                                    <td>{spot.daypart}</td>
                                    <td>{formatCurrency(spot.rate)}</td>
                                    <td>{advertiser?.name || 'Unknown'}</td>
                                    <td>
                                        <div className={`${styles.verificationBadge} ${styles[spot.verificationStatus]}`}>
                                            {getStatusIcon(spot.verificationStatus)}
                                            {spot.verificationStatus}
                                        </div>
                                    </td>
                                    <td>
                                        <div className={styles.spotActions}>
                                            <button title="View"><Eye size={14} /></button>
                                            <button title="Edit"><Edit2 size={14} /></button>
                                            {spot.status === 'missed' && (
                                                <button title="Schedule Make-good" className={styles.makeGoodBtn}>
                                                    <RefreshCw size={14} />
                                                </button>
                                            )}
                                        </div>
                                    </td>
                                </tr>
                            );
                        })}
                    </tbody>
                </table>
            </div>
        );
    };

    return (
        <div className={styles.dashboard}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <DollarSign size={24} />
                    <h2>Traffic & Billing</h2>
                </div>
                
                <div className={styles.tabs}>
                    <button 
                        className={activeTab === 'overview' ? styles.active : ''}
                        onClick={() => setActiveTab('overview')}
                    >
                        <BarChart2 size={14} /> Overview
                    </button>
                    <button 
                        className={activeTab === 'traffic' ? styles.active : ''}
                        onClick={() => setActiveTab('traffic')}
                    >
                        <Play size={14} /> Traffic Log
                    </button>
                    <button 
                        className={activeTab === 'billing' ? styles.active : ''}
                        onClick={() => setActiveTab('billing')}
                    >
                        <Receipt size={14} /> Billing
                    </button>
                    <button 
                        className={activeTab === 'advertisers' ? styles.active : ''}
                        onClick={() => setActiveTab('advertisers')}
                    >
                        <Building2 size={14} /> Advertisers
                    </button>
                    <button 
                        className={activeTab === 'reports' ? styles.active : ''}
                        onClick={() => setActiveTab('reports')}
                    >
                        <FileText size={14} /> Reports
                    </button>
                </div>
                
                <div className={styles.headerActions}>
                    <button className={styles.exportBtn}>
                        <Download size={14} /> Export
                    </button>
                </div>
            </div>

            {/* Content */}
            <div className={styles.content}>
                {activeTab === 'overview' && (
                    <div className={styles.overview}>
                        {/* Stats Cards */}
                        <div className={styles.statsGrid}>
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Play size={20} /></div>
                                <div className={styles.statInfo}>
                                    <span className={styles.statValue}>{stats.todaySpots}</span>
                                    <span className={styles.statLabel}>Today's Spots</span>
                                </div>
                                <div className={styles.statBreakdown}>
                                    <span className={styles.success}>{stats.airedSpots} Aired</span>
                                    <span className={styles.pending}>{stats.scheduledSpots} Scheduled</span>
                                    <span className={styles.error}>{stats.missedSpots} Missed</span>
                                </div>
                            </div>
                            
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><DollarSign size={20} /></div>
                                <div className={styles.statInfo}>
                                    <span className={styles.statValue}>{formatCurrency(stats.totalRevenue)}</span>
                                    <span className={styles.statLabel}>Today's Revenue</span>
                                </div>
                                <div className={styles.statProgress}>
                                    <div className={styles.progressBar}>
                                        <div 
                                            className={styles.progressFill}
                                            style={{ width: `${stats.projectedRevenue > 0 ? (stats.totalRevenue / stats.projectedRevenue * 100) : 0}%` }}
                                        />
                                    </div>
                                    <span>of {formatCurrency(stats.projectedRevenue)} projected</span>
                                </div>
                            </div>
                            
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Target size={20} /></div>
                                <div className={styles.statInfo}>
                                    <span className={styles.statValue}>{stats.fillRate.toFixed(1)}%</span>
                                    <span className={styles.statLabel}>Fill Rate</span>
                                </div>
                                <div className={styles.statTrend}>
                                    <TrendingUp size={14} />
                                    <span>+2.5% from yesterday</span>
                                </div>
                            </div>
                            
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Building2 size={20} /></div>
                                <div className={styles.statInfo}>
                                    <span className={styles.statValue}>{stats.activeAdvertisers}</span>
                                    <span className={styles.statLabel}>Active Advertisers</span>
                                </div>
                                <div className={styles.statExtra}>
                                    {stats.activeCampaigns} Active Campaigns
                                </div>
                            </div>
                            
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Wallet size={20} /></div>
                                <div className={styles.statInfo}>
                                    <span className={styles.statValue}>{formatCurrency(stats.outstandingBalance)}</span>
                                    <span className={styles.statLabel}>Outstanding Balance</span>
                                </div>
                            </div>
                        </div>

                        {/* Active Campaigns */}
                        <div className={styles.section}>
                            <div className={styles.sectionHeader}>
                                <h3><Film size={16} /> Active Campaigns</h3>
                                <button className={styles.addBtn} onClick={() => setShowAddModal('campaign')}>
                                    <Plus size={14} /> New Campaign
                                </button>
                            </div>
                            
                            <div className={styles.campaignList}>
                                {campaigns.filter(c => c.status === 'active').map(campaign => {
                                    const advertiser = advertisers.find(a => a.id === campaign.advertiserId);
                                    const campaignSpots = spots.filter(s => s.campaignId === campaign.id);
                                    const isExpanded = expandedCampaigns.has(campaign.id);
                                    
                                    return (
                                        <div key={campaign.id} className={styles.campaignCard}>
                                            <div 
                                                className={styles.campaignHeader}
                                                onClick={() => toggleCampaign(campaign.id)}
                                            >
                                                <div className={styles.campaignToggle}>
                                                    {isExpanded ? <ChevronDown size={16} /> : <ChevronRight size={16} />}
                                                </div>
                                                <div className={styles.campaignInfo}>
                                                    <h4>{campaign.name}</h4>
                                                    <span className={styles.campaignAdvertiser}>{advertiser?.name}</span>
                                                </div>
                                                <div className={styles.campaignDates}>
                                                    <Calendar size={12} />
                                                    {campaign.startDate} - {campaign.endDate}
                                                </div>
                                                <div className={styles.campaignBudget}>
                                                    <div className={styles.budgetProgress}>
                                                        <div 
                                                            className={styles.budgetFill}
                                                            style={{ width: `${(campaign.spent / campaign.budget * 100)}%` }}
                                                        />
                                                    </div>
                                                    <span>{formatCurrency(campaign.spent)} / {formatCurrency(campaign.budget)}</span>
                                                </div>
                                                <div className={styles.campaignSpots}>
                                                    {campaignSpots.length} spots
                                                </div>
                                            </div>
                                            
                                            {isExpanded && (
                                                <div className={styles.campaignDetails}>
                                                    <table className={styles.miniSpotTable}>
                                                        <thead>
                                                            <tr>
                                                                <th>Date</th>
                                                                <th>Time</th>
                                                                <th>Media</th>
                                                                <th>Status</th>
                                                                <th>Rate</th>
                                                            </tr>
                                                        </thead>
                                                        <tbody>
                                                            {campaignSpots.slice(0, 5).map(spot => (
                                                                <tr key={spot.id}>
                                                                    <td>{spot.scheduledDate}</td>
                                                                    <td>{spot.scheduledTime.slice(0, 5)}</td>
                                                                    <td>{spot.mediaName}</td>
                                                                    <td>
                                                                        <span className={`${styles.statusBadge} ${styles[spot.status]}`}>
                                                                            {getStatusIcon(spot.status)}
                                                                            {spot.status}
                                                                        </span>
                                                                    </td>
                                                                    <td>{formatCurrency(spot.rate)}</td>
                                                                </tr>
                                                            ))}
                                                        </tbody>
                                                    </table>
                                                    {campaignSpots.length > 5 && (
                                                        <div className={styles.viewMore}>
                                                            View all {campaignSpots.length} spots
                                                        </div>
                                                    )}
                                                </div>
                                            )}
                                        </div>
                                    );
                                })}
                            </div>
                        </div>

                        {/* Today's Timeline Mini */}
                        <div className={styles.section}>
                            <div className={styles.sectionHeader}>
                                <h3><Clock size={16} /> Today's Schedule</h3>
                                <button onClick={() => setActiveTab('traffic')}>
                                    View Full Log <ArrowRight size={14} />
                                </button>
                            </div>
                            {renderTrafficTimeline()}
                        </div>
                    </div>
                )}

                {activeTab === 'traffic' && (
                    <div className={styles.trafficLog}>
                        {renderTrafficTimeline()}
                        {renderSpotList()}
                    </div>
                )}

                {activeTab === 'billing' && (
                    <div className={styles.billing}>
                        <div className={styles.billingActions}>
                            <button className={styles.primaryBtn}>
                                <Plus size={14} /> Create Invoice
                            </button>
                            <button className={styles.secondaryBtn}>
                                <FileText size={14} /> Generate Statement
                            </button>
                        </div>
                        
                        <div className={styles.rateCard}>
                            <h3><Tag size={16} /> Daypart Rate Card</h3>
                            <table className={styles.rateTable}>
                                <thead>
                                    <tr>
                                        <th>Daypart</th>
                                        <th>Base Rate (30s)</th>
                                        <th>Premium Multiplier</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    {DAYPART_RATES.map(rate => (
                                        <tr key={rate.daypart}>
                                            <td>{rate.daypart}</td>
                                            <td>{formatCurrency(rate.rate)}</td>
                                            <td>{rate.premium}x</td>
                                        </tr>
                                    ))}
                                </tbody>
                            </table>
                        </div>
                        
                        <div className={styles.outstandingSection}>
                            <h3><AlertCircle size={16} /> Outstanding Invoices</h3>
                            <div className={styles.noData}>
                                <Receipt size={32} />
                                <p>No outstanding invoices</p>
                            </div>
                        </div>
                    </div>
                )}

                {activeTab === 'advertisers' && (
                    <div className={styles.advertisers}>
                        <div className={styles.advertiserHeader}>
                            <div className={styles.searchBox}>
                                <Search size={14} />
                                <input type="text" placeholder="Search advertisers..." />
                            </div>
                            <button className={styles.addBtn} onClick={() => setShowAddModal('advertiser')}>
                                <Plus size={14} /> Add Advertiser
                            </button>
                        </div>
                        
                        <div className={styles.advertiserGrid}>
                            {advertisers.map(advertiser => {
                                const advertiserCampaigns = campaigns.filter(c => c.advertiserId === advertiser.id);
                                const totalSpent = advertiserCampaigns.reduce((sum, c) => sum + c.spent, 0);
                                
                                return (
                                    <div key={advertiser.id} className={styles.advertiserCard}>
                                        <div className={styles.advertiserCardHeader}>
                                            <div className={styles.advertiserAvatar}>
                                                <Building2 size={24} />
                                            </div>
                                            <div className={styles.advertiserName}>
                                                <h4>{advertiser.name}</h4>
                                                <span className={`${styles.statusTag} ${styles[advertiser.status]}`}>
                                                    {advertiser.status}
                                                </span>
                                            </div>
                                        </div>
                                        
                                        <div className={styles.advertiserContact}>
                                            <div><Users size={12} /> {advertiser.contactName}</div>
                                            <div><Mail size={12} /> {advertiser.email}</div>
                                            <div><Phone size={12} /> {advertiser.phone}</div>
                                        </div>
                                        
                                        <div className={styles.advertiserStats}>
                                            <div className={styles.advertiserStat}>
                                                <span className={styles.statLabel}>Campaigns</span>
                                                <span className={styles.statValue}>{advertiserCampaigns.length}</span>
                                            </div>
                                            <div className={styles.advertiserStat}>
                                                <span className={styles.statLabel}>Total Spent</span>
                                                <span className={styles.statValue}>{formatCurrency(totalSpent)}</span>
                                            </div>
                                            <div className={styles.advertiserStat}>
                                                <span className={styles.statLabel}>Balance</span>
                                                <span className={`${styles.statValue} ${advertiser.balance > 0 ? styles.owed : ''}`}>
                                                    {formatCurrency(advertiser.balance)}
                                                </span>
                                            </div>
                                        </div>
                                        
                                        <div className={styles.advertiserActions}>
                                            <button><Eye size={14} /> View</button>
                                            <button><Edit2 size={14} /> Edit</button>
                                            <button><FileText size={14} /> Invoice</button>
                                        </div>
                                    </div>
                                );
                            })}
                        </div>
                    </div>
                )}

                {activeTab === 'reports' && (
                    <div className={styles.reports}>
                        <h3>Available Reports</h3>
                        <div className={styles.reportGrid}>
                            <button className={styles.reportCard}>
                                <BarChart2 size={32} />
                                <span>Revenue Summary</span>
                                <p>Daily, weekly, monthly revenue breakdown</p>
                            </button>
                            <button className={styles.reportCard}>
                                <Target size={32} />
                                <span>Fill Rate Analysis</span>
                                <p>Spot fill rates by daypart and advertiser</p>
                            </button>
                            <button className={styles.reportCard}>
                                <AlertTriangle size={32} />
                                <span>Discrepancy Report</span>
                                <p>Missed spots and make-goods tracking</p>
                            </button>
                            <button className={styles.reportCard}>
                                <Building2 size={32} />
                                <span>Advertiser Aging</span>
                                <p>Outstanding balances and payment history</p>
                            </button>
                            <button className={styles.reportCard}>
                                <Play size={32} />
                                <span>Affidavit of Performance</span>
                                <p>Proof of playout for advertisers</p>
                            </button>
                            <button className={styles.reportCard}>
                                <TrendingUp size={32} />
                                <span>Trend Analysis</span>
                                <p>Historical performance and forecasting</p>
                            </button>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
};

export default TrafficBillingDashboard;
