// ============================================
// Multi-Channel Manager
// Manages up to 4 independent playout channels
// Each channel has its own schedule and preview
// ============================================

import React, { useState, useCallback, useMemo, useEffect } from 'react';
import { PlayoutChannel, MultiChannelState, WeeklySchedule } from '../lib/types';
import { UnifiedScheduler } from './UnifiedScheduler';
import { loadWeeklySchedule, saveWeeklySchedule } from '../lib/storage';
import { 
    Plus, X, Monitor, Play, Pause, Settings, 
    Grid, Columns, Square, LayoutGrid, ChevronDown
} from 'lucide-react';
import styles from './MultiChannelManager.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

interface MultiChannelManagerProps {
    onChannelSelect?: (channelId: string) => void;
}

const DEFAULT_CHANNEL: Omit<PlayoutChannel, 'id' | 'scheduleId'> = {
    name: 'Channel',
    previewEnabled: true,
    previewPosition: 'main',
    isPlaying: false,
};

export function MultiChannelManager({ onChannelSelect }: MultiChannelManagerProps) {
    const [state, setState] = useState<MultiChannelState>({
        channels: [{
            id: 'ch-1',
            name: 'Channel 1',
            scheduleId: 'default',
            previewEnabled: true,
            previewPosition: 'main',
            isPlaying: false,
        }],
        activeChannelId: 'ch-1',
        layoutMode: '1x1',
        maxChannels: 4,
    });

    const [showAddMenu, setShowAddMenu] = useState(false);

    // Get active channel
    const activeChannel = useMemo(() => 
        state.channels.find(c => c.id === state.activeChannelId) || state.channels[0],
        [state.channels, state.activeChannelId]
    );

    // Add new channel
    const handleAddChannel = useCallback(() => {
        if (state.channels.length >= state.maxChannels) return;
        
        const newId = `ch-${generateId()}`;
        const newChannel: PlayoutChannel = {
            ...DEFAULT_CHANNEL,
            id: newId,
            name: `Channel ${state.channels.length + 1}`,
            scheduleId: `schedule-${newId}`,
        };

        setState(prev => ({
            ...prev,
            channels: [...prev.channels, newChannel],
            activeChannelId: newId,
            layoutMode: getLayoutMode(prev.channels.length + 1),
        }));
        setShowAddMenu(false);
    }, [state.channels.length, state.maxChannels]);

    // Remove channel
    const handleRemoveChannel = useCallback((channelId: string) => {
        if (state.channels.length <= 1) return;
        
        setState(prev => {
            const newChannels = prev.channels.filter(c => c.id !== channelId);
            const newActiveId = prev.activeChannelId === channelId 
                ? newChannels[0]?.id 
                : prev.activeChannelId;
            
            return {
                ...prev,
                channels: newChannels,
                activeChannelId: newActiveId,
                layoutMode: getLayoutMode(newChannels.length),
            };
        });
    }, [state.channels.length]);

    // Select channel
    const handleSelectChannel = useCallback((channelId: string) => {
        setState(prev => ({ ...prev, activeChannelId: channelId }));
        onChannelSelect?.(channelId);
    }, [onChannelSelect]);

    // Update channel
    const handleUpdateChannel = useCallback((channelId: string, updates: Partial<PlayoutChannel>) => {
        setState(prev => ({
            ...prev,
            channels: prev.channels.map(c => 
                c.id === channelId ? { ...c, ...updates } : c
            ),
        }));
    }, []);

    // Toggle play/pause
    const handleTogglePlay = useCallback((channelId: string) => {
        setState(prev => ({
            ...prev,
            channels: prev.channels.map(c => 
                c.id === channelId ? { ...c, isPlaying: !c.isPlaying } : c
            ),
        }));
    }, []);

    // Determine layout mode based on channel count
    const getLayoutMode = (count: number): '1x1' | '2x1' | '2x2' => {
        if (count <= 1) return '1x1';
        if (count === 2) return '2x1';
        return '2x2';
    };

    // Set layout mode
    const handleSetLayoutMode = useCallback((mode: '1x1' | '2x1' | '2x2') => {
        setState(prev => ({ ...prev, layoutMode: mode }));
    }, []);

    return (
        <div className={styles.container}>
            {/* Channel Tabs */}
            <div className={styles.tabBar}>
                <div className={styles.tabs}>
                    {state.channels.map(channel => (
                        <div
                            key={channel.id}
                            className={`${styles.tab} ${state.activeChannelId === channel.id ? styles.activeTab : ''}`}
                            onClick={() => handleSelectChannel(channel.id)}
                        >
                            <Monitor size={14} className={channel.isPlaying ? styles.playing : ''} />
                            <span className={styles.tabName}>{channel.name}</span>
                            {channel.isPlaying && <span className={styles.liveIndicator}>LIVE</span>}
                            {state.channels.length > 1 && (
                                <button
                                    className={styles.closeTabBtn}
                                    onClick={(e) => { e.stopPropagation(); handleRemoveChannel(channel.id); }}
                                >
                                    <X size={12} />
                                </button>
                            )}
                        </div>
                    ))}
                    
                    {state.channels.length < state.maxChannels && (
                        <button className={styles.addTabBtn} onClick={handleAddChannel}>
                            <Plus size={14} />
                            Add Channel
                        </button>
                    )}
                </div>

                <div className={styles.tabBarControls}>
                    {/* Layout mode selector */}
                    <div className={styles.layoutSelector}>
                        <button
                            className={`${styles.layoutBtn} ${state.layoutMode === '1x1' ? styles.active : ''}`}
                            onClick={() => handleSetLayoutMode('1x1')}
                            title="Single Preview"
                        >
                            <Square size={14} />
                        </button>
                        <button
                            className={`${styles.layoutBtn} ${state.layoutMode === '2x1' ? styles.active : ''}`}
                            onClick={() => handleSetLayoutMode('2x1')}
                            title="2 Previews Side-by-Side"
                            disabled={state.channels.length < 2}
                        >
                            <Columns size={14} />
                        </button>
                        <button
                            className={`${styles.layoutBtn} ${state.layoutMode === '2x2' ? styles.active : ''}`}
                            onClick={() => handleSetLayoutMode('2x2')}
                            title="4 Previews Grid"
                            disabled={state.channels.length < 3}
                        >
                            <LayoutGrid size={14} />
                        </button>
                    </div>
                </div>
            </div>

            {/* Main Content Area */}
            <div className={styles.content}>
                {/* Preview Grid */}
                <div className={`${styles.previewGrid} ${styles[`layout${state.layoutMode.replace('x', '')}`]}`}>
                    {state.channels.slice(0, state.layoutMode === '2x2' ? 4 : state.layoutMode === '2x1' ? 2 : 1).map(channel => (
                        <div 
                            key={channel.id}
                            className={`${styles.previewPanel} ${state.activeChannelId === channel.id ? styles.activePreview : ''}`}
                            onClick={() => handleSelectChannel(channel.id)}
                        >
                            <div className={styles.previewHeader}>
                                <span className={styles.previewTitle}>
                                    <Monitor size={12} />
                                    {channel.name}
                                </span>
                                <div className={styles.previewControls}>
                                    {channel.isPlaying ? (
                                        <span className={styles.liveTag}>LIVE</span>
                                    ) : (
                                        <span className={styles.stoppedTag}>STOPPED</span>
                                    )}
                                    <button
                                        className={styles.previewBtn}
                                        onClick={(e) => { e.stopPropagation(); handleTogglePlay(channel.id); }}
                                    >
                                        {channel.isPlaying ? <Pause size={14} /> : <Play size={14} />}
                                    </button>
                                </div>
                            </div>
                            <div className={styles.previewContent}>
                                {/* Placeholder for actual video preview */}
                                <div className={styles.previewPlaceholder}>
                                    {channel.currentItem ? (
                                        <>
                                            <span className={styles.nowPlaying}>Now Playing:</span>
                                            <span className={styles.currentTitle}>{channel.currentItem.media?.title || 'Unknown'}</span>
                                        </>
                                    ) : (
                                        <span>No content playing</span>
                                    )}
                                </div>
                            </div>
                        </div>
                    ))}
                </div>

                {/* Active Channel Schedule */}
                <div className={styles.schedulePanel}>
                    <div className={styles.channelHeader}>
                        <div className={styles.channelInfo}>
                            <input
                                type="text"
                                value={activeChannel.name}
                                onChange={(e) => handleUpdateChannel(activeChannel.id, { name: e.target.value })}
                                className={styles.channelNameInput}
                            />
                            <span className={styles.channelId}>ID: {activeChannel.scheduleId}</span>
                        </div>
                        <div className={styles.channelActions}>
                            <button
                                className={`${styles.playBtn} ${activeChannel.isPlaying ? styles.playing : ''}`}
                                onClick={() => handleTogglePlay(activeChannel.id)}
                            >
                                {activeChannel.isPlaying ? (
                                    <><Pause size={16} /> Stop</>
                                ) : (
                                    <><Play size={16} /> Start</>
                                )}
                            </button>
                            <button className={styles.settingsBtn}>
                                <Settings size={16} />
                            </button>
                        </div>
                    </div>
                    
                    {/* Scheduler for active channel */}
                    <div className={styles.schedulerWrapper}>
                        <UnifiedScheduler 
                            mode="schedule"
                            viewType="single"
                        />
                    </div>
                </div>
            </div>
        </div>
    );
}

