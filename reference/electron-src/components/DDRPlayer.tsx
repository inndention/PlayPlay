import React, { useState, useRef, useEffect, useCallback } from 'react';
import { DDRPlaylist, DDRChannel, PlaylistItem, MediaItem } from '../lib/types';
import { formatDuration } from '../lib/utils';
import {
    Play, Pause, Square, SkipForward, SkipBack, Volume2, VolumeX,
    Repeat, Shuffle, Maximize2, Minimize2, Plus, X, Film, Layers
} from 'lucide-react';
import styles from './DDRPlayer.module.css';

const CHANNEL_COLORS: Record<DDRChannel, string> = {
    A: '#ef4444',
    B: '#22c55e',
    C: '#3b82f6',
    D: '#a855f7',
};

interface DDRPlayerProps {
    channel: DDRChannel;
    playlist: DDRPlaylist;
    onPlaylistChange: (playlist: DDRPlaylist) => void;
    onPiPChange: (enabled: boolean, settings: DDRPlaylist['pipPosition']) => void;
    isMainPlayer?: boolean;
}

export function DDRPlayer({ channel, playlist, onPlaylistChange, onPiPChange, isMainPlayer }: DDRPlayerProps) {
    const videoRef = useRef<HTMLVideoElement>(null);
    const [currentTime, setCurrentTime] = useState(0);
    const [duration, setDuration] = useState(0);
    const [isExpanded, setIsExpanded] = useState(false);

    const currentItem = playlist.items[playlist.currentItemIndex];

    // Update video source when item changes (require playable src)
    useEffect(() => {
        if (videoRef.current && currentItem?.media?.src) {
            videoRef.current.src = currentItem.media.src;
            if (playlist.isPlaying) {
                videoRef.current.play().catch(console.error);
            }
        }
    }, [playlist.currentItemIndex, currentItem?.id, currentItem?.media?.src, playlist.isPlaying]);

    // Handle playback state
    useEffect(() => {
        if (videoRef.current) {
            videoRef.current.volume = playlist.volume;
            if (playlist.isPlaying) {
                videoRef.current.play().catch(console.error);
            } else {
                videoRef.current.pause();
            }
        }
    }, [playlist.isPlaying, playlist.volume]);

    const handleTimeUpdate = () => {
        if (videoRef.current) {
            setCurrentTime(videoRef.current.currentTime);
            setDuration(videoRef.current.duration || 0);
        }
    };

    const handleEnded = () => {
        if (playlist.isLooping && playlist.items.length === 1) {
            // Single item loop
            if (videoRef.current) {
                videoRef.current.currentTime = 0;
                videoRef.current.play().catch(console.error);
            }
        } else if (playlist.currentItemIndex < playlist.items.length - 1) {
            // Next item
            let nextIndex = playlist.currentItemIndex + 1;
            if (playlist.isRandomized) {
                nextIndex = Math.floor(Math.random() * playlist.items.length);
            }
            onPlaylistChange({ ...playlist, currentItemIndex: nextIndex });
        } else if (playlist.isLooping) {
            // Loop back to start
            onPlaylistChange({ ...playlist, currentItemIndex: 0 });
        } else {
            // Stop
            onPlaylistChange({ ...playlist, isPlaying: false });
        }
    };

    const handlePlayPause = () => {
        onPlaylistChange({ ...playlist, isPlaying: !playlist.isPlaying });
    };

    const handleStop = () => {
        onPlaylistChange({ ...playlist, isPlaying: false, currentItemIndex: 0 });
        if (videoRef.current) {
            videoRef.current.currentTime = 0;
        }
    };

    const handlePrev = () => {
        const newIndex = Math.max(0, playlist.currentItemIndex - 1);
        onPlaylistChange({ ...playlist, currentItemIndex: newIndex });
    };

    const handleNext = () => {
        const newIndex = Math.min(playlist.items.length - 1, playlist.currentItemIndex + 1);
        onPlaylistChange({ ...playlist, currentItemIndex: newIndex });
    };

    const handleVolumeChange = (e: React.ChangeEvent<HTMLInputElement>) => {
        const volume = parseFloat(e.target.value);
        onPlaylistChange({ ...playlist, volume });
        if (videoRef.current) {
            videoRef.current.volume = volume;
        }
    };

    const handleToggleMute = () => {
        onPlaylistChange({ ...playlist, volume: playlist.volume > 0 ? 0 : 0.8 });
    };

    const handleToggleLoop = () => {
        onPlaylistChange({ ...playlist, isLooping: !playlist.isLooping });
    };

    const handleToggleShuffle = () => {
        onPlaylistChange({ ...playlist, isRandomized: !playlist.isRandomized });
    };

    const handleTogglePiP = () => {
        onPlaylistChange({ ...playlist, pipEnabled: !playlist.pipEnabled });
        onPiPChange(!playlist.pipEnabled, playlist.pipPosition);
    };

    const handleItemClick = (index: number) => {
        onPlaylistChange({ ...playlist, currentItemIndex: index });
    };

    const handleRemoveItem = (index: number) => {
        const newItems = playlist.items.filter((_, i) => i !== index);
        let newIndex = playlist.currentItemIndex;
        if (index < playlist.currentItemIndex) {
            newIndex = Math.max(0, newIndex - 1);
        } else if (index === playlist.currentItemIndex) {
            newIndex = Math.min(newIndex, newItems.length - 1);
        }
        onPlaylistChange({ ...playlist, items: newItems, currentItemIndex: Math.max(0, newIndex) });
    };

    const progress = duration > 0 ? (currentTime / duration) * 100 : 0;

    return (
        <div className={`${styles.container} ${isExpanded ? styles.expanded : ''}`}>
            {/* Channel Header */}
            <div
                className={styles.header}
                style={{ borderLeftColor: CHANNEL_COLORS[channel] }}
            >
                <div className={styles.channelBadge} style={{ background: CHANNEL_COLORS[channel] }}>
                    DDR {channel}
                </div>
                <span className={styles.playlistName}>{playlist.name || 'Untitled'}</span>
                <div className={styles.headerActions}>
                    <button
                        className={`${styles.iconBtn} ${playlist.pipEnabled ? styles.active : ''}`}
                        onClick={handleTogglePiP}
                        title="Picture-in-Picture"
                    >
                        <Layers size={12} />
                    </button>
                    <button
                        className={styles.iconBtn}
                        onClick={() => setIsExpanded(!isExpanded)}
                    >
                        {isExpanded ? <Minimize2 size={12} /> : <Maximize2 size={12} />}
                    </button>
                </div>
            </div>

            {/* Video Preview (hidden but functional) */}
            <video
                ref={videoRef}
                className={styles.video}
                onTimeUpdate={handleTimeUpdate}
                onEnded={handleEnded}
                onLoadedMetadata={handleTimeUpdate}
                onError={(e) => console.error('[DDRPlayer] Video playback error:', e)}
                muted={playlist.volume === 0}
            />

            {/* Now Playing */}
            {currentItem && (
                <div className={styles.nowPlaying}>
                    <span className={styles.nowPlayingLabel}>NOW</span>
                    <span className={styles.nowPlayingTitle}>{currentItem.media.title}</span>
                    <span className={styles.nowPlayingTime}>
                        {formatDuration(Math.floor(currentTime))} / {formatDuration(Math.floor(duration))}
                    </span>
                </div>
            )}

            {/* Progress Bar */}
            <div className={styles.progressBar}>
                <div className={styles.progressFill} style={{ width: `${progress}%` }} />
            </div>

            {/* Controls */}
            {/* Controls */}
            <div className={styles.controls}>
                <div className={styles.controlRow}>
                    <button className={styles.controlBtn} onClick={handlePrev} disabled={playlist.items.length === 0}>
                        <SkipBack size={14} />
                    </button>
                    <button
                        className={`${styles.controlBtn} ${styles.playBtn} ${playlist.isPlaying ? styles.playing : ''}`}
                        onClick={handlePlayPause}
                        disabled={playlist.items.length === 0}
                    >
                        {playlist.isPlaying ? <Pause size={16} /> : <Play size={16} />}
                    </button>
                    <button className={styles.controlBtn} onClick={handleStop} disabled={playlist.items.length === 0}>
                        <Square size={14} />
                    </button>
                    <button className={styles.controlBtn} onClick={handleNext} disabled={playlist.items.length === 0}>
                        <SkipForward size={14} />
                    </button>
                </div>

                <div className={styles.controlRow}>
                    <button
                        className={`${styles.iconBtn} ${playlist.isLooping ? styles.active : ''}`}
                        onClick={handleToggleLoop}
                        title="Loop"
                    >
                        <Repeat size={12} />
                    </button>
                    <button
                        className={`${styles.iconBtn} ${playlist.isRandomized ? styles.active : ''}`}
                        onClick={handleToggleShuffle}
                        title="Shuffle"
                    >
                        <Shuffle size={12} />
                    </button>
                    <div className={styles.volumeControl}>
                        <button className={styles.iconBtn} onClick={handleToggleMute}>
                            {playlist.volume > 0 ? <Volume2 size={12} /> : <VolumeX size={12} />}
                        </button>
                        <input
                            type="range"
                            min="0"
                            max="1"
                            step="0.05"
                            value={playlist.volume}
                            onChange={handleVolumeChange}
                            className={styles.volumeSlider}
                        />
                    </div>
                </div>
            </div>

            {/* Playlist (expanded view) */}
            {isExpanded && (
                <div className={styles.playlist}>
                    <div className={styles.playlistHeader}>
                        <span>Playlist ({playlist.items.length})</span>
                    </div>
                    <div className={styles.playlistItems}>
                        {playlist.items.map((item, index) => (
                            <div
                                key={item.id}
                                className={`${styles.playlistItem} ${index === playlist.currentItemIndex ? styles.activeItem : ''}`}
                                onClick={() => handleItemClick(index)}
                            >
                                <span className={styles.itemIndex}>{index + 1}</span>
                                <span className={styles.itemTitle}>{item.media.title}</span>
                                <span className={styles.itemDuration}>{formatDuration(item.media.duration)}</span>
                                <button
                                    className={styles.removeBtn}
                                    onClick={(e) => { e.stopPropagation(); handleRemoveItem(index); }}
                                >
                                    <X size={10} />
                                </button>
                            </div>
                        ))}
                        {playlist.items.length === 0 && (
                            <div className={styles.emptyPlaylist}>
                                Right-click media items to send here
                            </div>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
}

export interface DDRPanelProps {
    channels: Record<DDRChannel, DDRPlaylist>;
    onChannelChange: (channel: DDRChannel, playlist: DDRPlaylist) => void;
    onSendToChannel: (channel: DDRChannel, items: PlaylistItem[]) => void;
    onAddMediaRequest?: (channel: DDRChannel) => void;
}

// DDR Panel with channel tabs (A | B | C | D)
export function DDRPanel({
    channels,
    onChannelChange,
    onSendToChannel,
    onAddMediaRequest,
}: DDRPanelProps) {
    const [activeChannel, setActiveChannel] = useState<DDRChannel>('A');
    const channelList: DDRChannel[] = ['A', 'B', 'C', 'D'];

    return (
        <div className={styles.panel}>
            <div className={styles.panelHeader}>
                <Film size={16} />
                <span>DDR Players</span>
                {onAddMediaRequest && (
                    <button
                        type="button"
                        className={styles.addMediaBtn}
                        onClick={() => onAddMediaRequest(activeChannel)}
                        title="Add media to this channel"
                    >
                        <Plus size={14} />
                        Add media
                    </button>
                )}
            </div>
            <div className={styles.channelTabs}>
                {channelList.map(ch => (
                    <button
                        key={ch}
                        type="button"
                        className={`${styles.channelTab} ${activeChannel === ch ? styles.channelTabActive : ''}`}
                        onClick={() => setActiveChannel(ch)}
                    >
                        {ch}
                    </button>
                ))}
            </div>
            <div className={styles.channelPane}>
                <DDRPlayer
                    key={activeChannel}
                    channel={activeChannel}
                    playlist={channels[activeChannel]}
                    onPlaylistChange={(p) => onChannelChange(activeChannel, p)}
                    onPiPChange={() => { }}
                />
            </div>
        </div>
    );
};

// Default empty DDR playlist
export function createEmptyDDRPlaylist(channel: DDRChannel): DDRPlaylist {
    return {
        id: `ddr_${channel}_${Date.now()}`,
        name: `DDR ${channel}`,
        channelId: channel,
        items: [],
        isLooping: true,
        isRandomized: false,
        pipEnabled: false,
        pipPosition: 'bottom-right',
        pipSize: 25,
        pipOpacity: 1,
        isPlaying: false,
        volume: 0.8,
        currentItemIndex: 0,
    };
}
