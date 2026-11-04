// ============================================
// ATSC Multi-Channel Panel
// UI for 4-6 channel ATSC output management
// ============================================

import React, { useState, useEffect, useCallback } from 'react';
import styles from './ATSCMultiChannelPanel.module.css';
import {
    Radio, Play, Pause, Settings, RefreshCw, Tv, Zap, AlertCircle,
    CheckCircle, XCircle, Monitor, HardDrive, Wifi, ChevronDown,
    ChevronUp, Edit2, Trash2, Plus, Save, Eye, EyeOff
} from 'lucide-react';
import { getATSCMultiChannelOutput, ATSCChannelConfig, ATSCMultiChannelConfig } from '../lib/atscMultiChannelOutput';
import { useToast } from './Toast';
import { OBSWebSocketClient } from '../lib/obsWebSocket';

export function ATSCMultiChannelPanel() {
    const { showToast } = useToast();
    const [isActive, setIsActive] = useState(false);
    const [config, setConfig] = useState<ATSCMultiChannelConfig | null>(null);
    const [selectedChannel, setSelectedChannel] = useState<number | null>(null);
    const [status, setStatus] = useState<any>(null);

    const multiChannel = getATSCMultiChannelOutput();

    // Load config
    useEffect(() => {
        // Load from storage or use default
        const defaultConfig: ATSCMultiChannelConfig = {
            transportStreamId: 0x0100,
            totalBitrate: 19392658, // ATSC standard: 19.392658 Mbps
            channels: [],
            psip: {
                networkName: 'PLAY PLAY',
                shortName: 'PLAY',
                modulationMode: 0x04, // 8-VSB
                carrierFrequency: 0,
            },
            output: {
                type: 'dektec_asi',
                dektecDevice: 0,
                dektecPort: 0, // Single ASI port for statmux
            },
            statmux: {
                enabled: true,
                minChannelBitrate: 1000000, // 1 Mbps minimum per channel
                maxChannelBitrate: 8000000, // 8 Mbps maximum per channel
                updateInterval: 5000, // Update every 5 seconds
                complexityWindow: 10, // 10 second complexity analysis window
            },
        };

        // Initialize with 4 channels
        for (let i = 0; i < 4; i++) {
            defaultConfig.channels.push({
                channelId: i + 1,
                name: `Channel ${i + 1}`,
                majorChannel: 4,
                minorChannel: i + 1,
                sourceId: i + 1,
                programNumber: 10 + i * 10,
                inputSource: {
                    type: 'udp',
                    url: `udp://127.0.0.1:${5000 + i}`,
                },
                encoding: {
                    videoBitrate: 4000000, // 4 Mbps
                    audioBitrate: 192000, // 192 kbps
                    resolution: '1920x1080',
                    framerate: 29.97,
                    profile: 'high',
                    level: '4.0',
                    gopSize: 30,
                    bFrames: 2,
                    keyframeInterval: 30,
                },
                pids: {
                    video: 0x1000 + i * 0x100,
                    audio: 0x1001 + i * 0x100,
                    pcr: 0x1000 + i * 0x100,
                    pmt: 0x0100 + i * 0x10,
                },
                enabled: true,
            });
        }

        // Output is already configured in defaultConfig.output
        setConfig(defaultConfig);
        multiChannel.setConfig(defaultConfig);
    }, []);

    // Update status
    useEffect(() => {
        if (!isActive) return;

        const interval = setInterval(() => {
            setStatus(multiChannel.getStatus());
        }, 1000);

        return () => clearInterval(interval);
    }, [isActive]);

    // Start/stop
    const handleStart = useCallback(async () => {
        if (!config) return;

        const result = await multiChannel.start();
        if (result.success) {
            setIsActive(true);
            showToast('ATSC multi-channel output started', 'success', 3000);
        } else {
            showToast(`Failed to start: ${result.error}`, 'error', 5000);
        }
    }, [config, showToast]);

    const handleStop = useCallback(async () => {
        const result = await multiChannel.stop();
        if (result.success) {
            setIsActive(false);
            showToast('ATSC multi-channel output stopped', 'info', 2000);
        }
    }, [showToast]);

    // Add/remove channels
    const handleAddChannel = useCallback(() => {
        if (!config || config.channels.length >= 6) {
            showToast('Maximum 6 channels supported', 'warning', 2000);
            return;
        }

        const newChannelId = config.channels.length + 1;
        const newChannel: ATSCChannelConfig = {
            channelId: newChannelId,
            name: `Channel ${newChannelId}`,
            majorChannel: 4,
            minorChannel: newChannelId,
            sourceId: newChannelId,
            programNumber: 10 + (newChannelId - 1) * 10,
            inputSource: {
                type: 'udp',
                url: `udp://127.0.0.1:${5000 + newChannelId - 1}`,
            },
            encoding: {
                videoBitrate: 4000000,
                audioBitrate: 192000,
                resolution: '1920x1080',
                framerate: 29.97,
                profile: 'high',
                level: '4.0',
                gopSize: 30,
                bFrames: 2,
                keyframeInterval: 30,
            },
            pids: {
                video: 0x1000 + (newChannelId - 1) * 0x100,
                audio: 0x1001 + (newChannelId - 1) * 0x100,
                pcr: 0x1000 + (newChannelId - 1) * 0x100,
                pmt: 0x0100 + (newChannelId - 1) * 0x10,
            },
            enabled: true,
        };

        setConfig({
            ...config,
            channels: [...config.channels, newChannel],
        });
    }, [config, showToast]);

    const handleRemoveChannel = useCallback((channelId: number) => {
        if (!config) return;
        setConfig({
            ...config,
            channels: config.channels.filter(ch => ch.channelId !== channelId),
        });
    }, [config]);

    // Toggle channel enabled
    const handleToggleChannel = useCallback((channelId: number) => {
        if (!config) return;
        setConfig({
            ...config,
            channels: config.channels.map(ch =>
                ch.channelId === channelId ? { ...ch, enabled: !ch.enabled } : ch
            ),
        });
    }, [config]);

    // Save config
    const handleSave = useCallback(() => {
        if (!config) return;
        multiChannel.setConfig(config);
        showToast('Configuration saved', 'success', 2000);
    }, [config, showToast]);

    if (!config) {
        return <div className={styles.container}>Loading...</div>;
    }

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Radio size={20} />
                    <h2>ATSC Multi-Channel Output (4-6 Channels)</h2>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.saveBtn}
                        onClick={handleSave}
                    >
                        <Save size={16} />
                        Save Config
                    </button>
                    {!isActive ? (
                        <button
                            className={styles.startBtn}
                            onClick={handleStart}
                        >
                            <Play size={16} />
                            Start
                        </button>
                    ) : (
                        <button
                            className={styles.stopBtn}
                            onClick={handleStop}
                        >
                            <Pause size={16} />
                            Stop
                        </button>
                    )}
                </div>
            </div>

            <div className={styles.content}>
                {/* Channels List */}
                <div className={styles.channelsSection}>
                    <div className={styles.sectionHeader}>
                        <h3>Channels ({config.channels.length}/6)</h3>
                        {config.channels.length < 6 && (
                            <button
                                className={styles.addBtn}
                                onClick={handleAddChannel}
                            >
                                <Plus size={16} />
                                Add Channel
                            </button>
                        )}
                    </div>

                    <div className={styles.channelsList}>
                        {config.channels.map(channel => (
                            <div
                                key={channel.channelId}
                                className={`${styles.channelCard} ${!channel.enabled ? styles.disabled : ''} ${selectedChannel === channel.channelId ? styles.selected : ''}`}
                                onClick={() => setSelectedChannel(channel.channelId)}
                            >
                                <div className={styles.channelHeader}>
                                    <div className={styles.channelInfo}>
                                        <span className={styles.channelNumber}>
                                            {channel.majorChannel}.{channel.minorChannel}
                                        </span>
                                        <span className={styles.channelName}>{channel.name}</span>
                                    </div>
                                    <div className={styles.channelActions}>
                                        <label className={styles.toggle}>
                                            <input
                                                type="checkbox"
                                                checked={channel.enabled}
                                                onChange={() => handleToggleChannel(channel.channelId)}
                                                onClick={(e) => e.stopPropagation()}
                                            />
                                            <span>{channel.enabled ? 'Enabled' : 'Disabled'}</span>
                                        </label>
                                        <button
                                            className={styles.iconBtn}
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                handleRemoveChannel(channel.channelId);
                                            }}
                                        >
                                            <Trash2 size={14} />
                                        </button>
                                    </div>
                                </div>
                                <div className={styles.channelDetails}>
                                    <div className={styles.detailRow}>
                                        <span>Input:</span>
                                        <span>{channel.inputSource.type} - {channel.inputSource.url || channel.inputSource.filePath || 'N/A'}</span>
                                    </div>
                                    <div className={styles.detailRow}>
                                        <span>Encoding:</span>
                                        <span>H.264 {channel.encoding.resolution} @ {channel.encoding.framerate}fps</span>
                                    </div>
                                    <div className={styles.detailRow}>
                                        <span>Bitrate:</span>
                                        <span>{(channel.encoding.videoBitrate / 1000000).toFixed(1)} Mbps video, {(channel.encoding.audioBitrate / 1000).toFixed(0)} kbps audio</span>
                                    </div>
                                    {status && status.channels.find((ch: any) => ch.channelId === channel.channelId) && (
                                        <div className={styles.statusRow}>
                                            <CheckCircle size={12} className={styles.statusIcon} />
                                            <span>Encoding active</span>
                                        </div>
                                    )}
                                </div>
                            </div>
                        ))}
                    </div>
                </div>

                {/* Channel Editor */}
                {selectedChannel && (
                    <div className={styles.editorSection}>
                        <h3>Edit Channel {selectedChannel}</h3>
                        {/* Channel editor form would go here */}
                        <p>Channel editor coming soon...</p>
                    </div>
                )}

                {/* Output */}
                <div className={styles.outputsSection}>
                    <h3>Output</h3>
                    <div className={styles.outputsList}>
                        <div className={styles.outputCard}>
                            <div className={styles.outputHeader}>
                                <span>ASI Output</span>
                                {status?.outputActive && (
                                    <span className={status.outputActive ? styles.active : styles.inactive}>
                                        {status.outputActive ? 'Active' : 'Inactive'}
                                    </span>
                                )}
                            </div>
                            <div className={styles.outputDetails}>
                                <span>Type: {config.output.type}</span>
                                {config.output.type === 'dektec_asi' && (
                                    <span>Device {config.output.dektecDevice}, Port {config.output.dektecPort}</span>
                                )}
                            </div>
                        </div>
                    </div>
                </div>

                {/* Status */}
                {status && (
                    <div className={styles.statusSection}>
                        <h3>Status</h3>
                        <div className={styles.statusGrid}>
                            <div className={styles.statusItem}>
                                <span>Mux Active:</span>
                                <span className={status.muxActive ? styles.active : styles.inactive}>
                                    {status.muxActive ? 'Yes' : 'No'}
                                </span>
                            </div>
                            <div className={styles.statusItem}>
                                <span>PSIP Injected:</span>
                                <span className={status.psipInjected ? styles.active : styles.inactive}>
                                    {status.psipInjected ? 'Yes' : 'No'}
                                </span>
                            </div>
                            <div className={styles.statusItem}>
                                <span>Active Channels:</span>
                                <span>{status.channels.filter((ch: any) => ch.encoding).length}</span>
                            </div>
                            <div className={styles.statusItem}>
                                <span>Output Active:</span>
                                <span className={status.outputActive ? styles.active : styles.inactive}>
                                    {status.outputActive ? 'Yes' : 'No'}
                                </span>
                            </div>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

