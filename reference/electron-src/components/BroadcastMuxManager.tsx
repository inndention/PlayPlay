/**
 * Broadcast Mux Manager
 * 4-channel ATSC statistical multiplexer interface
 */

import React, { useState, useEffect, useCallback } from 'react';
import { Play, Square, Settings, AlertCircle, CheckCircle, XCircle } from 'lucide-react';
import type { ATSCMuxConfig, MuxChannelConfig, MuxChannelStatus } from '../lib/types';
import { ATSCMuxEngine } from '../lib/atscMuxEngine';
import { MuxChannelConfigPanel } from './MuxChannelConfig';
import { PSIPConfigPanel } from './PSIPConfigPanel';
import styles from './BroadcastMuxManager.module.css';

const getAPI = () => (window as any).electronAPI;

export function BroadcastMuxManager() {
    const [config, setConfig] = useState<ATSCMuxConfig>({
        enabled: false,
        channels: [
            { channelId: 1, enabled: false, inputSource: 'udp_stream', inputConfig: {}, encoding: { codec: 'libx264', videoBitrate: '5M', audioCodec: 'aac', audioBitrate: '192k', resolution: '1920x1080', framerate: 30 }, output: { destination: 'dektec_asi' } },
            { channelId: 2, enabled: false, inputSource: 'udp_stream', inputConfig: {}, encoding: { codec: 'libx264', videoBitrate: '5M', audioCodec: 'aac', audioBitrate: '192k', resolution: '1920x1080', framerate: 30 }, output: { destination: 'dektec_asi' } },
            { channelId: 3, enabled: false, inputSource: 'udp_stream', inputConfig: {}, encoding: { codec: 'libx264', videoBitrate: '5M', audioCodec: 'aac', audioBitrate: '192k', resolution: '1920x1080', framerate: 30 }, output: { destination: 'dektec_asi' } },
            { channelId: 4, enabled: false, inputSource: 'udp_stream', inputConfig: {}, encoding: { codec: 'libx264', videoBitrate: '5M', audioCodec: 'aac', audioBitrate: '192k', resolution: '1920x1080', framerate: 30 }, output: { destination: 'dektec_asi' } },
        ],
        statmux: {
            enabled: true,
            totalBitrate: 19392658, // ATSC 1.0
        },
        psiSi: {
            enabled: true,
            networkName: 'PLAY PLAY Network',
            serviceName: 'PLAY PLAY',
            serviceId: 1,
            programNumber: 1,
            psip: {
                enabled: false,
                mgt: {
                    tableId: 0xC7,
                    version: 0,
                    protocolVersion: 0,
                },
                vct: {
                    enabled: false,
                    transportStreamId: 0x0001,
                    protocolVersion: 0,
                    channels: [],
                },
                eit: {
                    enabled: false,
                    events: [],
                },
                stt: {
                    enabled: false,
                    systemTime: Math.floor(Date.now() / 1000),
                },
                rrt: {
                    enabled: false,
                    ratingRegion: 0,
                },
            },
            atsc3: {
                enabled: false,
                vtc: {
                    enabled: false,
                    channelFrequency: 26.1,
                    channelNumber: 26,
                    subChannelNumber: 1,
                    serviceId: 1,
                    plpId: 0,
                },
            },
            lowJitter: true,
            constantBitrate: 19392658,
        },
        compliance: {
            validate: true,
            atscVersion: '1.0',
        },
    });

    const [muxEngine] = useState(() => new ATSCMuxEngine(getAPI()));
    const [isRunning, setIsRunning] = useState(false);
    const [channelStatuses, setChannelStatuses] = useState<MuxChannelStatus[]>([]);
    const [error, setError] = useState<string | null>(null);

    useEffect(() => {
        muxEngine.setConfig(config);
    }, [config, muxEngine]);

    useEffect(() => {
        const interval = setInterval(() => {
            const statuses = muxEngine.getChannelStatuses();
            setChannelStatuses(statuses);
        }, 1000);

        return () => clearInterval(interval);
    }, [muxEngine]);

    const handleStart = useCallback(async () => {
        setError(null);
        const result = await muxEngine.start();
        if (result.success) {
            setIsRunning(true);
        } else {
            setError(result.error || 'Failed to start mux');
        }
    }, [muxEngine]);

    const handleStop = useCallback(async () => {
        await muxEngine.stop();
        setIsRunning(false);
    }, [muxEngine]);

    const handleChannelUpdate = useCallback((channelId: number, channelConfig: MuxChannelConfig) => {
        setConfig(prev => ({
            ...prev,
            channels: prev.channels.map(ch => ch.channelId === channelId ? channelConfig : ch),
        }));
    }, []);

    const handleStatMuxUpdate = useCallback((statmux: ATSCMuxConfig['statmux']) => {
        setConfig(prev => ({ ...prev, statmux }));
    }, []);

    const handlePsiSiUpdate = useCallback((psiSi: ATSCMuxConfig['psiSi']) => {
        setConfig(prev => ({ ...prev, psiSi }));
    }, []);

    // Listen for PSIP updates from playlist editor
    useEffect(() => {
        const handlePSIPUpdate = (event: CustomEvent) => {
            const { events, scheduleStartTime, channelName } = event.detail;
            
            // Auto-update EIT if PSIP is enabled
            if (config.psiSi.enabled && config.psiSi.psip?.enabled && config.psiSi.psip.eit) {
                setConfig(prev => ({
                    ...prev,
                    psiSi: {
                        ...prev.psiSi,
                        psip: {
                            ...prev.psiSi.psip,
                            eit: {
                                ...prev.psiSi.psip?.eit,
                                enabled: true,
                                events: events.map((e: any) => ({
                                    eventId: e.eventId,
                                    startTime: e.startTime,
                                    lengthInSeconds: e.lengthInSeconds,
                                    title: e.title,
                                    description: e.description,
                                })),
                            },
                        },
                    },
                }));
                
                console.log('[BroadcastMux] Auto-updated EIT with', events.length, 'events from playlist');
            }
        };

        window.addEventListener('psip-update', handlePSIPUpdate as EventListener);
        return () => window.removeEventListener('psip-update', handlePSIPUpdate as EventListener);
    }, [config.psiSi.enabled, config.psiSi.psip?.enabled]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h2>4-Channel ATSC Broadcast Mux</h2>
                <div className={styles.controls}>
                    {!isRunning ? (
                        <button className={styles.startButton} onClick={handleStart}>
                            <Play size={16} /> Start Mux
                        </button>
                    ) : (
                        <button className={styles.stopButton} onClick={handleStop}>
                            <Square size={16} /> Stop Mux
                        </button>
                    )}
                </div>
            </div>

            {error && (
                <div className={styles.error}>
                    <AlertCircle size={16} /> {error}
                </div>
            )}

            <div className={styles.statusGrid}>
                {channelStatuses.map(status => (
                    <div key={status.channelId} className={styles.channelStatus}>
                        <div className={styles.channelHeader}>
                            <span>Channel {status.channelId}</span>
                            {status.enabled ? (
                                status.encodingActive && status.outputActive ? (
                                    <CheckCircle size={16} className={styles.statusOk} />
                                ) : (
                                    <XCircle size={16} className={styles.statusError} />
                                )
                            ) : (
                                <XCircle size={16} className={styles.statusDisabled} />
                            )}
                        </div>
                        {status.currentBitrate && (
                            <div className={styles.statusInfo}>
                                Bitrate: {(status.currentBitrate / 1000000).toFixed(2)} Mbps
                            </div>
                        )}
                        {status.frameRate && (
                            <div className={styles.statusInfo}>
                                Frame Rate: {status.frameRate.toFixed(2)} fps
                            </div>
                        )}
                        {status.errors && status.errors.length > 0 && (
                            <div className={styles.statusErrors}>
                                {status.errors?.map((err: string, idx: number) => (
                                    <div key={idx}>{err}</div>
                                ))}
                            </div>
                        )}
                    </div>
                ))}
            </div>

            <div className={styles.configSection}>
                <h3>Channel Configuration</h3>
                <div className={styles.channelsGrid}>
                    {config.channels.map(channel => (
                        <MuxChannelConfigPanel
                            key={channel.channelId}
                            channel={channel}
                            status={channelStatuses.find(s => s.channelId === channel.channelId)}
                            onUpdate={(updated) => handleChannelUpdate(channel.channelId, updated)}
                        />
                    ))}
                </div>
            </div>

            <div className={styles.configSection}>
                <h3>Statistical Multiplexing</h3>
                <div className={styles.statMuxConfig}>
                    <label>
                        <input
                            type="checkbox"
                            checked={config.statmux.enabled}
                            onChange={(e) => handleStatMuxUpdate({ ...config.statmux, enabled: e.target.checked })}
                        />
                        Enable StatMux
                    </label>
                    {config.statmux.enabled && (
                        <>
                            <div className={styles.field}>
                                <label>Total Bitrate (bps)</label>
                                <input
                                    type="number"
                                    value={config.statmux.totalBitrate}
                                    onChange={(e) => handleStatMuxUpdate({ ...config.statmux, totalBitrate: parseInt(e.target.value) || 0 })}
                                />
                                <small>ATSC 1.0: 19,392,658 bps</small>
                            </div>
                            <div className={styles.field}>
                                <label>Min Channel Bitrate (bps)</label>
                                <input
                                    type="number"
                                    value={config.statmux.minChannelBitrate || ''}
                                    onChange={(e) => handleStatMuxUpdate({ ...config.statmux, minChannelBitrate: parseInt(e.target.value) || undefined })}
                                    placeholder="Auto"
                                />
                            </div>
                            <div className={styles.field}>
                                <label>Max Channel Bitrate (bps)</label>
                                <input
                                    type="number"
                                    value={config.statmux.maxChannelBitrate || ''}
                                    onChange={(e) => handleStatMuxUpdate({ ...config.statmux, maxChannelBitrate: parseInt(e.target.value) || undefined })}
                                    placeholder="Auto"
                                />
                            </div>
                        </>
                    )}
                </div>
            </div>

            <div className={styles.configSection}>
                <PSIPConfigPanel
                    config={config.psiSi}
                    onUpdate={handlePsiSiUpdate}
                />
            </div>
        </div>
    );
}

