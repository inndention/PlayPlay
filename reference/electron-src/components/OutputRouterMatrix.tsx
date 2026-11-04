/**
 * OUTPUT ROUTER MATRIX
 * Professional broadcast signal routing matrix
 * Inspired by: Evertz EQX, Ross Video Ultrix, Grass Valley NV
 * 
 * Features:
 * - Visual crosspoint matrix
 * - Multi-destination routing
 * - Preset/salvo recall
 * - Lock protection
 * - Audio follow video
 * - Breakaway routing
 */

import React, { useState, useCallback, useMemo } from 'react';
import {
    ArrowRight, Lock, Unlock, Save, RotateCcw,
    Grid, List, Eye, EyeOff, Volume2, VolumeX,
    Monitor, Radio, Tv, Layers, Film, Mic,
    AlertTriangle, CheckCircle2, Settings, Zap
} from 'lucide-react';
import styles from './OutputRouterMatrix.module.css';

// Types
export interface RouterSource {
    id: string;
    name: string;
    shortName?: string;
    type: 'video' | 'audio' | 'av' | 'ndi' | 'sdi' | 'ip';
    status: 'active' | 'standby' | 'offline' | 'error';
    hasVideo?: boolean;
    hasAudio?: boolean;
    resolution?: string;
    format?: string;
}

export interface RouterDestination {
    id: string;
    name: string;
    shortName?: string;
    type: 'video' | 'audio' | 'av' | 'monitor' | 'stream' | 'record';
    currentSource?: string;
    audioSource?: string; // For breakaway
    isLocked?: boolean;
    status: 'active' | 'standby' | 'offline';
}

export interface RouterPreset {
    id: string;
    name: string;
    routes: Record<string, { video?: string; audio?: string }>;
    isLocked?: boolean;
}

interface OutputRouterMatrixProps {
    sources: RouterSource[];
    destinations: RouterDestination[];
    presets?: RouterPreset[];
    onRoute?: (destinationId: string, sourceId: string, type: 'video' | 'audio' | 'av') => void;
    onLockDestination?: (destinationId: string, locked: boolean) => void;
    onSavePreset?: (preset: RouterPreset) => void;
    onRecallPreset?: (presetId: string) => void;
    audioFollowVideo?: boolean;
    onAudioFollowVideoChange?: (enabled: boolean) => void;
}

// Icon helpers
const getSourceIcon = (type: string) => {
    switch (type) {
        case 'video': return <Film size={14} />;
        case 'audio': return <Mic size={14} />;
        case 'av': return <Tv size={14} />;
        case 'ndi': return <Radio size={14} />;
        case 'sdi': return <Monitor size={14} />;
        case 'ip': return <Layers size={14} />;
        default: return <Monitor size={14} />;
    }
};

const getDestIcon = (type: string) => {
    switch (type) {
        case 'video': return <Film size={14} />;
        case 'audio': return <Volume2 size={14} />;
        case 'av': return <Tv size={14} />;
        case 'monitor': return <Monitor size={14} />;
        case 'stream': return <Radio size={14} />;
        case 'record': return <Zap size={14} />;
        default: return <Monitor size={14} />;
    }
};

