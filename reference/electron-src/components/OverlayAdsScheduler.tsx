/**
 * OverlayAdsScheduler - Schedule overlay-only ads with CG fade filtering
 * 
 * Features:
 * - Schedule overlay-only ads (no full-screen takeover)
 * - Automatic CG fade filter when ad displays
 * - Integration with ticker (ad icon indicator)
 * - Integration with schedule editor (add overlay ad to slot)
 * - Ad preview before air
 * - Daypart targeting
 */

import React, { useState, useEffect, useCallback, useMemo } from 'react';
import styles from './OverlayAdsScheduler.module.css';
import {
    Calendar, Clock, Play, Pause, Square, Eye, EyeOff, Settings,
    Plus, Edit2, Trash2, Save, X, Check, AlertCircle, Filter,
    Layers, Image as ImageIcon, Video, Zap, ChevronDown, ChevronRight,
    Grid, List, Search, Tag, DollarSign, Target, BarChart2
} from 'lucide-react';
import { MediaItem, CGElement } from '../lib/types';
import { formatDuration } from '../lib/utils';

export interface OverlayAd {
    id: string;
    name: string;
    mediaId: string;
    media?: MediaItem;
    position: 'lower-third' | 'corner' | 'l-bar' | 'custom';
    customPosition?: {
        x: number; // percentage
        y: number; // percentage
        width: number; // percentage
        height: number; // percentage
    };
    scheduledTimes: ScheduledAdSlot[];
    fadeCGOnDisplay: boolean;
    fadeAmount: number; // 0-100, opacity reduction
    duration: number; // seconds
    opacity: number; // 0-100
    zIndex: number;
    clickable: boolean;
    clickUrl?: string;
    daypartTargeting?: DaypartTarget[];
    createdAt: string;
    updatedAt: string;
}

export interface ScheduledAdSlot {
    id: string;
    adId: string;
    startTime: Date;
    endTime: Date;
    dayOfWeek?: number; // 0-6, Sunday-Saturday
    recurring?: boolean;
    status: 'scheduled' | 'active' | 'completed' | 'missed';
}

export interface DaypartTarget {
    daypart: 'AM Drive' | 'Midday' | 'PM Drive' | 'Evening' | 'Late Night' | 'Overnight' | 'ROS';
    days: number[]; // 0-6
}

interface OverlayAdsSchedulerProps {
    mediaLibrary?: MediaItem[];
    cgElements?: CGElement[];
    onAdScheduled?: (ad: OverlayAd) => void;
    onAdTriggered?: (ad: OverlayAd) => void;
    onCGFadeRequest?: (fade: boolean, amount: number) => void;
    currentTime?: Date;
}

