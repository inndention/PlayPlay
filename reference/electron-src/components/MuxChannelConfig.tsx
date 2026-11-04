/**
 * Mux Channel Configuration Panel
 * Per-channel configuration for the ATSC mux
 */

import React, { useState, useEffect } from 'react';
import type { MuxChannelConfig, MuxChannelStatus, MuxInputSourceType, SdiVideoFormat } from '../lib/types';
import { NdiSourceSelector } from './NdiSourceSelector';
import styles from './MuxChannelConfig.module.css';

interface Props {
    channel: MuxChannelConfig;
    status?: MuxChannelStatus;
    onUpdate: (channel: MuxChannelConfig) => void;
}

export function MuxChannelConfigPanel({ channel, status, onUpdate }: Props) {
    const [localChannel, setLocalChannel] = useState<MuxChannelConfig>(channel);

    useEffect(() => {
        setLocalChannel(channel);
    }, [channel]);

    const updateChannel = (updates: Partial<MuxChannelConfig>) => {
        const updated = { ...localChannel, ...updates };
        setLocalChannel(updated);
        onUpdate(updated);
    };

    return (
        <div className={styles.panel}>
            <div className={styles.header}>
                <label>
                    <input
                        type="checkbox"
                        checked={localChannel.enabled}
                        onChange={(e) => updateChannel({ enabled: e.target.checked })}
                    />
                    Channel {localChannel.channelId}
                </label>
                {status && (
                    <div className={styles.status}>
                        {status.encodingActive ? '●' : '○'}
                    </div>
                )}
            </div>

            {localChannel.enabled && (
                <div className={styles.config}>
                    <div className={styles.field}>
                        <label>Input Source</label>
                        <select
                            value={localChannel.inputSource}
                            onChange={(e) => updateChannel({ inputSource: e.target.value as MuxInputSourceType })}
                        >
                            <option value="playplay_instance">play_play Instance</option>
                            <option value="udp_stream">UDP Stream</option>
                            <option value="srt_stream">SRT Stream</option>
                            <option value="local_file">Local File</option>
                            <option value="ndi_source">NDI Source</option>
                            <option value="decklink_input">DeckLink Input</option>
                        </select>
                    </div>

                    {localChannel.inputSource === 'udp_stream' && (
                        <div className={styles.field}>
                            <label>UDP URL</label>
                            <input
                                type="text"
                                value={localChannel.inputConfig.udpUrl || ''}
                                onChange={(e) => updateChannel({
                                    inputConfig: { ...localChannel.inputConfig, udpUrl: e.target.value }
                                })}
                                placeholder="udp://127.0.0.1:5004"
                            />
                        </div>
                    )}

                    {localChannel.inputSource === 'srt_stream' && (
                        <div className={styles.field}>
                            <label>SRT URL</label>
                            <input
                                type="text"
                                value={localChannel.inputConfig.srtUrl || ''}
                                onChange={(e) => updateChannel({
                                    inputConfig: { ...localChannel.inputConfig, srtUrl: e.target.value }
                                })}
                                placeholder="srt://127.0.0.1:4000"
                            />
                        </div>
                    )}

                    {localChannel.inputSource === 'local_file' && (
                        <div className={styles.field}>
                            <label>File Path</label>
                            <input
                                type="text"
                                value={localChannel.inputConfig.filePath || ''}
                                onChange={(e) => updateChannel({
                                    inputConfig: { ...localChannel.inputConfig, filePath: e.target.value }
                                })}
                                placeholder="C:\path\to\file.ts"
                            />
                        </div>
                    )}

                    {localChannel.inputSource === 'ndi_source' && (
                        <NdiSourceSelector
                            selectedSource={localChannel.inputConfig.ndiSourceName}
                            onSelect={(sourceName) => updateChannel({
                                inputConfig: { ...localChannel.inputConfig, ndiSourceName: sourceName }
                            })}
                        />
                    )}

                    <div className={styles.section}>
                        <h4>Encoding</h4>
                        <div className={styles.field}>
                            <label>Codec</label>
                            <select
                                value={localChannel.encoding.codec}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, codec: e.target.value as any }
                                })}
                            >
                                <option value="h264_nvenc">NVENC H.264</option>
                                <option value="libx264">Software H.264</option>
                                <option value="mpeg2video">MPEG-2</option>
                            </select>
                        </div>
                        <div className={styles.field}>
                            <label>Video Bitrate</label>
                            <input
                                type="text"
                                value={localChannel.encoding.videoBitrate}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, videoBitrate: e.target.value }
                                })}
                            />
                        </div>
                        <div className={styles.field}>
                            <label>Audio Codec</label>
                            <select
                                value={localChannel.encoding.audioCodec}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, audioCodec: e.target.value as any }
                                })}
                            >
                                <option value="aac">AAC</option>
                                <option value="ac3">AC-3</option>
                                <option value="mp2">MP2</option>
                            </select>
                        </div>
                        <div className={styles.field}>
                            <label>Audio Bitrate</label>
                            <input
                                type="text"
                                value={localChannel.encoding.audioBitrate}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, audioBitrate: e.target.value }
                                })}
                            />
                        </div>
                        <div className={styles.field}>
                            <label>Resolution</label>
                            <input
                                type="text"
                                value={localChannel.encoding.resolution}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, resolution: e.target.value }
                                })}
                            />
                        </div>
                        <div className={styles.field}>
                            <label>Frame Rate</label>
                            <input
                                type="number"
                                value={localChannel.encoding.framerate}
                                onChange={(e) => updateChannel({
                                    encoding: { ...localChannel.encoding, framerate: parseFloat(e.target.value) || 30 }
                                })}
                            />
                        </div>
                    </div>

                    <div className={styles.section}>
                        <h4>Output</h4>
                        <div className={styles.field}>
                            <label>Destination</label>
                            <select
                                value={localChannel.output.destination}
                                onChange={(e) => updateChannel({
                                    output: { ...localChannel.output, destination: e.target.value as any }
                                })}
                            >
                                <option value="dektec_asi">DekTec ASI</option>
                                <option value="dektec_sdi">DekTec SDI</option>
                                <option value="udp">UDP</option>
                                <option value="file">File</option>
                            </select>
                        </div>

                        {localChannel.output.destination === 'dektec_asi' || localChannel.output.destination === 'dektec_sdi' ? (
                            <>
                                <div className={styles.field}>
                                    <label>DekTec Serial</label>
                                    <input
                                        type="number"
                                        value={localChannel.output.dektecSerial || ''}
                                        onChange={(e) => updateChannel({
                                            output: { ...localChannel.output, dektecSerial: parseInt(e.target.value) || undefined }
                                        })}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>DekTec Port</label>
                                    <input
                                        type="number"
                                        value={localChannel.output.dektecPort || ''}
                                        onChange={(e) => updateChannel({
                                            output: { ...localChannel.output, dektecPort: parseInt(e.target.value) || undefined }
                                        })}
                                    />
                                </div>
                                {localChannel.output.destination === 'dektec_sdi' && (
                                    <div className={styles.field}>
                                        <label>SDI Format</label>
                                        <select
                                            value={localChannel.output.dektecSdiFormat || '1080i59.94'}
                                            onChange={(e) => updateChannel({
                                                output: { ...localChannel.output, dektecSdiFormat: e.target.value as SdiVideoFormat }
                                            })}
                                        >
                                            <option value="720p59.94">720p59.94</option>
                                            <option value="720p60">720p60</option>
                                            <option value="1080i59.94">1080i59.94</option>
                                            <option value="1080i60">1080i60</option>
                                            <option value="1080p29.97">1080p29.97</option>
                                            <option value="1080p30">1080p30</option>
                                        </select>
                                    </div>
                                )}
                            </>
                        ) : localChannel.output.destination === 'udp' ? (
                            <div className={styles.field}>
                                <label>UDP URL</label>
                                <input
                                    type="text"
                                    value={localChannel.output.udpUrl || ''}
                                    onChange={(e) => updateChannel({
                                        output: { ...localChannel.output, udpUrl: e.target.value }
                                    })}
                                    placeholder="udp://127.0.0.1:5005"
                                />
                            </div>
                        ) : (
                            <div className={styles.field}>
                                <label>File Path</label>
                                <input
                                    type="text"
                                    value={localChannel.output.filePath || ''}
                                    onChange={(e) => updateChannel({
                                        output: { ...localChannel.output, filePath: e.target.value }
                                    })}
                                    placeholder="C:\output\channel.ts"
                                />
                            </div>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
}




