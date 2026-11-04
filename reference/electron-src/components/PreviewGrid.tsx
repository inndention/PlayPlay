// ============================================
// Preview Grid Component
// Displays multiple channel previews in a grid layout
// ============================================

import React from 'react';
import { PlayoutChannel, PlaylistItem } from '../lib/types';
import { formatDuration } from '../lib/utils';
import { Play, Pause, Monitor } from 'lucide-react';
import styles from './PreviewGrid.module.css';

interface PreviewGridProps {
    channels: PlayoutChannel[];
    activeChannelId: string;
    layoutMode: '1x1' | '2x1' | '2x2';
    onChannelClick?: (channelId: string) => void;
    onTogglePlay?: (channelId: string) => void;
}

export function PreviewGrid({ 
    channels, 
    activeChannelId, 
    layoutMode,
    onChannelClick,
    onTogglePlay 
}: PreviewGridProps) {
    // Determine which channels to show based on layout
    const channelsToShow = channels.slice(0, layoutMode === '2x2' ? 4 : layoutMode === '2x1' ? 2 : 1);

    return (
        <div className={`${styles.grid} ${styles[`layout${layoutMode.replace('x', '')}`]}`}>
            {channelsToShow.map(channel => {
                const isActive = channel.id === activeChannelId;
                const currentItem = channel.currentItem;
                const nextItem = channel.nextItem;
                
                return (
                    <div
                        key={channel.id}
                        className={`${styles.previewCell} ${isActive ? styles.active : ''}`}
                        onClick={() => onChannelClick?.(channel.id)}
                    >
                        <div className={styles.previewHeader}>
                            <div className={styles.channelLabel}>
                                <Monitor size={12} />
                                <span>{channel.name}</span>
                            </div>
                            <div className={styles.previewControls}>
                                {channel.isPlaying ? (
                                    <span className={styles.liveBadge}>LIVE</span>
                                ) : (
                                    <span className={styles.stoppedBadge}>STOPPED</span>
                                )}
                                <button
                                    className={styles.playButton}
                                    onClick={(e) => {
                                        e.stopPropagation();
                                        onTogglePlay?.(channel.id);
                                    }}
                                    title={channel.isPlaying ? 'Pause' : 'Play'}
                                >
                                    {channel.isPlaying ? <Pause size={12} /> : <Play size={12} />}
                                </button>
                            </div>
                        </div>
                        
                        <div className={styles.previewContent}>
                            {currentItem ? (
                                <div className={styles.nowPlaying}>
                                    <div className={styles.currentTitle}>
                                        {currentItem.media?.title || 'Unknown'}
                                    </div>
                                    <div className={styles.currentMeta}>
                                        <span>{formatDuration(currentItem.media?.duration || 0)}</span>
                                        <span>•</span>
                                        <span>{currentItem.media?.type || 'unknown'}</span>
                                    </div>
                                    {nextItem && (
                                        <div className={styles.nextUp}>
                                            <span className={styles.nextLabel}>Next:</span>
                                            <span className={styles.nextTitle}>{nextItem.media?.title || 'Unknown'}</span>
                                        </div>
                                    )}
                                </div>
                            ) : (
                                <div className={styles.emptyState}>
                                    <Monitor size={32} />
                                    <span>No content playing</span>
                                </div>
                            )}
                        </div>
                    </div>
                );
            })}
        </div>
    );
}

