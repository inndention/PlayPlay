import React, { useState } from 'react';
import { Settings, Plus, Trash2, Clock, Tv } from 'lucide-react';
import type { ATSCMuxConfig } from '../lib/types';
import styles from './PSIPConfigPanel.module.css';

interface PSIPConfigPanelProps {
    config: ATSCMuxConfig['psiSi'];
    onUpdate: (psiSi: ATSCMuxConfig['psiSi']) => void;
}

export function PSIPConfigPanel({ config, onUpdate }: PSIPConfigPanelProps) {
    const [expanded, setExpanded] = useState<string | null>(null);

    const updatePsip = (updates: Partial<ATSCMuxConfig['psiSi']['psip']>) => {
        onUpdate({
            ...config,
            psip: {
                ...config.psip,
                ...updates,
            },
        });
    };

    const updateVct = (updates: Partial<ATSCMuxConfig['psiSi']['psip']['vct']>) => {
        updatePsip({
            vct: {
                ...config.psip?.vct,
                ...updates,
            } as any,
        });
    };

    const addVctChannel = () => {
        const channels = config.psip?.vct?.channels || [];
        updateVct({
            channels: [
                ...channels,
                {
                    shortName: 'CHANNEL',
                    majorChannelNumber: channels.length + 1,
                    minorChannelNumber: 1,
                    modulationMode: 0x04, // 8-VSB
                    carrierFrequency: 0,
                    channelTSID: 0x0001,
                    programNumber: 1,
                    ETMLocation: 0,
                    accessControlled: false,
                    hidden: false,
                    hideGuide: false,
                    serviceType: 0x02, // ATSC digital TV
                    sourceId: channels.length + 1,
                },
            ],
        });
    };

    const removeVctChannel = (index: number) => {
        const channels = config.psip?.vct?.channels || [];
        updateVct({
            channels: channels.filter((_, i) => i !== index),
        });
    };

    const updateVctChannel = (index: number, updates: Partial<ATSCMuxConfig['psiSi']['psip']['vct']['channels'][0]>) => {
        const channels = config.psip?.vct?.channels || [];
        updateVct({
            channels: channels.map((ch, i) => i === index ? { ...ch, ...updates } : ch),
        });
    };

    const addEitEvent = () => {
        const events = config.psip?.eit?.events || [];
        updatePsip({
            eit: {
                ...config.psip?.eit,
                enabled: true,
                events: [
                    ...events,
                    {
                        eventId: events.length + 1,
                        startTime: Math.floor(Date.now() / 1000),
                        lengthInSeconds: 3600,
                        title: 'New Event',
                    },
                ],
            } as any,
        });
    };

    const removeEitEvent = (index: number) => {
        const events = config.psip?.eit?.events || [];
        updatePsip({
            eit: {
                ...config.psip?.eit,
                events: events.filter((_, i) => i !== index),
            } as any,
        });
    };

    return (
        <div className={styles.container}>
            <div className={styles.header} onClick={() => setExpanded(expanded === 'psip' ? null : 'psip')}>
                <Settings size={16} />
                <span>ATSC PSIP Configuration</span>
            </div>

            {expanded === 'psip' && (
                <div className={styles.content}>
                    {/* PSIP Enable */}
                    <div className={styles.section}>
                        <label className={styles.checkboxLabel}>
                            <input
                                type="checkbox"
                                checked={config.psip?.enabled || false}
                                onChange={(e) => updatePsip({ enabled: e.target.checked })}
                            />
                            <span>Enable PSIP Tables</span>
                        </label>
                    </div>

                    {/* VCT (Virtual Channel Table) */}
                    {config.psip?.enabled && (
                        <>
                            <div className={styles.section}>
                                <div className={styles.sectionHeader}>
                                    <Tv size={14} />
                                    <span>Virtual Channel Table (VCT)</span>
                                </div>
                                <label className={styles.checkboxLabel}>
                                    <input
                                        type="checkbox"
                                        checked={config.psip.vct?.enabled || false}
                                        onChange={(e) => updateVct({ enabled: e.target.checked })}
                                    />
                                    <span>Enable VCT</span>
                                </label>

                                {config.psip.vct?.enabled && (
                                    <>
                                        <div className={styles.field}>
                                            <label>Transport Stream ID</label>
                                            <input
                                                type="number"
                                                value={config.psip.vct.transportStreamId || 0x0001}
                                                onChange={(e) => updateVct({ transportStreamId: parseInt(e.target.value) || 0x0001 })}
                                                min="0"
                                                max="65535"
                                            />
                                        </div>

                                        <div className={styles.channels}>
                                            <div className={styles.channelsHeader}>
                                                <span>Channels</span>
                                                <button className={styles.addBtn} onClick={addVctChannel}>
                                                    <Plus size={14} /> Add Channel
                                                </button>
                                            </div>
                                            {config.psip.vct.channels?.map((channel, index) => (
                                                <div key={index} className={styles.channelItem}>
                                                    <div className={styles.channelFields}>
                                                        <div className={styles.field}>
                                                            <label>Short Name (7 chars)</label>
                                                            <input
                                                                type="text"
                                                                value={channel.shortName}
                                                                onChange={(e) => updateVctChannel(index, { shortName: e.target.value.substring(0, 7) })}
                                                                maxLength={7}
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Major Channel</label>
                                                            <input
                                                                type="number"
                                                                value={channel.majorChannelNumber}
                                                                onChange={(e) => updateVctChannel(index, { majorChannelNumber: parseInt(e.target.value) || 0 })}
                                                                min="0"
                                                                max="999"
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Minor Channel</label>
                                                            <input
                                                                type="number"
                                                                value={channel.minorChannelNumber}
                                                                onChange={(e) => updateVctChannel(index, { minorChannelNumber: parseInt(e.target.value) || 0 })}
                                                                min="0"
                                                                max="999"
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Carrier Frequency (Hz)</label>
                                                            <input
                                                                type="number"
                                                                value={channel.carrierFrequency}
                                                                onChange={(e) => updateVctChannel(index, { carrierFrequency: parseInt(e.target.value) || 0 })}
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Channel TSID</label>
                                                            <input
                                                                type="number"
                                                                value={channel.channelTSID}
                                                                onChange={(e) => updateVctChannel(index, { channelTSID: parseInt(e.target.value) || 0 })}
                                                                min="0"
                                                                max="65535"
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Program Number</label>
                                                            <input
                                                                type="number"
                                                                value={channel.programNumber}
                                                                onChange={(e) => updateVctChannel(index, { programNumber: parseInt(e.target.value) || 1 })}
                                                                min="1"
                                                                max="65535"
                                                            />
                                                        </div>
                                                        <div className={styles.field}>
                                                            <label>Source ID</label>
                                                            <input
                                                                type="number"
                                                                value={channel.sourceId}
                                                                onChange={(e) => updateVctChannel(index, { sourceId: parseInt(e.target.value) || 1 })}
                                                                min="0"
                                                                max="65535"
                                                            />
                                                        </div>
                                                    </div>
                                                    <button className={styles.removeBtn} onClick={() => removeVctChannel(index)}>
                                                        <Trash2 size={14} />
                                                    </button>
                                                </div>
                                            ))}
                                        </div>
                                    </>
                                )}
                            </div>

                            {/* EIT (Event Information Table) */}
                            <div className={styles.section}>
                                <div className={styles.sectionHeader}>
                                    <Clock size={14} />
                                    <span>Event Information Table (EIT)</span>
                                </div>
                                <label className={styles.checkboxLabel}>
                                    <input
                                        type="checkbox"
                                        checked={config.psip.eit?.enabled || false}
                                        onChange={(e) => updatePsip({ eit: { ...config.psip?.eit, enabled: e.target.checked } as any })}
                                    />
                                    <span>Enable EIT</span>
                                </label>

                                {config.psip.eit?.enabled && (
                                    <>
                                        <div className={styles.eventsHeader}>
                                            <span>Events</span>
                                            <button className={styles.addBtn} onClick={addEitEvent}>
                                                <Plus size={14} /> Add Event
                                            </button>
                                        </div>
                                        {config.psip.eit.events?.map((event, index) => (
                                            <div key={index} className={styles.eventItem}>
                                                <div className={styles.field}>
                                                    <label>Event ID</label>
                                                    <input
                                                        type="number"
                                                        value={event.eventId}
                                                        onChange={(e) => {
                                                            const events = config.psip.eit?.events || [];
                                                            updatePsip({
                                                                eit: {
                                                                    ...config.psip.eit,
                                                                    events: events.map((ev, i) => i === index ? { ...ev, eventId: parseInt(e.target.value) || 1 } : ev),
                                                                } as any,
                                                            });
                                                        }}
                                                    />
                                                </div>
                                                <div className={styles.field}>
                                                    <label>Title</label>
                                                    <input
                                                        type="text"
                                                        value={event.title}
                                                        onChange={(e) => {
                                                            const events = config.psip.eit?.events || [];
                                                            updatePsip({
                                                                eit: {
                                                                    ...config.psip.eit,
                                                                    events: events.map((ev, i) => i === index ? { ...ev, title: e.target.value } : ev),
                                                                } as any,
                                                            });
                                                        }}
                                                    />
                                                </div>
                                                <div className={styles.field}>
                                                    <label>Start Time (Unix timestamp)</label>
                                                    <input
                                                        type="number"
                                                        value={event.startTime}
                                                        onChange={(e) => {
                                                            const events = config.psip.eit?.events || [];
                                                            updatePsip({
                                                                eit: {
                                                                    ...config.psip.eit,
                                                                    events: events.map((ev, i) => i === index ? { ...ev, startTime: parseInt(e.target.value) || 0 } : ev),
                                                                } as any,
                                                            });
                                                        }}
                                                    />
                                                </div>
                                                <div className={styles.field}>
                                                    <label>Length (seconds)</label>
                                                    <input
                                                        type="number"
                                                        value={event.lengthInSeconds}
                                                        onChange={(e) => {
                                                            const events = config.psip.eit?.events || [];
                                                            updatePsip({
                                                                eit: {
                                                                    ...config.psip.eit,
                                                                    events: events.map((ev, i) => i === index ? { ...ev, lengthInSeconds: parseInt(e.target.value) || 0 } : ev),
                                                                } as any,
                                                            });
                                                        }}
                                                    />
                                                </div>
                                                <button className={styles.removeBtn} onClick={() => removeEitEvent(index)}>
                                                    <Trash2 size={14} />
                                                </button>
                                            </div>
                                        ))}
                                    </>
                                )}
                            </div>

                            {/* STT (System Time Table) */}
                            <div className={styles.section}>
                                <label className={styles.checkboxLabel}>
                                    <input
                                        type="checkbox"
                                        checked={config.psip.stt?.enabled || false}
                                        onChange={(e) => updatePsip({ stt: { enabled: e.target.checked } as any })}
                                    />
                                    <span>Enable STT (System Time Table) - Auto-updated</span>
                                </label>
                            </div>

                            {/* RRT (Rating Region Table) */}
                            <div className={styles.section}>
                                <label className={styles.checkboxLabel}>
                                    <input
                                        type="checkbox"
                                        checked={config.psip.rrt?.enabled || false}
                                        onChange={(e) => updatePsip({ rrt: { enabled: e.target.checked, ratingRegion: 0 } as any })}
                                    />
                                    <span>Enable RRT (Rating Region Table)</span>
                                </label>
                                {config.psip.rrt?.enabled && (
                                    <div className={styles.field}>
                                        <label>Rating Region (0 = US)</label>
                                        <input
                                            type="number"
                                            value={config.psip.rrt.ratingRegion || 0}
                                            onChange={(e) => updatePsip({ rrt: { enabled: true, ratingRegion: parseInt(e.target.value) || 0 } as any })}
                                            min="0"
                                            max="255"
                                        />
                                    </div>
                                )}
                            </div>
                        </>
                    )}

                    {/* ATSC 3.0 VTC */}
                    <div className={styles.section}>
                        <div className={styles.sectionHeader}>
                            <Tv size={14} />
                            <span>ATSC 3.0 VTC (Virtual Transport Channel)</span>
                        </div>
                        <label className={styles.checkboxLabel}>
                            <input
                                type="checkbox"
                                checked={config.atsc3?.enabled || false}
                                onChange={(e) => onUpdate({
                                    ...config,
                                    atsc3: {
                                        enabled: e.target.checked,
                                        vtc: config.atsc3?.vtc || {
                                            channelFrequency: 26.1,
                                            channelNumber: 26,
                                            subChannelNumber: 1,
                                            serviceId: 1,
                                            plpId: 0,
                                        },
                                    },
                                })}
                            />
                            <span>Enable ATSC 3.0 VTC</span>
                        </label>

                        {config.atsc3?.enabled && config.atsc3.vtc && (
                            <div className={styles.vtcFields}>
                                <div className={styles.field}>
                                    <label>Channel Frequency (MHz, e.g., 26.1)</label>
                                    <input
                                        type="number"
                                        step="0.1"
                                        value={config.atsc3.vtc.channelFrequency}
                                        onChange={(e) => onUpdate({
                                            ...config,
                                            atsc3: {
                                                ...config.atsc3,
                                                vtc: { ...config.atsc3.vtc, channelFrequency: parseFloat(e.target.value) || 26.1 },
                                            },
                                        })}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Physical Channel Number</label>
                                    <input
                                        type="number"
                                        value={config.atsc3.vtc.channelNumber}
                                        onChange={(e) => onUpdate({
                                            ...config,
                                            atsc3: {
                                                ...config.atsc3,
                                                vtc: { ...config.atsc3.vtc, channelNumber: parseInt(e.target.value) || 26 },
                                            },
                                        })}
                                        min="2"
                                        max="51"
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Sub-Channel Number</label>
                                    <input
                                        type="number"
                                        value={config.atsc3.vtc.subChannelNumber}
                                        onChange={(e) => onUpdate({
                                            ...config,
                                            atsc3: {
                                                ...config.atsc3,
                                                vtc: { ...config.atsc3.vtc, subChannelNumber: parseInt(e.target.value) || 1 },
                                            },
                                        })}
                                        min="1"
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Service ID</label>
                                    <input
                                        type="number"
                                        value={config.atsc3.vtc.serviceId}
                                        onChange={(e) => onUpdate({
                                            ...config,
                                            atsc3: {
                                                ...config.atsc3,
                                                vtc: { ...config.atsc3.vtc, serviceId: parseInt(e.target.value) || 1 },
                                            },
                                        })}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>PLP ID (Physical Layer Pipe)</label>
                                    <input
                                        type="number"
                                        value={config.atsc3.vtc.plpId}
                                        onChange={(e) => onUpdate({
                                            ...config,
                                            atsc3: {
                                                ...config.atsc3,
                                                vtc: { ...config.atsc3.vtc, plpId: parseInt(e.target.value) || 0 },
                                            },
                                        })}
                                        min="0"
                                        max="63"
                                    />
                                </div>
                            </div>
                        )}
                    </div>

                    {/* Low Jitter / Constant Bitrate */}
                    <div className={styles.section}>
                        <div className={styles.sectionHeader}>
                            <Settings size={14} />
                            <span>Output Settings (Professional Exciter)</span>
                        </div>
                        <label className={styles.checkboxLabel}>
                            <input
                                type="checkbox"
                                checked={config.lowJitter || false}
                                onChange={(e) => onUpdate({ ...config, lowJitter: e.target.checked })}
                            />
                            <span>Low Jitter Mode (Constant Bitrate, Proper PCR Timing)</span>
                        </label>
                        {config.lowJitter && (
                            <div className={styles.field}>
                                <label>Constant Bitrate (bps, e.g., 19392658 for ATSC 1.0)</label>
                                <input
                                    type="number"
                                    value={config.constantBitrate || 19392658}
                                    onChange={(e) => onUpdate({ ...config, constantBitrate: parseInt(e.target.value) || 19392658 })}
                                />
                            </div>
                        )}
                    </div>
                </div>
            )}
        </div>
    );
}






