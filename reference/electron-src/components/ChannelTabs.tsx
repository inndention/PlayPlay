// ============================================
// Channel Tabs Component
// Tab interface for switching between channels
// ============================================

import React from 'react';
import { PlayoutChannel } from '../lib/types';
import { Monitor, X, ExternalLink } from 'lucide-react';
import styles from './ChannelTabs.module.css';

interface ChannelTabsProps {
    channels: PlayoutChannel[];
    activeChannelId: string;
    onSelectChannel: (channelId: string) => void;
    onRemoveChannel?: (channelId: string) => void;
    onBreakoutChannel?: (channelId: string) => void;
    maxChannels?: number;
}

export function ChannelTabs({
    channels,
    activeChannelId,
    onSelectChannel,
    onRemoveChannel,
    onBreakoutChannel,
    maxChannels = 4
}: ChannelTabsProps) {
    return (
        <div className={styles.tabsContainer}>
            {channels.map((channel, index) => {
                const isActive = channel.id === activeChannelId;
                const channelNumber = index + 1;
                
                return (
                    <div
                        key={channel.id}
                        className={`${styles.tab} ${isActive ? styles.active : ''}`}
                        onClick={() => onSelectChannel(channel.id)}
                    >
                        <Monitor size={14} className={channel.isPlaying ? styles.playing : ''} />
                        <span className={styles.tabLabel}>CH{channelNumber}</span>
                        {channel.isPlaying && <span className={styles.liveDot} />}
                        {channels.length > 1 && onRemoveChannel && (
                            <button
                                className={styles.closeBtn}
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onRemoveChannel(channel.id);
                                }}
                                title="Remove Channel"
                            >
                                <X size={12} />
                            </button>
                        )}
                        {channelNumber > 1 && onBreakoutChannel && (
                            <button
                                className={styles.breakoutBtn}
                                onClick={(e) => {
                                    e.stopPropagation();
                                    onBreakoutChannel(channel.id);
                                }}
                                title="Open in Separate Window"
                            >
                                <ExternalLink size={12} />
                            </button>
                        )}
                    </div>
                );
            })}
        </div>
    );
}

