// ============================================
// MULTIVIEWER — Detached per-channel monitor
// 1x / 4x / 8x layout, VU meters, OTA volume
// ============================================

import React, { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import { PlayoutChannel, MultiChannelState } from '../lib/types';
import { Settings, Plus, Video, Radio, Trash2, Activity } from 'lucide-react';
import { OutputManager } from '../components/OutputManager';

export type StreamProtocol = 'srt' | 'udp' | 'tcp' | 'rtmp' | 'hls' | 'rtsp' | 'youtube' | 'facebook' | 'roku' | '.plus/.fun';

export interface StreamTargetPreset {
    id: string;
    name: string;
    protocol: StreamProtocol;
    encoder?: string;
    bitrate?: string;
    framerate?: string;
    address?: string;
    port?: string;
    mode?: 'caller' | 'listener';
    server?: string;
    key?: string;
    tuning?: string;
    profile?: string;
}

// ── VU Meter ─────────────────────────────────────────────────────────────────

interface VuMeterProps {
    label: string;
    level: number;      // 0-1 current level
    peak: number;       // 0-1 peak hold
    targetMin?: number; // 0-1 normalization lower bound (optional indicator)
    targetMax?: number; // 0-1 normalization upper bound
    color?: string;
    height?: number;
}

function VuMeter({ label, level, peak, targetMin, targetMax, color = '#10b981', height = 120 }: VuMeterProps) {
    // A continuous compact vertical VU meter
    const db = level > 0 ? (level - 1) * 60 : -60;
    
    return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 2 }}>
            <div style={{ position: 'relative', width: 8, height, backgroundColor: 'rgba(255,255,255,0.06)', borderRadius: 2, overflow: 'hidden' }}>
                {/* Level fill */}
                <div style={{
                    position: 'absolute',
                    bottom: 0,
                    left: 0,
                    right: 0,
                    height: `${level * 100}%`,
                    background: `linear-gradient(to top, ${color}, #f59e0b 80%, #ef4444 100%)`,
                    transition: 'height 0.05s ease-out'
                }} />
                {/* Peak line */}
                {peak > 0.01 && (
                    <div style={{
                        position: 'absolute',
                        bottom: `${peak * 100}%`,
                        left: 0,
                        right: 0,
                        height: 2,
                        backgroundColor: '#fff',
                        transition: 'bottom 0.1s linear'
                    }} />
                )}
                {/* Target zones */}
                {targetMin != null && targetMax != null && (
                    <div style={{
                        position: 'absolute',
                        bottom: `${targetMin * 100}%`,
                        height: `${(targetMax - targetMin) * 100}%`,
                        left: 0,
                        right: 0,
                        backgroundColor: 'rgba(16,185,129,0.3)',
                        borderTop: '1px solid rgba(16,185,129,0.8)',
                        borderBottom: '1px solid rgba(16,185,129,0.8)',
                        pointerEvents: 'none'
                    }} />
                )}
            </div>
            <div style={{ fontSize: 9, color: '#64748b', fontWeight: 700, letterSpacing: '0.05em', marginTop: 2 }}>{label}</div>
            <div style={{ fontSize: 8, color: '#475569', fontFamily: 'monospace' }}>
                {db <= -59 ? '–∞' : `${Math.round(db)}`}
            </div>
        </div>
    );
}

// ── Channel Cell ──────────────────────────────────────────────────────────────

interface ChannelCellProps {
    channel: PlayoutChannel;
    compact?: boolean;
}

interface AudioDeviceInfo {
    deviceId: string;
    label: string;
}

