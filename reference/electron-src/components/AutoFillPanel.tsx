import React, { useState, useEffect } from 'react';
import { MediaItem, PlaylistItem, MediaType, HourSlot, BumperConfig } from '../lib/types';
import { loadLibraryMedia, loadLocalMedia, loadMediaStats } from '../lib/storage';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { buildDynamicFillerIdeas, rankMediaForSmartFill } from '../lib/aiHelper';
import { 
    Wand2, Radio, Play, Film, Megaphone, Sparkles, 
    Clock, ArrowRight, Check, AlertTriangle
} from 'lucide-react';
import styles from './AutoFillPanel.module.css';

interface AutoFillPanelProps {
    slot: HourSlot;
    onUpdateSlot: (slot: HourSlot) => void;
}

interface AutoFillResult {
    type: 'ad' | 'id' | 'bumper' | 'gap';
    position: number;
    mediaId?: string;
    media?: MediaItem;
    duration: number;
}

const generateId = () => Math.random().toString(36).substr(2, 9);

export function AutoFillPanel({ slot, onUpdateSlot }: AutoFillPanelProps) {
    const [mediaLibrary, setMediaLibrary] = useState<MediaItem[]>([]);
    const [idsAvailable, setIdsAvailable] = useState<MediaItem[]>([]);
    const [bumpersAvailable, setBumpersAvailable] = useState<MediaItem[]>([]);
    const [adsAvailable, setAdsAvailable] = useState<MediaItem[]>([]);
    const [promosAvailable, setPromosAvailable] = useState<MediaItem[]>([]);
    const [previewResults, setPreviewResults] = useState<AutoFillResult[]>([]);
    const [showPreview, setShowPreview] = useState(false);

    // Settings
    const [addTopOfHourId, setAddTopOfHourId] = useState(true);
    const [addBottomOfHourId, setAddBottomOfHourId] = useState(true);
    const [addBumpersBefore, setAddBumpersBefore] = useState(true);
    const [fillAdsInBreaks, setFillAdsInBreaks] = useState(true);
    const [targetAdMinutes, setTargetAdMinutes] = useState(12); // Minutes of ads per hour
    const [maxAdBreakLength, setMaxAdBreakLength] = useState(180); // 3 minutes max per break

    // Load media library
    useEffect(() => {
        (async () => {
            const [dbMedia, localMedia, storedLibrary] = await Promise.all([
                dbFetchMedia().catch(() => []),
                Promise.resolve(loadLocalMedia()),
                Promise.resolve(loadLibraryMedia([])),
            ]);

            const allMedia = [...storedLibrary, ...localMedia, ...dbMedia];
            // Deduplicate by ID
            const uniqueMedia = Array.from(new Map(allMedia.map(m => [m.id, m])).values());
            
            setMediaLibrary(uniqueMedia);
            setIdsAvailable(uniqueMedia.filter(m => m.type === 'station_id'));
            setBumpersAvailable(uniqueMedia.filter(m => m.type === 'bumper'));
            setAdsAvailable(uniqueMedia.filter(m => m.type === 'ad'));
            setPromosAvailable(uniqueMedia.filter(m => m.type === 'promo'));
        })();
    }, []);

    // Get program segments in the slot
    const getProgramSegments = (): { item: PlaylistItem; index: number }[] => {
        return slot.items
            .map((item, index) => ({ item, index }))
            .filter(({ item }) => item.media.type === 'program' || item.media.type === 'paid_program');
    };

    // Calculate current slot duration
    const getCurrentDuration = (): number => {
        return slot.items.reduce((sum, item) => sum + item.media.duration, 0);
    };

    // Pick from a list while avoiding paused, expired, and recently overplayed items.
    const getSmartMedia = (list: MediaItem[], preferredSeries?: string): MediaItem | null => {
        if (list.length === 0) return null;
        
        // For bumpers, try to find one matching the program series
        const stats = loadMediaStats();
        if (preferredSeries) {
            const matchingBumpers = list.filter(m => {
                const bumperConfig = (m as any).bumperConfig as BumperConfig | undefined;
                return bumperConfig?.programSeries === preferredSeries || bumperConfig?.isGeneric;
            });
            if (matchingBumpers.length > 0) {
                return rankMediaForSmartFill(matchingBumpers, stats, { preferredSeries })[0] || null;
            }
        }
        
        return rankMediaForSmartFill(list, stats)[0] || null;
    };

    // Generate auto-fill preview
    const generatePreview = () => {
        const results: AutoFillResult[] = [];
        const programs = getProgramSegments();
        let insertOffset = 0;

        // 1. Add top-of-hour ID
        if (addTopOfHourId && idsAvailable.length > 0) {
            const id = getSmartMedia(idsAvailable);
            if (id) {
                results.push({
                    type: 'id',
                    position: 0,
                    mediaId: id.id,
                    media: id,
                    duration: id.duration,
                });
                insertOffset++;
            }
        }

        // 2. Add bumpers before program segments
        if (addBumpersBefore && bumpersAvailable.length > 0) {
            programs.forEach(({ item, index }) => {
                const bumper = getSmartMedia(bumpersAvailable, item.media.programSeries);
                if (bumper) {
                    results.push({
                        type: 'bumper',
                        position: index + insertOffset,
                        mediaId: bumper.id,
                        media: bumper,
                        duration: bumper.duration,
                    });
                    insertOffset++;
                }
            });
        }

        // 3. Fill ads between programs
        if (fillAdsInBreaks && adsAvailable.length > 0 && programs.length > 1) {
            const totalAdSeconds = targetAdMinutes * 60;
            const breakCount = programs.length - 1;
            const adsPerBreak = Math.floor(totalAdSeconds / breakCount / 30); // Assume 30s avg ad

            programs.slice(0, -1).forEach(({ index }, i) => {
                let breakDuration = 0;
                const breakAds: AutoFillResult[] = [];

                while (breakDuration < maxAdBreakLength && breakAds.length < adsPerBreak) {
                    const ad = getSmartMedia(adsAvailable);
                    if (!ad) break;
                    if (breakDuration + ad.duration > maxAdBreakLength) break;

                    breakAds.push({
                        type: 'ad',
                        position: index + insertOffset + 1,
                        mediaId: ad.id,
                        media: ad,
                        duration: ad.duration,
                    });
                    breakDuration += ad.duration;
                }

                results.push(...breakAds);
                insertOffset += breakAds.length;
            });
        }

        // 4. Add bottom-of-hour ID (at the end)
        if (addBottomOfHourId && idsAvailable.length > 0) {
            const id = getSmartMedia(idsAvailable);
            if (id) {
                results.push({
                    type: 'id',
                    position: slot.items.length + insertOffset,
                    mediaId: id.id,
                    media: id,
                    duration: id.duration,
                });
            }
        }

        setPreviewResults(results);
        setShowPreview(true);
    };

    // Apply the auto-fill
    const applyAutoFill = () => {
        const newItems = [...slot.items];
        
        // Sort results by position (descending to insert from end to start)
        const sortedResults = [...previewResults].sort((a, b) => b.position - a.position);
        
        sortedResults.forEach(result => {
            if (!result.media) return;
            
            const newItem: PlaylistItem = {
                id: generateId(),
                mediaId: result.media.id,
                media: result.media,
                isResolved: true,
            };
            
            // Insert at position
            newItems.splice(result.position, 0, newItem);
        });
        
        onUpdateSlot({
            ...slot,
            items: newItems,
        });
        
        setShowPreview(false);
        setPreviewResults([]);
    };

    // Quick actions
    const addTopId = () => {
        if (idsAvailable.length === 0) return;
        const id = getSmartMedia(idsAvailable);
        if (!id) return;
        
        const newItem: PlaylistItem = {
            id: generateId(),
            mediaId: id.id,
            media: id,
            isResolved: true,
        };
        
        onUpdateSlot({
            ...slot,
            items: [newItem, ...slot.items],
        });
    };

    const addBottomId = () => {
        if (idsAvailable.length === 0) return;
        const id = getSmartMedia(idsAvailable);
        if (!id) return;
        
        const newItem: PlaylistItem = {
            id: generateId(),
            mediaId: id.id,
            media: id,
            isResolved: true,
        };
        
        onUpdateSlot({
            ...slot,
            items: [...slot.items, newItem],
        });
    };

    const addBumperForProgram = (programItem: PlaylistItem, insertBefore: boolean) => {
        if (bumpersAvailable.length === 0) return;
        const bumper = getSmartMedia(bumpersAvailable, programItem.media.programSeries);
        if (!bumper) return;
        
        const newItem: PlaylistItem = {
            id: generateId(),
            mediaId: bumper.id,
            media: bumper,
            isResolved: true,
        };
        
        const index = slot.items.findIndex(i => i.id === programItem.id);
        if (index === -1) return;
        
        const newItems = [...slot.items];
        newItems.splice(insertBefore ? index : index + 1, 0, newItem);
        
        onUpdateSlot({
            ...slot,
            items: newItems,
        });
    };

    const programs = getProgramSegments();
    const currentDuration = getCurrentDuration();
    const overHour = currentDuration > 3600;
    const fillerIdeas = buildDynamicFillerIdeas({
        upcomingTitles: programs.map(({ item }) => item.media.title),
    });

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Wand2 size={16} />
                <span>Auto-Fill</span>
            </div>

            {/* Quick Actions */}
            <div className={styles.quickActions}>
                <button 
                    className={styles.quickBtn}
                    onClick={addTopId}
                    disabled={idsAvailable.length === 0}
                    title="Add station ID at top of hour"
                >
                    <Radio size={14} />
                    Add ID (Top)
                </button>
                <button 
                    className={styles.quickBtn}
                    onClick={addBottomId}
                    disabled={idsAvailable.length === 0}
                    title="Add station ID at bottom of hour"
                >
                    <Radio size={14} />
                    Add ID (Bottom)
                </button>
            </div>

            {/* Program-Specific Bumpers */}
            {programs.length > 0 && bumpersAvailable.length > 0 && (
                <div className={styles.section}>
                    <div className={styles.sectionTitle}>
                        <Play size={14} />
                        Add Bumpers
                    </div>
                    <div className={styles.bumperList}>
                        {programs.map(({ item }) => (
                            <div key={item.id} className={styles.bumperItem}>
                                <span className={styles.programName}>{item.media.title}</span>
                                <button
                                    className={styles.smallBtn}
                                    onClick={() => addBumperForProgram(item, true)}
                                    title="Add bumper before"
                                >
                                    ← Before
                                </button>
                                <button
                                    className={styles.smallBtn}
                                    onClick={() => addBumperForProgram(item, false)}
                                    title="Add bumper after"
                                >
                                    After →
                                </button>
                            </div>
                        ))}
                    </div>
                </div>
            )}

            {/* Auto-Fill Settings */}
            <div className={styles.section}>
                <div className={styles.sectionTitle}>
                    <Sparkles size={14} />
                    Smart Fill Settings
                </div>
                
                <label className={styles.checkbox}>
                    <input
                        type="checkbox"
                        checked={addTopOfHourId}
                        onChange={e => setAddTopOfHourId(e.target.checked)}
                    />
                    <Radio size={12} />
                    <span>ID at top of hour</span>
                </label>
                
                <label className={styles.checkbox}>
                    <input
                        type="checkbox"
                        checked={addBottomOfHourId}
                        onChange={e => setAddBottomOfHourId(e.target.checked)}
                    />
                    <Radio size={12} />
                    <span>ID at bottom of hour</span>
                </label>
                
                <label className={styles.checkbox}>
                    <input
                        type="checkbox"
                        checked={addBumpersBefore}
                        onChange={e => setAddBumpersBefore(e.target.checked)}
                    />
                    <Play size={12} />
                    <span>Bumpers before programs</span>
                </label>
                
                <label className={styles.checkbox}>
                    <input
                        type="checkbox"
                        checked={fillAdsInBreaks}
                        onChange={e => setFillAdsInBreaks(e.target.checked)}
                    />
                    <Megaphone size={12} />
                    <span>Fill ads in breaks</span>
                </label>

                {fillAdsInBreaks && (
                    <div className={styles.adSettings}>
                        <label className={styles.inputLabel}>
                            <span>Target ad minutes/hour:</span>
                            <input
                                type="number"
                                min={0}
                                max={30}
                                value={targetAdMinutes}
                                onChange={e => setTargetAdMinutes(parseInt(e.target.value) || 0)}
                                className={styles.numberInput}
                            />
                        </label>
                        <label className={styles.inputLabel}>
                            <span>Max break length (sec):</span>
                            <input
                                type="number"
                                min={30}
                                max={600}
                                value={maxAdBreakLength}
                                onChange={e => setMaxAdBreakLength(parseInt(e.target.value) || 180)}
                                className={styles.numberInput}
                            />
                        </label>
                    </div>
                )}
            </div>

            {/* Generate Button */}
            <button className={styles.generateBtn} onClick={generatePreview}>
                <Wand2 size={16} />
                Preview Auto-Fill
            </button>

            <div className={styles.section}>
                <div className={styles.sectionTitle}>
                    <Sparkles size={14} />
                    Dynamic Fillers
                </div>
                <div className={styles.fillerList}>
                    {fillerIdeas.map(idea => (
                        <div key={idea.id} className={styles.fillerIdea}>
                            <span>{idea.title}</span>
                            <small>{idea.duration}s · {idea.loopable ? 'loopable' : 'one-shot'} · {idea.source}</small>
                        </div>
                    ))}
                </div>
            </div>

            {/* Preview Results */}
            {showPreview && previewResults.length > 0 && (
                <div className={styles.preview}>
                    <div className={styles.previewHeader}>
                        <span>Preview ({previewResults.length} items to add)</span>
                        <button onClick={() => setShowPreview(false)}>×</button>
                    </div>
                    <div className={styles.previewList}>
                        {previewResults.map((result, i) => (
                            <div key={i} className={styles.previewItem}>
                                <span className={`${styles.previewType} ${styles[result.type]}`}>
                                    {result.type.toUpperCase()}
                                </span>
                                <span className={styles.previewName}>
                                    {result.media?.title || 'Unknown'}
                                </span>
                                <span className={styles.previewDuration}>
                                    {Math.floor(result.duration / 60)}:{Math.round(result.duration % 60).toString().padStart(2, '0')}
                                </span>
                            </div>
                        ))}
                    </div>
                    <button className={styles.applyBtn} onClick={applyAutoFill}>
                        <Check size={16} />
                        Apply Auto-Fill
                    </button>
                </div>
            )}

            {/* Status */}
            <div className={styles.status}>
                <div className={styles.statusItem}>
                    <Clock size={12} />
                    <span>Duration: {Math.floor(currentDuration / 60)}:{Math.round(currentDuration % 60).toString().padStart(2, '0')}</span>
                    {overHour && <AlertTriangle size={12} className={styles.warning} />}
                </div>
                <div className={styles.statusItem}>
                    <Film size={12} />
                    <span>{programs.length} program{programs.length !== 1 ? 's' : ''}</span>
                </div>
            </div>

            {/* Available Media Stats */}
            <div className={styles.stats}>
                <span title="Station IDs available">
                    <Radio size={12} /> {idsAvailable.length} IDs
                </span>
                <span title="Bumpers available">
                    <Play size={12} /> {bumpersAvailable.length} Bumpers
                </span>
                <span title="Ads available">
                    <Megaphone size={12} /> {adsAvailable.length} Ads
                </span>
            </div>
        </div>
    );
}
