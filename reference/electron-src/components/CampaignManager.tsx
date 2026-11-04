import React, { useState, useEffect } from 'react';
import { Campaign, AdClient, MediaItem } from '../lib/types';
import { loadCampaigns, saveCampaigns, createCampaign, loadClients } from '../lib/storage';
import { Plus, Edit2, Trash2, X, Save, Calendar, Target, TrendingUp, Settings } from 'lucide-react';
import styles from './CampaignManager.module.css';

interface CampaignManagerProps {
    allMedia: MediaItem[];
    onMediaUpdate?: (media: MediaItem[]) => void;
    onClose?: () => void;
}

type Mode = 'simple' | 'advanced';

export function CampaignManager({ allMedia, onMediaUpdate, onClose }: CampaignManagerProps) {
    const [campaigns, setCampaigns] = useState<Campaign[]>(() => loadCampaigns());
    const [clients, setClients] = useState<AdClient[]>(() => loadClients());
    const [mode, setMode] = useState<Mode>('simple');
    const [editingCampaign, setEditingCampaign] = useState<Campaign | null>(null);
    const [showForm, setShowForm] = useState(false);
    const [formData, setFormData] = useState<Partial<Campaign>>({});

    useEffect(() => {
        saveCampaigns(campaigns);
    }, [campaigns]);

    const handleCreate = () => {
        setFormData({});
        setEditingCampaign(null);
        setShowForm(true);
    };

    const handleEdit = (campaign: Campaign) => {
        setFormData(campaign);
        setEditingCampaign(campaign);
        setShowForm(true);
    };

    const handleSave = () => {
        if (!formData.name || !formData.client) {
            alert('Name and client are required');
            return;
        }

        if (editingCampaign) {
            setCampaigns(campaigns.map(c => c.id === editingCampaign.id ? { ...formData, id: editingCampaign.id } as Campaign : c));
        } else {
            const newCampaign = createCampaign(formData.name!, formData.client!);
            setCampaigns([...campaigns, { ...newCampaign, ...formData }]);
        }

        setShowForm(false);
        setFormData({});
        setEditingCampaign(null);
    };

    const handleDelete = (id: string) => {
        if (confirm('Delete this campaign? Media assignments will be preserved.')) {
            setCampaigns(campaigns.filter(c => c.id !== id));
        }
    };

    const getCampaignMedia = (campaignId: string) => {
        return allMedia.filter(m => m.adMetadata?.campaignId === campaignId);
    };

    const formatDate = (dateStr: string) => {
        return new Date(dateStr).toLocaleDateString();
    };

    const getProgress = (campaign: Campaign) => {
        if (campaign.targetPlays === 0) return 0;
        return Math.min(100, (campaign.currentPlays / campaign.targetPlays) * 100);
    };

    return (
        <div className={styles.campaignManager}>
            <div className={styles.header}>
                <h2>Campaign Management</h2>
                <div className={styles.controls}>
                    <button
                        className={cn(styles.modeToggle, mode === 'simple' && styles.active)}
                        onClick={() => setMode('simple')}
                    >
                        Simple
                    </button>
                    <button
                        className={cn(styles.modeToggle, mode === 'advanced' && styles.active)}
                        onClick={() => setMode('advanced')}
                    >
                        Advanced
                    </button>
                    <button className={styles.createBtn} onClick={handleCreate}>
                        <Plus size={16} />
                        New Campaign
                    </button>
                    {onClose && (
                        <button className={styles.closeBtn} onClick={onClose} title="Close Campaign Manager">
                            <X size={16} />
                        </button>
                    )}
                </div>
            </div>

            {showForm && (
                <div className={styles.formOverlay}>
                    <div className={styles.form}>
                        <div className={styles.formHeader}>
                            <h3>{editingCampaign ? 'Edit Campaign' : 'New Campaign'}</h3>
                            <button className={styles.closeBtn} onClick={() => setShowForm(false)}>
                                <X size={20} />
                            </button>
                        </div>

                        <div className={styles.formFields}>
                            <div className={styles.field}>
                                <label>Campaign Name *</label>
                                <input
                                    type="text"
                                    value={formData.name || ''}
                                    onChange={(e) => setFormData({ ...formData, name: e.target.value })}
                                    placeholder="Summer Sale 2025"
                                />
                            </div>

                            <div className={styles.field}>
                                <label>Client *</label>
                                <select
                                    value={formData.client || ''}
                                    onChange={(e) => setFormData({ ...formData, client: e.target.value })}
                                >
                                    <option value="">Select client...</option>
                                    {clients.map(c => (
                                        <option key={c.id} value={c.id}>{c.name}</option>
                                    ))}
                                </select>
                            </div>

                            <div className={styles.row}>
                                <div className={styles.field}>
                                    <label>Start Date</label>
                                    <input
                                        type="date"
                                        value={formData.startDate ? formData.startDate.split('T')[0] : ''}
                                        onChange={(e) => setFormData({ ...formData, startDate: e.target.value + 'T00:00:00' })}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>End Date</label>
                                    <input
                                        type="date"
                                        value={formData.endDate ? formData.endDate.split('T')[0] : ''}
                                        onChange={(e) => setFormData({ ...formData, endDate: e.target.value + 'T00:00:00' })}
                                    />
                                </div>
                            </div>

                            <div className={styles.row}>
                                <div className={styles.field}>
                                    <label>Target Plays</label>
                                    <input
                                        type="number"
                                        value={formData.targetPlays || 0}
                                        onChange={(e) => setFormData({ ...formData, targetPlays: parseInt(e.target.value) || 0 })}
                                        min="0"
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Current Plays</label>
                                    <input
                                        type="number"
                                        value={formData.currentPlays || 0}
                                        onChange={(e) => setFormData({ ...formData, currentPlays: parseInt(e.target.value) || 0 })}
                                        min="0"
                                    />
                                </div>
                            </div>

                            {mode === 'advanced' && (
                                <>
                                    <div className={styles.field}>
                                        <label>Billing Code</label>
                                        <input
                                            type="text"
                                            value={formData.billingCode || ''}
                                            onChange={(e) => setFormData({ ...formData, billingCode: e.target.value })}
                                            placeholder="BILL-2025-001"
                                        />
                                    </div>

                                    <div className={styles.field}>
                                        <label>Daypart Restrictions</label>
                                        <input
                                            type="text"
                                            value={formData.daypartRestrictions?.join(', ') || ''}
                                            onChange={(e) => setFormData({
                                                ...formData,
                                                daypartRestrictions: e.target.value.split(',').map(s => s.trim()).filter(Boolean)
                                            })}
                                            placeholder="morning, primetime"
                                        />
                                    </div>

                                    <div className={styles.field}>
                                        <label>Separation (minutes)</label>
                                        <input
                                            type="number"
                                            value={formData.separationMinutes || ''}
                                            onChange={(e) => setFormData({ ...formData, separationMinutes: parseInt(e.target.value) || undefined })}
                                            min="0"
                                            placeholder="30"
                                        />
                                    </div>

                                    <div className={styles.field}>
                                        <label>Competitive Exclusions (Client IDs)</label>
                                        <input
                                            type="text"
                                            value={formData.competitiveExclusions?.join(', ') || ''}
                                            onChange={(e) => setFormData({
                                                ...formData,
                                                competitiveExclusions: e.target.value.split(',').map(s => s.trim()).filter(Boolean)
                                            })}
                                            placeholder="client-id-1, client-id-2"
                                        />
                                    </div>
                                </>
                            )}

                            <div className={styles.formActions}>
                                <button className={styles.cancelBtn} onClick={() => setShowForm(false)}>
                                    Cancel
                                </button>
                                <button className={styles.saveBtn} onClick={handleSave}>
                                    <Save size={16} />
                                    Save
                                </button>
                            </div>
                        </div>
                    </div>
                </div>
            )}

            <div className={styles.campaignList}>
                {campaigns.length === 0 ? (
                    <div className={styles.empty}>
                        <p>No campaigns yet. Create one to get started.</p>
                    </div>
                ) : (
                    campaigns.map(campaign => {
                        const media = getCampaignMedia(campaign.id);
                        const progress = getProgress(campaign);
                        const client = clients.find(c => c.id === campaign.client);

                        return (
                            <div key={campaign.id} className={styles.campaignCard}>
                                <div className={styles.campaignHeader}>
                                    <div>
                                        <h3>{campaign.name}</h3>
                                        <p className={styles.clientName} style={{ color: client?.color }}>
                                            {client?.name || 'Unknown Client'}
                                        </p>
                                    </div>
                                    <div className={styles.campaignActions}>
                                        <button onClick={() => handleEdit(campaign)} className={styles.iconBtn}>
                                            <Edit2 size={16} />
                                        </button>
                                        <button onClick={() => handleDelete(campaign.id)} className={styles.iconBtn}>
                                            <Trash2 size={16} />
                                        </button>
                                    </div>
                                </div>

                                <div className={styles.campaignDetails}>
                                    <div className={styles.detail}>
                                        <Calendar size={14} />
                                        <span>{formatDate(campaign.startDate)} - {formatDate(campaign.endDate)}</span>
                                    </div>
                                    <div className={styles.detail}>
                                        <Target size={14} />
                                        <span>{campaign.currentPlays} / {campaign.targetPlays || '∞'} plays</span>
                                    </div>
                                    <div className={styles.detail}>
                                        <TrendingUp size={14} />
                                        <span>{media.length} media items</span>
                                    </div>
                                </div>

                                {campaign.targetPlays > 0 && (
                                    <div className={styles.progressBar}>
                                        <div
                                            className={styles.progressFill}
                                            style={{ width: `${progress}%` }}
                                        />
                                        <span className={styles.progressText}>{Math.round(progress)}%</span>
                                    </div>
                                )}

                                {mode === 'advanced' && (
                                    <div className={styles.advancedInfo}>
                                        {campaign.billingCode && (
                                            <div className={styles.infoItem}>
                                                <strong>Billing:</strong> {campaign.billingCode}
                                            </div>
                                        )}
                                        {campaign.daypartRestrictions && campaign.daypartRestrictions.length > 0 && (
                                            <div className={styles.infoItem}>
                                                <strong>Dayparts:</strong> {campaign.daypartRestrictions.join(', ')}
                                            </div>
                                        )}
                                        {campaign.separationMinutes && (
                                            <div className={styles.infoItem}>
                                                <strong>Separation:</strong> {campaign.separationMinutes} min
                                            </div>
                                        )}
                                    </div>
                                )}
                            </div>
                        );
                    })
                )}
            </div>
        </div>
    );
}

function cn(...classes: (string | undefined | false)[]): string {
    return classes.filter(Boolean).join(' ');
}