function ChannelCell({ channel, compact }: ChannelCellProps) {
    const api = (window as any).electronAPI;
    const casparCh: number = (channel as any).casparChannel ?? 0;

    // OTA volume state (0-1)
    const [otaVolume, setOtaVolume] = useState(1);
    const [normEnabled, setNormEnabled] = useState(false);

    // MONITOR state
    const [monEnabled, setMonEnabled] = useState(false);
    const [monVolume, setMonVolume] = useState(0.5);
    const [audioDevices, setAudioDevices] = useState<AudioDeviceInfo[]>([]);
    const [selectedDevice, setSelectedDevice] = useState<string>('default');

    // VU meter levels — simulated (0-1)
    const otaLevelRef = useRef(0);
    const monLevelRef = useRef(0);
    const otaPeakRef = useRef(0);
    const monPeakRef = useRef(0);
    const peakHoldTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
    const [, forceRedraw] = useState(0);

    // Stream routing state
    const [ndiEnabled, setNdiEnabled] = useState(false);
    
    // Global stream presets
    const [presets, setPresets] = useState<StreamTargetPreset[]>(() => {
        try {
            return JSON.parse(localStorage.getItem('playplay_stream_presets') || '[]');
        } catch { return []; }
    });

    const [streamTargets, setStreamTargets] = useState<{ 
        id: string; 
        presetId?: string;
        url?: string; // fallback if no preset
        enabled: boolean; 
        state?: 'ok'|'dropped'|'off'|'error'; 
        droppedFrames?: number; 
        bitrate?: number;
    }[]>([]);
    const [showStreamConfig, setShowStreamConfig] = useState(false);
    
    // "New Preset" UI state
    const [addingNew, setAddingNew] = useState(false);
    const [newPreset, setNewPreset] = useState<Partial<StreamTargetPreset>>({ protocol: 'rtmp' });

    const [showOutputManager, setShowOutputManager] = useState(false);

    // Normalization auto-gain loop
    const normTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);

    // Enumerate audio output devices
    useEffect(() => {
        const enumerate = async () => {
            try {
                await navigator.mediaDevices.getUserMedia({ audio: true }).catch(() => {});
                const devices = await navigator.mediaDevices.enumerateDevices();
                const outputs = devices
                    .filter(d => d.kind === 'audiooutput')
                    .map(d => ({ deviceId: d.deviceId, label: d.label || `Device ${d.deviceId.slice(0, 6)}` }));
                setAudioDevices([{ deviceId: 'default', label: 'Default' }, ...outputs]);
            } catch (_) {}
        };
        enumerate();
    }, []);

    // Simulate VU levels — follows volume with random noise + envelope
    useEffect(() => {
        let rafId: number;
        let envL = 0;
        let envR = 0;

        const tick = () => {
            // Simulate audio activity: random noise scaled by volume
            const active = channel.isPlaying ? 1 : 0.05;
            const noise = Math.random() * 0.15 + Math.random() * 0.1;
            const target = otaVolume * active * (0.55 + noise);
            envL += (target - envL) * 0.25 + (Math.random() - 0.5) * 0.04;
            envL = Math.max(0, Math.min(1, envL));

            const monNoise = Math.random() * 0.12 + Math.random() * 0.08;
            const monTarget = monEnabled ? monVolume * active * (0.50 + monNoise) : 0;
            envR += (monTarget - envR) * 0.3 + (Math.random() - 0.5) * 0.03;
            envR = Math.max(0, Math.min(1, envR));

            otaLevelRef.current = envL;
            monLevelRef.current = envR;

            // Peak hold
            if (envL > otaPeakRef.current) {
                otaPeakRef.current = envL;
                if (peakHoldTimer.current) clearTimeout(peakHoldTimer.current);
                peakHoldTimer.current = setTimeout(() => { otaPeakRef.current = 0; }, 1500);
            }
            if (envR > monPeakRef.current) monPeakRef.current = envR;

            forceRedraw(n => n + 1);
            rafId = requestAnimationFrame(tick);
        };

        rafId = requestAnimationFrame(tick);
        return () => {
            cancelAnimationFrame(rafId);
            if (peakHoldTimer.current) clearTimeout(peakHoldTimer.current);
        };
    }, [otaVolume, monVolume, monEnabled, channel.isPlaying]);

    // Normalization loop: every 2s, nudge OTA volume toward 75-85% target
    useEffect(() => {
        if (!normEnabled || !casparCh) return;
        normTimerRef.current = setInterval(() => {
            const lvl = otaLevelRef.current;
            const targetMid = 0.80; // ~-2dB
            const diff = targetMid - lvl;
            if (Math.abs(diff) > 0.03) {
                setOtaVolume(prev => {
                    const next = Math.max(0, Math.min(1, prev + diff * 0.15));
                    api?.casparSetMasterVolume?.(casparCh, next);
                    return next;
                });
            }
        }, 2000);
        return () => { if (normTimerRef.current) clearInterval(normTimerRef.current); };
    }, [normEnabled, casparCh, api]);

    // Mock live OSC/AMCP stream health monitor
    useEffect(() => {
        const interval = setInterval(() => {
            setStreamTargets(prev => prev.map(t => {
                if (!t.enabled) return { ...t, state: 'off' };
                // Simulate network conditions over time
                const rand = Math.random();
                if (rand > 0.95) {
                    return { ...t, state: 'error' };
                } else if (rand > 0.8) {
                    return { ...t, state: 'dropped', droppedFrames: Math.floor(Math.random() * 50) + 1 };
                } else {
                    return { ...t, state: 'ok', bitrate: 4500 + Math.floor(Math.random() * 1500) };
                }
            }));
        }, 5000);
        return () => clearInterval(interval);
    }, []);

    const handleOtaVolume = useCallback((v: number) => {
        setOtaVolume(v);
        if (casparCh && api?.casparSetMasterVolume) {
            api.casparSetMasterVolume(casparCh, v);
        }
    }, [casparCh, api]);

    const currentTitle = channel.currentItem?.media.title ?? '—';
    const nextTitle = channel.nextItem?.media.title;

    const cellBg = channel.isPlaying
        ? 'linear-gradient(180deg, rgba(16,185,129,0.06) 0%, rgba(9,9,11,0.95) 100%)'
        : 'rgba(9,9,11,0.95)';

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            background: cellBg,
            border: `1px solid ${channel.isPlaying ? 'rgba(16,185,129,0.35)' : 'rgba(255,255,255,0.07)'}`,
            borderRadius: 8,
            overflow: 'hidden',
            flex: 1,
            minWidth: 0,
        }}>
            {/* Header */}
            <div style={{
                padding: compact ? '4px 8px' : '6px 10px',
                borderBottom: '1px solid rgba(255,255,255,0.06)',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                gap: 6,
            }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6, flex: 1, overflow: 'hidden' }}>
                    <span style={{
                        fontSize: compact ? 9 : 10,
                        fontWeight: 800,
                        color: '#3b82f6',
                        letterSpacing: '0.1em',
                        fontFamily: 'monospace',
                        flexShrink: 0
                    }}>
                        {casparCh ? `CH${casparCh}` : channel.id.toUpperCase()}
                    </span>
                    <span style={{ fontSize: compact ? 10 : 11, fontWeight: 600, color: '#e2e8f0', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                        {channel.name}
                    </span>
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    {/* Routing Indicators */}
                    <div style={{ display: 'flex', gap: 4 }}>
                        <button
                            onClick={() => setNdiEnabled(!ndiEnabled)}
                            title="Toggle NDI Output"
                            style={{
                                background: ndiEnabled ? 'rgba(59, 130, 246, 0.2)' : 'transparent',
                                border: `1px solid ${ndiEnabled ? '#3b82f6' : 'rgba(255,255,255,0.1)'}`,
                                color: ndiEnabled ? '#3b82f6' : '#64748b',
                                fontSize: 8,
                                fontWeight: 800,
                                padding: '2px 4px',
                                borderRadius: 3,
                                cursor: 'pointer',
                            }}
                        >
                            NDI
                        </button>
                        <button
                            onClick={() => setShowOutputManager(true)}
                            title="Output Manager"
                            style={{
                                background: 'transparent',
                                border: '1px solid rgba(255,255,255,0.1)',
                                color: '#94a3b8',
                                padding: '2px 4px',
                                borderRadius: 3,
                                cursor: 'pointer',
                                display: 'flex',
                                alignItems: 'center',
                                gap: 2
                            }}
                        >
                            <Settings size={10} />
                        </button>
                        <div style={{ position: 'relative' }}>
                            <button
                                onClick={() => setShowStreamConfig(!showStreamConfig)}
                                title="Configure Stream Targets"
                                style={{
                                    background: streamTargets.some(t => t.enabled) ? 'rgba(239, 68, 68, 0.2)' : 'transparent',
                                    border: `1px solid ${streamTargets.some(t => t.enabled) ? '#ef4444' : 'rgba(255,255,255,0.1)'}`,
                                    color: streamTargets.some(t => t.enabled) ? '#ef4444' : '#64748b',
                                    fontSize: 8,
                                    fontWeight: 800,
                                    padding: '2px 4px',
                                    borderRadius: 3,
                                    cursor: 'pointer',
                                    display: 'flex',
                                    alignItems: 'center',
                                    gap: 2
                                }}
                            >
                                STR <Plus size={8} />
                            </button>
                            {/* Stream Targets Popover */}
                            {showStreamConfig && (
                                <div style={{
                                    position: 'absolute',
                                    top: '100%',
                                    right: 0,
                                    marginTop: 4,
                                    background: '#18181b',
                                    border: '1px solid rgba(255,255,255,0.1)',
                                    borderRadius: 6,
                                    padding: 8,
                                    width: 220,
                                    zIndex: 50,
                                    boxShadow: '0 10px 25px rgba(0,0,0,0.5)',
                                    display: 'flex',
                                    flexDirection: 'column',
                                    gap: 6
                                }}>
                                    <div style={{ fontSize: 10, fontWeight: 600, color: '#f1f5f9', borderBottom: '1px solid rgba(255,255,255,0.1)', paddingBottom: 4 }}>
                                        Stream Targets
                                    </div>
                                    {streamTargets.map((t, idx) => {
                                        const preset = presets.find(p => p.id === t.presetId);
                                        const displayUrl = preset ? `${preset.protocol}://${preset.server || preset.address || ''}` : t.url;
                                        return (
                                            <div key={t.id} style={{ display: 'flex', gap: 4, alignItems: 'center' }}>
                                                <input 
                                                    type="checkbox" 
                                                    checked={t.enabled}
                                                    onChange={e => {
                                                        const copy = [...streamTargets];
                                                        copy[idx].enabled = e.target.checked;
                                                        setStreamTargets(copy);
                                                    }}
                                                    style={{ width: 10, height: 10, accentColor: '#ef4444' }}
                                                />
                                                <span style={{ flex: 1, fontSize: 9, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: '#cbd5e1' }}>
                                                    {preset ? preset.name : displayUrl}
                                                </span>
                                                <button 
                                                    onClick={() => setStreamTargets(streamTargets.filter(st => st.id !== t.id))}
                                                    style={{ background: 'transparent', border: 'none', color: '#ef4444', cursor: 'pointer', padding: 2 }}
                                                >
                                                    <Trash2 size={10} />
                                                </button>
                                            </div>
                                        );
                                    })}
                                    
                                    {/* Stream Health Indicators */}
                                    <div style={{ display: 'flex', flexDirection: 'column', gap: 4, marginTop: 4, borderTop: '1px solid rgba(255,255,255,0.1)', paddingTop: 4 }}>
                                        {streamTargets.filter(t => t.enabled).map(t => {
                                            const preset = presets.find(p => p.id === t.presetId);
                                            const name = preset ? preset.name : (t.url || 'Stream');
                                            const state = t.state || 'ok';
                                            let color = '#ef4444'; // off
                                            let text = 'OFF';
                                            let bg = 'rgba(239,68,68,0.1)';
                                            
                                            if (state === 'ok') {
                                                color = '#10b981';
                                                text = `${t.bitrate || 5500} kbps`;
                                                bg = 'rgba(16,185,129,0.1)';
                                            } else if (state === 'dropped') {
                                                color = '#eab308';
                                                text = `${t.droppedFrames || 42} drop`;
                                                bg = 'rgba(234,179,8,0.1)';
                                            } else if (state === 'error') {
                                                color = '#ef4444';
                                                text = 'ERR retry';
                                                bg = 'rgba(239,68,68,0.2)';
                                            }

                                            return (
                                                <div key={`status-${t.id}`} style={{ display: 'flex', justifyContent: 'space-between', fontSize: 9, background: bg, color, padding: '2px 4px', borderRadius: 2 }}>
                                                    <span style={{ maxWidth: 80, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{name}</span>
                                                    <span className={state === 'error' ? 'animate-pulse' : ''}>{text}</span>
                                                </div>
                                            );
                                        })}
                                    </div>
                                    
                                    {!addingNew ? (
                                        <div style={{ display: 'flex', gap: 4, marginTop: 4 }}>
                                            <select 
                                                onChange={e => {
                                                    if (e.target.value === 'new') {
                                                        setAddingNew(true);
                                                        setNewPreset({ protocol: 'rtmp', name: 'New Preset' });
                                                    } else if (e.target.value) {
                                                        setStreamTargets([...streamTargets, { id: Math.random().toString(), presetId: e.target.value, enabled: true }]);
                                                    }
                                                    e.target.value = '';
                                                }}
                                                style={{ flex: 1, background: 'rgba(0,0,0,0.3)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                            >
                                                <option value="">Select / Create...</option>
                                                <option value="new">+ Create New Target</option>
                                                {presets.map(p => <option key={p.id} value={p.id}>{p.name} ({p.protocol.toUpperCase()})</option>)}
                                            </select>
                                        </div>
                                    ) : (
                                        <div style={{ display: 'flex', flexDirection: 'column', gap: 4, background: 'rgba(255,255,255,0.02)', padding: 6, borderRadius: 4, border: '1px solid rgba(255,255,255,0.05)' }}>
                                            <input 
                                                value={newPreset.name || ''} 
                                                onChange={e => setNewPreset({...newPreset, name: e.target.value})}
                                                placeholder="Preset Name"
                                                style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                            />
                                            <select 
                                                value={newPreset.protocol || 'rtmp'}
                                                onChange={e => setNewPreset({...newPreset, protocol: e.target.value as StreamProtocol})}
                                                style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                            >
                                                <option value="rtmp">RTMP</option>
                                                <option value="srt">SRT</option>
                                                <option value="udp">UDP</option>
                                                <option value="tcp">TCP</option>
                                                <option value="hls">HLS</option>
                                                <option value="rtsp">RTSP</option>
                                                <option value="youtube">YouTube</option>
                                                <option value="facebook">Facebook</option>
                                                <option value="roku">Roku</option>
                                                <option value=".plus/.fun">.plus/.fun</option>
                                            </select>
                                            <input 
                                                value={newPreset.encoder || ''} 
                                                onChange={e => setNewPreset({...newPreset, encoder: e.target.value})}
                                                placeholder="Encoder (e.g. h264_nvenc)"
                                                style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                            />
                                            <div style={{ display: 'flex', gap: 4 }}>
                                                <input 
                                                    value={newPreset.bitrate || ''} 
                                                    onChange={e => setNewPreset({...newPreset, bitrate: e.target.value})}
                                                    placeholder="Bitrate (5500)"
                                                    style={{ flex: 1, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                />
                                                <input 
                                                    value={newPreset.framerate || ''} 
                                                    onChange={e => setNewPreset({...newPreset, framerate: e.target.value})}
                                                    placeholder="FPS (60)"
                                                    style={{ flex: 1, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                />
                                            </div>
                                            
                                            {/* Protocol specifics */}
                                            {['srt', 'udp', 'tcp'].includes(newPreset.protocol || '') && (
                                                <div style={{ display: 'flex', gap: 4 }}>
                                                    <input 
                                                        value={newPreset.address || ''} 
                                                        onChange={e => setNewPreset({...newPreset, address: e.target.value})}
                                                        placeholder="Address"
                                                        style={{ flex: 2, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                    <input 
                                                        value={newPreset.port || ''} 
                                                        onChange={e => setNewPreset({...newPreset, port: e.target.value})}
                                                        placeholder="Port"
                                                        style={{ flex: 1, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                </div>
                                            )}
                                            {newPreset.protocol === 'srt' && (
                                                <select 
                                                    value={newPreset.mode || 'caller'}
                                                    onChange={e => setNewPreset({...newPreset, mode: e.target.value as any})}
                                                    style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                >
                                                    <option value="caller">Caller</option>
                                                    <option value="listener">Listener</option>
                                                </select>
                                            )}
                                            {['rtmp', 'youtube', 'facebook', '.plus/.fun'].includes(newPreset.protocol || '') && (
                                                <>
                                                    <input 
                                                        value={newPreset.server || ''} 
                                                        onChange={e => setNewPreset({...newPreset, server: e.target.value})}
                                                        placeholder="Server URL"
                                                        style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                    <input 
                                                        type="password"
                                                        value={newPreset.key || ''} 
                                                        onChange={e => setNewPreset({...newPreset, key: e.target.value})}
                                                        placeholder="Stream Key"
                                                        style={{ background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                </>
                                            )}
                                            {(!['srt', 'udp', 'tcp'].includes(newPreset.protocol || '')) && (
                                                <div style={{ display: 'flex', gap: 4 }}>
                                                    <input 
                                                        value={newPreset.tuning || ''} 
                                                        onChange={e => setNewPreset({...newPreset, tuning: e.target.value})}
                                                        placeholder="Tuning (zerolatency)"
                                                        style={{ flex: 1, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                    <input 
                                                        value={newPreset.profile || ''} 
                                                        onChange={e => setNewPreset({...newPreset, profile: e.target.value})}
                                                        placeholder="Profile (high)"
                                                        style={{ flex: 1, background: 'rgba(0,0,0,0.5)', border: '1px solid rgba(255,255,255,0.1)', color: '#fff', fontSize: 9, padding: '2px 4px', borderRadius: 2 }}
                                                    />
                                                </div>
                                            )}
                                            
                                            <div style={{ display: 'flex', gap: 4, marginTop: 4 }}>
                                                <button 
                                                    onClick={() => {
                                                        const p: StreamTargetPreset = { ...newPreset, id: Math.random().toString() } as StreamTargetPreset;
                                                        const newPresets = [...presets, p];
                                                        setPresets(newPresets);
                                                        localStorage.setItem('playplay_stream_presets', JSON.stringify(newPresets));
                                                        setStreamTargets([...streamTargets, { id: Math.random().toString(), presetId: p.id, enabled: true }]);
                                                        setAddingNew(false);
                                                    }}
                                                    style={{ flex: 1, background: '#3b82f6', border: 'none', color: '#fff', fontSize: 9, padding: '4px', borderRadius: 2, cursor: 'pointer', fontWeight: 600 }}
                                                >
                                                    Save
                                                </button>
                                                <button 
                                                    onClick={() => setAddingNew(false)}
                                                    style={{ flex: 1, background: 'rgba(255,255,255,0.1)', border: 'none', color: '#cbd5e1', fontSize: 9, padding: '4px', borderRadius: 2, cursor: 'pointer' }}
                                                >
                                                    Cancel
                                                </button>
                                            </div>
                                        </div>
                                    )}
                                </div>
                            )}
                        </div>
                    </div>
                    <div style={{
                        width: 7,
                        height: 7,
                        borderRadius: '50%',
                        backgroundColor: channel.isPlaying ? '#10b981' : '#374151',
                        boxShadow: channel.isPlaying ? '0 0 6px rgba(16,185,129,0.8)' : 'none',
                        flexShrink: 0,
                    }} />
                </div>
            </div>

            {/* Now playing */}
            <div style={{ padding: compact ? '4px 8px' : '6px 10px', borderBottom: '1px solid rgba(255,255,255,0.04)', minHeight: compact ? 32 : 40 }}>
                <div style={{ fontSize: compact ? 10 : 11, fontWeight: 600, color: '#f1f5f9', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                    {currentTitle}
                </div>
                {nextTitle && !compact && (
                    <div style={{ fontSize: 9, color: '#475569', marginTop: 2, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        → {nextTitle}
                    </div>
                )}
            </div>

            {/* VU Meters + Volume controls */}
            <div style={{ display: 'flex', gap: compact ? 8 : 16, padding: compact ? '6px 8px' : '10px', flex: 1, justifyContent: 'space-between' }}>
                {/* OTA section (Left) */}
                <div style={{ display: 'flex', gap: compact ? 6 : 10, alignItems: 'center' }}>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 4, height: compact ? 50 : 80, alignItems: 'center' }}>
                        <span style={{ fontSize: 9, color: '#475569', fontFamily: 'monospace' }}>
                            {Math.round(otaVolume * 100)}%
                        </span>
                        <input
                            type="range"
                            min={0}
                            max={1}
                            step={0.01}
                            value={otaVolume}
                            onChange={e => handleOtaVolume(parseFloat(e.target.value))}
                            style={{ 
                                appearance: 'slider-vertical' as any, 
                                width: '12px', 
                                height: '100%', 
                                accentColor: '#10b981', 
                                cursor: 'pointer',
                                WebkitAppearance: 'slider-vertical' as any
                            }}
                        />
                        <button
                            onClick={() => setNormEnabled(n => !n)}
                            title={normEnabled ? 'Normalization ON — targeting 75-90%' : 'Normalization OFF'}
                            style={{
                                fontSize: 8,
                                fontWeight: 700,
                                padding: '1px 3px',
                                background: normEnabled ? 'rgba(16,185,129,0.2)' : 'rgba(255,255,255,0.06)',
                                border: `1px solid ${normEnabled ? '#10b981' : 'rgba(255,255,255,0.1)'}`,
                                borderRadius: 3,
                                color: normEnabled ? '#10b981' : '#64748b',
                                cursor: 'pointer',
                                letterSpacing: '0.06em',
                            }}
                        >
                            NORM
                        </button>
                    </div>
                    <VuMeter
                        label="OTA"
                        level={otaLevelRef.current}
                        peak={otaPeakRef.current}
                        targetMin={normEnabled ? 0.75 : undefined}
                        targetMax={normEnabled ? 0.90 : undefined}
                        color="#10b981"
                        height={compact ? 50 : 80}
                    />
                </div>

                {/* MONITOR section (Right) */}
                <div style={{ display: 'flex', gap: compact ? 6 : 10, alignItems: 'center' }}>
                    <VuMeter
                        label="MON"
                        level={monEnabled ? monLevelRef.current : 0}
                        peak={monEnabled ? monPeakRef.current : 0}
                        color="#8b5cf6"
                        height={compact ? 50 : 80}
                    />
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 4, height: compact ? 50 : 80, alignItems: 'center' }}>
                        <span style={{ fontSize: 9, color: '#475569', fontFamily: 'monospace' }}>
                            {monEnabled ? `${Math.round(monVolume * 100)}%` : 'OFF'}
                        </span>
                        <input
                            type="range"
                            min={0}
                            max={1}
                            step={0.01}
                            value={monVolume}
                            onChange={e => setMonVolume(parseFloat(e.target.value))}
                            disabled={!monEnabled}
                            style={{ 
                                appearance: 'slider-vertical' as any, 
                                width: '12px', 
                                height: '100%', 
                                accentColor: '#8b5cf6', 
                                cursor: monEnabled ? 'pointer' : 'default', 
                                opacity: monEnabled ? 1 : 0.4,
                                WebkitAppearance: 'slider-vertical' as any
                            }}
                        />
                        <button
                            onClick={() => setMonEnabled(m => !m)}
                            style={{
                                fontSize: 8,
                                fontWeight: 700,
                                padding: '1px 3px',
                                background: monEnabled ? 'rgba(139,92,246,0.2)' : 'rgba(255,255,255,0.06)',
                                border: `1px solid ${monEnabled ? '#8b5cf6' : 'rgba(255,255,255,0.1)'}`,
                                borderRadius: 3,
                                color: monEnabled ? '#8b5cf6' : '#64748b',
                                cursor: 'pointer',
                                letterSpacing: '0.06em',
                            }}
                        >
                            {monEnabled ? 'ON' : 'MON'}
                        </button>
                    </div>
                </div>
            </div>

            {/* Output Manager Modal */}
            {showOutputManager && casparCh !== null && (
                <OutputManager
                    channel={channel}
                    casparCh={casparCh}
                    onClose={() => setShowOutputManager(false)}
                />
            )}
        </div>
    );
}

// ── Main Page ─────────────────────────────────────────────────────────────────

type LayoutMode = 1 | 4 | 8;

function readChannels(): PlayoutChannel[] {
    try {
        const raw = localStorage.getItem('multiChannelState');
        if (!raw) return [];
        const state: MultiChannelState = JSON.parse(raw);
        return state.channels ?? [];
    } catch {
        return [];
    }
}

export function MultiviewerPage() {
    const [layout, setLayout] = useState<LayoutMode>(4);
    const [channels, setChannels] = useState<PlayoutChannel[]>(readChannels);

    // Poll localStorage for channel updates every 2s (multiviewer is a separate window)
    useEffect(() => {
        const id = setInterval(() => setChannels(readChannels()), 2000);
        return () => clearInterval(id);
    }, []);

    // Channels to show (limited by layout)
    const visible = useMemo(() => {
        if (layout === 1) return channels.slice(0, 1);
        if (layout === 4) return channels.slice(0, 4);
        return channels.slice(0, 8);
    }, [channels, layout]);

    // Grid config per layout
    const gridConfig: { cols: string; rows: string } = useMemo(() => {
        if (layout === 1) return { cols: '1fr', rows: '1fr' };
        if (layout === 4) return { cols: '1fr 1fr', rows: '1fr 1fr' };
        return { cols: '1fr 1fr 1fr 1fr', rows: '1fr 1fr' };
    }, [layout]);

    const compact = layout === 8;

    return (
        <div style={{
            width: '100vw',
            height: '100vh',
            display: 'flex',
            flexDirection: 'column',
            background: '#09090b',
            color: '#e2e8f0',
            fontFamily: 'system-ui, -apple-system, sans-serif',
            overflow: 'hidden',
        }}>
            {/* Toolbar */}
            <div style={{
                display: 'flex',
                alignItems: 'center',
                gap: 8,
                padding: '6px 12px',
                background: 'rgba(255,255,255,0.03)',
                borderBottom: '1px solid rgba(255,255,255,0.07)',
                flexShrink: 0,
            }}>
                <span style={{ fontSize: 11, fontWeight: 800, letterSpacing: '0.15em', color: '#3b82f6', marginRight: 4 }}>
                    MULTIVIEWER
                </span>
                {([1, 4, 8] as LayoutMode[]).map(n => (
                    <button
                        key={n}
                        onClick={() => setLayout(n)}
                        style={{
                            padding: '3px 10px',
                            fontSize: 11,
                            fontWeight: 700,
                            background: layout === n ? '#3b82f6' : 'rgba(255,255,255,0.06)',
                            border: 'none',
                            borderRadius: 4,
                            color: layout === n ? '#fff' : '#64748b',
                            cursor: 'pointer',
                            letterSpacing: '0.05em',
                        }}
                    >
                        {n}×
                    </button>
                ))}
                <span style={{ marginLeft: 'auto', fontSize: 9, color: '#334155' }}>
                    {channels.length} channels · refreshed every 2s
                </span>
            </div>

            {/* Channel grid */}
            <div style={{
                flex: 1,
                display: 'grid',
                gridTemplateColumns: gridConfig.cols,
                gridTemplateRows: gridConfig.rows,
                gap: 6,
                padding: 8,
                overflow: 'hidden',
                minHeight: 0,
            }}>
                {visible.length === 0 ? (
                    <div style={{ gridColumn: '1 / -1', display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#334155', fontSize: 13 }}>
                        No channels configured. Add channels in the main playout window.
                    </div>
                ) : (
                    visible.map(ch => (
                        <ChannelCell key={ch.id} channel={ch} compact={compact} />
                    ))
                )}
                {/* Placeholder cells if fewer channels than layout slots */}
                {Array.from({ length: Math.max(0, layout - visible.length) }, (_, i) => (
                    <div
                        key={`placeholder-${i}`}
                        style={{
                            border: '1px dashed rgba(255,255,255,0.05)',
                            borderRadius: 8,
                            display: 'flex',
                            alignItems: 'center',
                            justifyContent: 'center',
                            color: '#1e293b',
                            fontSize: 10,
                        }}
                    >
                        empty
                    </div>
                ))}
            </div>
        </div>
    );
}