export const OverlayAdsScheduler: React.FC<OverlayAdsSchedulerProps> = ({
    mediaLibrary = [],
    cgElements = [],
    onAdScheduled,
    onAdTriggered,
    onCGFadeRequest,
    currentTime = new Date(),
}) => {
    const [ads, setAds] = useState<OverlayAd[]>([]);
    const [selectedAd, setSelectedAd] = useState<OverlayAd | null>(null);
    const [isEditing, setIsEditing] = useState(false);
    const [searchQuery, setSearchQuery] = useState('');
    const [viewMode, setViewMode] = useState<'list' | 'calendar'>('list');
    const [activeAds, setActiveAds] = useState<Set<string>>(new Set());

    // Load ads from storage
    useEffect(() => {
        const saved = localStorage.getItem('overlayAds');
        if (saved) {
            try {
                const parsed = JSON.parse(saved);
                setAds(parsed.map((ad: any) => ({
                    ...ad,
                    scheduledTimes: ad.scheduledTimes.map((slot: any) => ({
                        ...slot,
                        startTime: new Date(slot.startTime),
                        endTime: new Date(slot.endTime),
                    })),
                })));
            } catch (err) {
                console.error('Failed to load overlay ads:', err);
            }
        }
    }, []);

    // Save ads to storage
    useEffect(() => {
        if (ads.length > 0) {
            localStorage.setItem('overlayAds', JSON.stringify(ads));
        }
    }, [ads]);

    // Check for active ads and trigger CG fade
    useEffect(() => {
        const checkActiveAds = () => {
            const now = currentTime;
            const newActiveAds = new Set<string>();
            
            ads.forEach(ad => {
                const activeSlot = ad.scheduledTimes.find(slot => {
                    const start = new Date(slot.startTime);
                    const end = new Date(slot.endTime);
                    return now >= start && now <= end;
                });

                if (activeSlot) {
                    newActiveAds.add(ad.id);
                    if (ad.fadeCGOnDisplay && !activeAds.has(ad.id)) {
                        onCGFadeRequest?.(true, ad.fadeAmount);
                    }
                } else if (activeAds.has(ad.id)) {
                    // Ad just ended, restore CG
                    const adObj = ads.find(a => a.id === ad.id);
                    if (adObj?.fadeCGOnDisplay) {
                        onCGFadeRequest?.(false, 0);
                    }
                }
            });

            setActiveAds(newActiveAds);
            
            // Trigger callbacks for newly active ads
            newActiveAds.forEach(adId => {
                if (!activeAds.has(adId)) {
                    const ad = ads.find(a => a.id === adId);
                    if (ad) {
                        onAdTriggered?.(ad);
                    }
                }
            });
        };

        checkActiveAds();
        const interval = setInterval(checkActiveAds, 1000);
        return () => clearInterval(interval);
    }, [ads, currentTime, activeAds, onCGFadeRequest, onAdTriggered]);

    const handleCreateAd = useCallback(() => {
        const newAd: OverlayAd = {
            id: `ad_${Date.now()}`,
            name: 'New Overlay Ad',
            mediaId: '',
            position: 'lower-third',
            scheduledTimes: [],
            fadeCGOnDisplay: true,
            fadeAmount: 50,
            duration: 30,
            opacity: 100,
            zIndex: 1000,
            clickable: false,
            createdAt: new Date().toISOString(),
            updatedAt: new Date().toISOString(),
        };
        setAds(prev => [...prev, newAd]);
        setSelectedAd(newAd);
        setIsEditing(true);
    }, []);

    const handleSaveAd = useCallback((ad: OverlayAd) => {
        setAds(prev => prev.map(a => a.id === ad.id ? { ...ad, updatedAt: new Date().toISOString() } : a));
        setIsEditing(false);
        setSelectedAd(null);
        onAdScheduled?.(ad);
    }, [onAdScheduled]);

    const handleDeleteAd = useCallback((adId: string) => {
        setAds(prev => prev.filter(a => a.id !== adId));
        if (selectedAd?.id === adId) {
            setSelectedAd(null);
            setIsEditing(false);
        }
    }, [selectedAd]);

    const filteredAds = useMemo(() => {
        if (!searchQuery) return ads;
        const query = searchQuery.toLowerCase();
        return ads.filter(ad =>
            ad.name.toLowerCase().includes(query) ||
            ad.media?.title.toLowerCase().includes(query)
        );
    }, [ads, searchQuery]);

    const upcomingAds = useMemo(() => {
        const now = currentTime;
        return ads
            .flatMap(ad => ad.scheduledTimes.map(slot => ({ ad, slot })))
            .filter(({ slot }) => new Date(slot.startTime) > now)
            .sort((a, b) => new Date(a.slot.startTime).getTime() - new Date(b.slot.startTime).getTime())
            .slice(0, 10);
    }, [ads, currentTime]);

    return (
        <div className={styles.scheduler}>
            <div className={styles.header}>
                <div className={styles.titleSection}>
                    <Layers size={20} />
                    <h2>Overlay Ads Scheduler</h2>
                    {activeAds.size > 0 && (
                        <span className={styles.activeBadge}>
                            {activeAds.size} Active
                        </span>
                    )}
                </div>
                <div className={styles.controls}>
                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input
                            type="text"
                            placeholder="Search ads..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>
                    <button className={styles.viewToggle} onClick={() => setViewMode(viewMode === 'list' ? 'calendar' : 'list')}>
                        {viewMode === 'list' ? <Calendar size={16} /> : <List size={16} />}
                    </button>
                    <button className={styles.createButton} onClick={handleCreateAd}>
                        <Plus size={16} />
                        New Ad
                    </button>
                </div>
            </div>

            <div className={styles.content}>
                {viewMode === 'list' ? (
                    <div className={styles.listView}>
                        <div className={styles.adsList}>
                            {filteredAds.map(ad => {
                                const media = mediaLibrary.find(m => m.id === ad.mediaId);
                                const isActive = activeAds.has(ad.id);
                                const nextSlot = ad.scheduledTimes
                                    .filter(slot => new Date(slot.startTime) > currentTime)
                                    .sort((a, b) => new Date(a.startTime).getTime() - new Date(b.startTime).getTime())[0];

                                return (
                                    <div
                                        key={ad.id}
                                        className={`${styles.adCard} ${isActive ? styles.active : ''} ${selectedAd?.id === ad.id ? styles.selected : ''}`}
                                        onClick={() => {
                                            setSelectedAd(ad);
                                            setIsEditing(false);
                                        }}
                                    >
                                        <div className={styles.adHeader}>
                                            <div className={styles.adInfo}>
                                                {isActive && <div className={styles.activeIndicator} />}
                                                <h3>{ad.name}</h3>
                                                {media && (
                                                    <span className={styles.mediaName}>{media.title}</span>
                                                )}
                                            </div>
                                            <div className={styles.adActions}>
                                                <button
                                                    onClick={(e) => {
                                                        e.stopPropagation();
                                                        setSelectedAd(ad);
                                                        setIsEditing(true);
                                                    }}
                                                    title="Edit"
                                                >
                                                    <Edit2 size={14} />
                                                </button>
                                                <button
                                                    onClick={(e) => {
                                                        e.stopPropagation();
                                                        handleDeleteAd(ad.id);
                                                    }}
                                                    title="Delete"
                                                >
                                                    <Trash2 size={14} />
                                                </button>
                                            </div>
                                        </div>
                                        <div className={styles.adDetails}>
                                            <div className={styles.adMeta}>
                                                <span className={styles.metaItem}>
                                                    <Tag size={12} />
                                                    {ad.position}
                                                </span>
                                                <span className={styles.metaItem}>
                                                    <Clock size={12} />
                                                    {formatDuration(ad.duration)}
                                                </span>
                                                {ad.fadeCGOnDisplay && (
                                                    <span className={styles.metaItem}>
                                                        <Filter size={12} />
                                                        CG Fade: {ad.fadeAmount}%
                                                    </span>
                                                )}
                                                <span className={styles.metaItem}>
                                                    <Calendar size={12} />
                                                    {ad.scheduledTimes.length} slots
                                                </span>
                                            </div>
                                            {nextSlot && (
                                                <div className={styles.nextSlot}>
                                                    Next: {new Date(nextSlot.startTime).toLocaleString()}
                                                </div>
                                            )}
                                        </div>
                                    </div>
                                );
                            })}
                        </div>
                    </div>
                ) : (
                    <div className={styles.calendarView}>
                        <div className={styles.calendarGrid}>
                            {/* Calendar view implementation */}
                            <p>Calendar view coming soon</p>
                        </div>
                    </div>
                )}

                {/* Upcoming Ads Sidebar */}
                {upcomingAds.length > 0 && (
                    <div className={styles.sidebar}>
                        <h3>Upcoming</h3>
                        <div className={styles.upcomingList}>
                            {upcomingAds.map(({ ad, slot }) => (
                                <div key={`${ad.id}_${slot.id}`} className={styles.upcomingItem}>
                                    <div className={styles.upcomingTime}>
                                        {new Date(slot.startTime).toLocaleTimeString()}
                                    </div>
                                    <div className={styles.upcomingName}>{ad.name}</div>
                                    <div className={styles.upcomingDuration}>
                                        {formatDuration(ad.duration)}
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>
                )}
            </div>

            {/* Edit Modal */}
            {isEditing && selectedAd && (
                <AdEditorModal
                    ad={selectedAd}
                    mediaLibrary={mediaLibrary}
                    onSave={handleSaveAd}
                    onCancel={() => {
                        setIsEditing(false);
                        setSelectedAd(null);
                    }}
                />
            )}
        </div>
    );
};

// Ad Editor Modal Component
interface AdEditorModalProps {
    ad: OverlayAd;
    mediaLibrary: MediaItem[];
    onSave: (ad: OverlayAd) => void;
    onCancel: () => void;
}

const AdEditorModal: React.FC<AdEditorModalProps> = ({ ad, mediaLibrary, onSave, onCancel }) => {
    const [editedAd, setEditedAd] = useState<OverlayAd>(ad);

    const handleSave = () => {
        onSave(editedAd);
    };

    return (
        <div className={styles.modalOverlay} onClick={onCancel}>
            <div className={styles.modal} onClick={(e) => e.stopPropagation()}>
                <div className={styles.modalHeader}>
                    <h3>Edit Overlay Ad</h3>
                    <button onClick={onCancel}>
                        <X size={20} />
                    </button>
                </div>
                <div className={styles.modalContent}>
                    <div className={styles.formGroup}>
                        <label>Ad Name</label>
                        <input
                            type="text"
                            value={editedAd.name}
                            onChange={(e) => setEditedAd(prev => ({ ...prev, name: e.target.value }))}
                        />
                    </div>
                    <div className={styles.formGroup}>
                        <label>Media</label>
                        <select
                            value={editedAd.mediaId}
                            onChange={(e) => setEditedAd(prev => ({ ...prev, mediaId: e.target.value }))}
                        >
                            <option value="">Select media...</option>
                            {mediaLibrary.map(media => (
                                <option key={media.id} value={media.id}>
                                    {media.title} ({formatDuration(media.duration)})
                                </option>
                            ))}
                        </select>
                    </div>
                    <div className={styles.formGroup}>
                        <label>Position</label>
                        <select
                            value={editedAd.position}
                            onChange={(e) => setEditedAd(prev => ({ ...prev, position: e.target.value as any }))}
                        >
                            <option value="lower-third">Lower Third</option>
                            <option value="corner">Corner</option>
                            <option value="l-bar">L-Bar</option>
                            <option value="custom">Custom</option>
                        </select>
                    </div>
                    <div className={styles.formGroup}>
                        <label>
                            <input
                                type="checkbox"
                                checked={editedAd.fadeCGOnDisplay}
                                onChange={(e) => setEditedAd(prev => ({ ...prev, fadeCGOnDisplay: e.target.checked }))}
                            />
                            Fade CG on Display
                        </label>
                        {editedAd.fadeCGOnDisplay && (
                            <input
                                type="range"
                                min="0"
                                max="100"
                                value={editedAd.fadeAmount}
                                onChange={(e) => setEditedAd(prev => ({ ...prev, fadeAmount: parseInt(e.target.value, 10) }))}
                            />
                        )}
                    </div>
                    <div className={styles.formGroup}>
                        <label>Duration (seconds)</label>
                        <input
                            type="number"
                            min="1"
                            value={editedAd.duration}
                            onChange={(e) => setEditedAd(prev => ({ ...prev, duration: parseInt(e.target.value, 10) }))}
                        />
                    </div>
                    <div className={styles.formGroup}>
                        <label>Opacity (%)</label>
                        <input
                            type="range"
                            min="0"
                            max="100"
                            value={editedAd.opacity}
                            onChange={(e) => setEditedAd(prev => ({ ...prev, opacity: parseInt(e.target.value, 10) }))}
                        />
                    </div>
                </div>
                <div className={styles.modalFooter}>
                    <button onClick={onCancel}>Cancel</button>
                    <button className={styles.saveButton} onClick={handleSave}>
                        <Save size={16} />
                        Save
                    </button>
                </div>
            </div>
        </div>
    );
};