// Main Component
export const OutputRouterMatrix: React.FC<OutputRouterMatrixProps> = ({
    sources,
    destinations,
    presets = [],
    onRoute,
    onLockDestination,
    onSavePreset,
    onRecallPreset,
    audioFollowVideo = true,
    onAudioFollowVideoChange,
}) => {
    const [selectedSource, setSelectedSource] = useState<string | null>(null);
    const [selectedDestination, setSelectedDestination] = useState<string | null>(null);
    const [viewMode, setViewMode] = useState<'matrix' | 'list'>('matrix');
    const [showAudioBreakaway, setShowAudioBreakaway] = useState(false);

    // Handle crosspoint click
    const handleCrosspointClick = useCallback((destId: string, srcId: string) => {
        const dest = destinations.find(d => d.id === destId);
        if (dest?.isLocked) return;
        
        onRoute?.(destId, srcId, audioFollowVideo ? 'av' : 'video');
    }, [destinations, onRoute, audioFollowVideo]);

    // Handle take button
    const handleTake = useCallback(() => {
        if (selectedSource && selectedDestination) {
            const dest = destinations.find(d => d.id === selectedDestination);
            if (dest?.isLocked) return;
            
            onRoute?.(selectedDestination, selectedSource, audioFollowVideo ? 'av' : 'video');
        }
    }, [selectedSource, selectedDestination, destinations, onRoute, audioFollowVideo]);

    // Check if crosspoint is active
    const isCrosspointActive = useCallback((destId: string, srcId: string) => {
        const dest = destinations.find(d => d.id === destId);
        return dest?.currentSource === srcId;
    }, [destinations]);

    return (
        <div className={styles.container}>
            {/* Header */}
            <header className={styles.header}>
                <div className={styles.headerLeft}>
                    <h2 className={styles.title}>
                        <Grid size={20} />
                        Signal Router
                    </h2>
                    <span className={styles.stats}>
                        {sources.length} × {destinations.length}
                    </span>
                </div>

                <div className={styles.headerCenter}>
                    {/* Quick Take */}
                    <div className={styles.quickTake}>
                        <div className={styles.takeSelection}>
                            <select 
                                className={styles.takeSelect}
                                value={selectedSource || ''}
                                onChange={(e) => setSelectedSource(e.target.value)}
                            >
                                <option value="">Source...</option>
                                {sources.map(src => (
                                    <option key={src.id} value={src.id}>{src.name}</option>
                                ))}
                            </select>
                            <ArrowRight size={16} />
                            <select 
                                className={styles.takeSelect}
                                value={selectedDestination || ''}
                                onChange={(e) => setSelectedDestination(e.target.value)}
                            >
                                <option value="">Destination...</option>
                                {destinations.map(dest => (
                                    <option key={dest.id} value={dest.id} disabled={dest.isLocked}>
                                        {dest.name} {dest.isLocked ? '🔒' : ''}
                                    </option>
                                ))}
                            </select>
                        </div>
                        <button 
                            className={styles.takeBtn}
                            onClick={handleTake}
                            disabled={!selectedSource || !selectedDestination}
                        >
                            <Zap size={16} />
                            TAKE
                        </button>
                    </div>
                </div>

                <div className={styles.headerRight}>
                    {/* Options */}
                    <button 
                        className={`${styles.optionBtn} ${audioFollowVideo ? styles.active : ''}`}
                        onClick={() => onAudioFollowVideoChange?.(!audioFollowVideo)}
                        title="Audio Follow Video"
                    >
                        <Volume2 size={14} />
                        AFV
                    </button>
                    <button 
                        className={`${styles.optionBtn} ${showAudioBreakaway ? styles.active : ''}`}
                        onClick={() => setShowAudioBreakaway(!showAudioBreakaway)}
                        title="Show Audio Breakaway"
                    >
                        <Mic size={14} />
                    </button>
                    <div className={styles.divider} />
                    <button 
                        className={`${styles.viewBtn} ${viewMode === 'matrix' ? styles.active : ''}`}
                        onClick={() => setViewMode('matrix')}
                    >
                        <Grid size={14} />
                    </button>
                    <button 
                        className={`${styles.viewBtn} ${viewMode === 'list' ? styles.active : ''}`}
                        onClick={() => setViewMode('list')}
                    >
                        <List size={14} />
                    </button>
                </div>
            </header>

            {/* Presets Bar */}
            {presets.length > 0 && (
                <div className={styles.presetsBar}>
                    <span className={styles.presetsLabel}>Presets:</span>
                    {presets.map(preset => (
                        <button 
                            key={preset.id}
                            className={styles.presetBtn}
                            onClick={() => onRecallPreset?.(preset.id)}
                            disabled={preset.isLocked}
                        >
                            {preset.name}
                        </button>
                    ))}
                    <button className={styles.savePresetBtn}>
                        <Save size={12} />
                        Save
                    </button>
                </div>
            )}

            {/* Matrix View */}
            {viewMode === 'matrix' && (
                <div className={styles.matrixContainer}>
                    <div className={styles.matrix}>
                        {/* Corner Cell */}
                        <div className={styles.cornerCell}>
                            <span>SRC / DEST</span>
                        </div>

                        {/* Destination Headers */}
                        {destinations.map(dest => (
                            <div 
                                key={dest.id} 
                                className={`${styles.destHeader} ${styles[`status${dest.status}`]}`}
                            >
                                <div className={styles.destIcon}>
                                    {getDestIcon(dest.type)}
                                </div>
                                <span className={styles.destName}>{dest.shortName || dest.name}</span>
                                {dest.isLocked && <Lock size={10} className={styles.lockIcon} />}
                            </div>
                        ))}

                        {/* Source Rows */}
                        {sources.map(src => (
                            <React.Fragment key={src.id}>
                                {/* Source Header */}
                                <div 
                                    className={`${styles.srcHeader} ${styles[`status${src.status}`]}`}
                                >
                                    <div className={styles.srcIcon}>
                                        {getSourceIcon(src.type)}
                                    </div>
                                    <span className={styles.srcName}>{src.shortName || src.name}</span>
                                    <span className={`${styles.srcStatus} ${styles[src.status]}`} />
                                </div>

                                {/* Crosspoints */}
                                {destinations.map(dest => {
                                    const isActive = isCrosspointActive(dest.id, src.id);
                                    const isLocked = dest.isLocked;

                                    return (
                                        <button
                                            key={`${src.id}-${dest.id}`}
                                            className={`${styles.crosspoint} ${isActive ? styles.active : ''} ${isLocked ? styles.locked : ''}`}
                                            onClick={() => handleCrosspointClick(dest.id, src.id)}
                                            disabled={isLocked}
                                        >
                                            {isActive && <CheckCircle2 size={16} />}
                                        </button>
                                    );
                                })}
                            </React.Fragment>
                        ))}
                    </div>
                </div>
            )}

            {/* List View */}
            {viewMode === 'list' && (
                <div className={styles.listContainer}>
                    {destinations.map(dest => {
                        const currentSrc = sources.find(s => s.id === dest.currentSource);
                        
                        return (
                            <div key={dest.id} className={styles.listRow}>
                                <div className={styles.listDest}>
                                    {getDestIcon(dest.type)}
                                    <span>{dest.name}</span>
                                    {dest.isLocked && (
                                        <button 
                                            className={styles.lockBtn}
                                            onClick={() => onLockDestination?.(dest.id, false)}
                                        >
                                            <Lock size={12} />
                                        </button>
                                    )}
                                    {!dest.isLocked && (
                                        <button 
                                            className={styles.unlockBtn}
                                            onClick={() => onLockDestination?.(dest.id, true)}
                                        >
                                            <Unlock size={12} />
                                        </button>
                                    )}
                                </div>
                                <ArrowRight size={16} className={styles.listArrow} />
                                <div className={styles.listSource}>
                                    {currentSrc ? (
                                        <>
                                            {getSourceIcon(currentSrc.type)}
                                            <span>{currentSrc.name}</span>
                                        </>
                                    ) : (
                                        <span className={styles.noSource}>No Source</span>
                                    )}
                                </div>
                                <select 
                                    className={styles.listSelect}
                                    value={dest.currentSource || ''}
                                    onChange={(e) => onRoute?.(dest.id, e.target.value, 'av')}
                                    disabled={dest.isLocked}
                                >
                                    <option value="">None</option>
                                    {sources.map(src => (
                                        <option key={src.id} value={src.id}>{src.name}</option>
                                    ))}
                                </select>
                            </div>
                        );
                    })}
                </div>
            )}

            {/* Status Bar */}
            <footer className={styles.statusBar}>
                <div className={styles.statusLeft}>
                    <span className={styles.statusItem}>
                        <Monitor size={12} />
                        {sources.filter(s => s.status === 'active').length} active sources
                    </span>
                    <span className={styles.statusItem}>
                        <Lock size={12} />
                        {destinations.filter(d => d.isLocked).length} locked
                    </span>
                </div>
                <div className={styles.statusRight}>
                    <span className={`${styles.afvStatus} ${audioFollowVideo ? styles.on : styles.off}`}>
                        AFV {audioFollowVideo ? 'ON' : 'OFF'}
                    </span>
                </div>
            </footer>
        </div>
    );
};

export default OutputRouterMatrix;
