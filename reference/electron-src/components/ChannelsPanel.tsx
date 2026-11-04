import React, { useState, useCallback } from 'react';
import { Tv2, Wifi, Radio, Rss, ChevronDown, ChevronUp, RotateCcw, CheckCircle } from 'lucide-react';
import {
    ChannelOutputConfig,
    loadChannelConfigs,
    saveChannelConfigs,
} from '../lib/channelConfig';
import {
    getCasparVideoModePreference,
} from '../lib/casparVideoMode';
import { getCasparMediaPathForCurrentSettings } from '../lib/casparRamdisk';

interface Props {
    onApplied?: () => void;
}

const PROTOCOL_COLORS: Record<string, string> = {
    ndi:  '#22c55e',
    srt:  '#a855f7',
    rtmp: '#ef4444',
    hls:  '#f59e0b',
};

const codecs = ['libx264', 'h264_nvenc', 'h264_amf', 'hevc_nvenc', 'libx265'];

function Toggle({ on, onToggle, label }: { on: boolean; onToggle: () => void; label?: string }) {
    return (
        <button
            type="button"
            onClick={onToggle}
            title={label}
            style={{
                display: 'inline-flex', alignItems: 'center', gap: '6px',
                padding: '3px 10px', borderRadius: '999px', border: 'none', cursor: 'pointer',
                fontSize: '0.72rem', fontWeight: 700, letterSpacing: '0.05em',
                background: on ? 'rgba(34,197,94,0.18)' : 'rgba(255,255,255,0.06)',
                color: on ? '#22c55e' : 'rgba(255,255,255,0.35)',
                transition: 'all 0.15s',
            }}
        >
            <span style={{ width: 8, height: 8, borderRadius: '50%', background: 'currentColor', flexShrink: 0 }} />
            {label ?? (on ? 'ON' : 'OFF')}
        </button>
    );
}

export function ChannelsPanel({ onApplied }: Props) {
    const [configs, setConfigs] = useState<ChannelOutputConfig[]>(loadChannelConfigs);
    const [expanded, setExpanded] = useState<Set<string>>(new Set(['ch-1']));
    const [applyStatus, setApplyStatus] = useState<'idle' | 'ok' | 'err'>('idle');

    const update = useCallback((id: string, patch: Partial<ChannelOutputConfig>) => {
        setConfigs(prev => {
            const next = prev.map(c => c.id === id ? { ...c, ...patch } : c);
            saveChannelConfigs(next);
            return next;
        });
    }, []);

    const updateNested = useCallback((id: string, key: keyof ChannelOutputConfig, patch: Record<string, unknown>) => {
        setConfigs(prev => {
            const next = prev.map(c => c.id === id ? { ...c, [key]: { ...(c[key] as object), ...patch } } : c);
            saveChannelConfigs(next);
            return next;
        });
    }, []);

    const toggleExpand = (id: string) => {
        setExpanded(prev => {
            const s = new Set(prev);
            if (s.has(id)) s.delete(id); else s.add(id);
            return s;
        });
    };

    const applyToCaspar = async () => {
        const api = (window as any).electronAPI;
        if (!api?.casparConfigure) { setApplyStatus('err'); return; }
        const enabledCount = configs.filter(c => c.enabled).length;
        const casparDir = (() => {
            try { return localStorage.getItem('casparDir') || 'C:/casparcg'; }
            catch { return ''; }
        })();
        try {
            const casparMediaPath = getCasparMediaPathForCurrentSettings();
            await api.casparConfigure({
                casparDir,
                channels: Math.max(1, enabledCount),
                videoMode: getCasparVideoModePreference(),
                channelConfigs: configs,
                ...(casparMediaPath ? { casparMediaPath } : {}),
            });
            setApplyStatus('ok');
            onApplied?.();
        } catch (e) {
            console.error('[ChannelsPanel] casparConfigure failed', e);
            setApplyStatus('err');
        }
        setTimeout(() => setApplyStatus('idle'), 3000);
    };

    const row = (c: ChannelOutputConfig) => {
        const open = expanded.has(c.id);
        const isBg = !c.enabled;

        return (
            <div key={c.id} style={{
                border: '1px solid',
                borderColor: c.enabled ? 'rgba(255,255,255,0.1)' : 'rgba(255,255,255,0.05)',
                borderRadius: 8,
                marginBottom: 6,
                background: isBg ? 'rgba(255,255,255,0.02)' : 'rgba(255,255,255,0.04)',
                opacity: isBg ? 0.6 : 1,
                overflow: 'hidden',
                transition: 'opacity 0.2s',
            }}>
                {/* Header row */}
                <div style={{
                    display: 'flex', alignItems: 'center', gap: 10,
                    padding: '8px 12px', cursor: 'pointer', userSelect: 'none',
                }}
                    onClick={() => toggleExpand(c.id)}
                >
                    <span style={{
                        width: 28, height: 28, borderRadius: 6, display: 'flex',
                        alignItems: 'center', justifyContent: 'center',
                        background: c.enabled ? 'rgba(99,102,241,0.25)' : 'rgba(255,255,255,0.06)',
                        color: c.enabled ? '#818cf8' : 'rgba(255,255,255,0.3)',
                        fontWeight: 800, fontSize: '0.7rem',
                    }}>
                        {c.number}
                    </span>
                    <input
                        value={c.name}
                        onChange={e => { e.stopPropagation(); update(c.id, { name: e.target.value }); }}
                        onClick={e => e.stopPropagation()}
                        style={{
                            flex: 1, background: 'transparent', border: 'none', color: 'var(--text-primary)',
                            fontWeight: 600, fontSize: '0.85rem', outline: 'none',
                        }}
                    />
                    {/* Output badges */}
                    <div style={{ display: 'flex', gap: 4 }}>
                        {c.ndi.enabled  && <span style={{ color: PROTOCOL_COLORS.ndi,  fontSize: '0.65rem', fontWeight: 700 }}>NDI</span>}
                        {c.srt.enabled  && <span style={{ color: PROTOCOL_COLORS.srt,  fontSize: '0.65rem', fontWeight: 700 }}>SRT</span>}
                        {c.rtmp.enabled && <span style={{ color: PROTOCOL_COLORS.rtmp, fontSize: '0.65rem', fontWeight: 700 }}>RTMP</span>}
                        {c.hls.enabled  && <span style={{ color: PROTOCOL_COLORS.hls,  fontSize: '0.65rem', fontWeight: 700 }}>HLS</span>}
                    </div>
                    <Toggle on={c.enabled} onToggle={() => update(c.id, { enabled: !c.enabled })} label={c.enabled ? 'ACTIVE' : 'OFFLINE'} />
                    {open ? <ChevronUp size={14} style={{ opacity: 0.4 }} /> : <ChevronDown size={14} style={{ opacity: 0.4 }} />}
                </div>

                {/* Expanded detail */}
                {open && (
                    <div style={{ padding: '12px 16px 16px', borderTop: '1px solid rgba(255,255,255,0.06)', display: 'flex', flexDirection: 'column', gap: 12 }}>
                        {/* CasparCG channel */}
                        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                            <Tv2 size={13} style={{ color: '#818cf8' }} />
                            <span style={{ fontSize: '0.75rem', color: 'var(--text-secondary)', width: 110 }}>CasparCG Channel</span>
                            <select
                                value={c.casparChannel}
                                onChange={e => update(c.id, { casparChannel: Number(e.target.value) })}
                                style={selectStyle}
                            >
                                {[1,2,3,4].map(n => <option key={n} value={n}>CH {n}</option>)}
                            </select>
                        </div>

                        {/* NDI */}
                        <Section color={PROTOCOL_COLORS.ndi} icon={<Radio size={12} />} label="NDI Output">
                            <Toggle on={c.ndi.enabled} onToggle={() => updateNested(c.id, 'ndi', { enabled: !c.ndi.enabled })} />
                            <LabeledInput label="Name" value={c.ndi.name} onChange={v => updateNested(c.id, 'ndi', { name: v })} placeholder="PlayPlay CH1" />
                        </Section>

                        {/* SRT */}
                        <Section color={PROTOCOL_COLORS.srt} icon={<Wifi size={12} />} label="SRT Output">
                            <Toggle on={c.srt.enabled} onToggle={() => updateNested(c.id, 'srt', { enabled: !c.srt.enabled })} />
                            <LabeledInput label="URL" value={c.srt.url} onChange={v => updateNested(c.id, 'srt', { url: v })} placeholder="srt://0.0.0.0:9001" wide />
                            <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginTop: 6 }}>
                                <LabeledInput label="Bitrate (kbps)" value={String(c.srt.bitrate)} onChange={v => updateNested(c.id, 'srt', { bitrate: Number(v) || 4000 })} placeholder="4000" type="number" />
                                <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
                                    <span style={{ fontSize: '0.65rem', color: 'var(--text-secondary)' }}>Codec</span>
                                    <select value={c.srt.codec} onChange={e => updateNested(c.id, 'srt', { codec: e.target.value })} style={selectStyle}>
                                        {codecs.map(cd => <option key={cd} value={cd}>{cd}</option>)}
                                    </select>
                                </div>
                            </div>
                        </Section>

                        {/* RTMP */}
                        <Section color={PROTOCOL_COLORS.rtmp} icon={<Rss size={12} />} label="RTMP Output">
                            <Toggle on={c.rtmp.enabled} onToggle={() => updateNested(c.id, 'rtmp', { enabled: !c.rtmp.enabled })} />
                            <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginTop: 6 }}>
                                <LabeledInput label="URL" value={c.rtmp.url} onChange={v => updateNested(c.id, 'rtmp', { url: v })} placeholder="rtmp://a.rtmp.youtube.com/live2/" wide />
                                <LabeledInput label="Stream Key" value={c.rtmp.key} onChange={v => updateNested(c.id, 'rtmp', { key: v })} placeholder="xxxx-xxxx-xxxx" />
                            </div>
                        </Section>

                        {/* HLS */}
                        <Section color={PROTOCOL_COLORS.hls} icon={<Wifi size={12} />} label="HLS / HTTP Output">
                            <Toggle on={c.hls.enabled} onToggle={() => updateNested(c.id, 'hls', { enabled: !c.hls.enabled })} />
                            <LabeledInput label="Manifest URL" value={c.hls.url} onChange={v => updateNested(c.id, 'hls', { url: v })} placeholder="http://localhost:8080/stream.m3u8" wide />
                        </Section>
                    </div>
                )}
            </div>
        );
    };

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 0, padding: '0 4px' }}>
            {/* Toolbar */}
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 12 }}>
                <span style={{ fontSize: '0.8rem', color: 'var(--text-secondary)' }}>
                    {configs.filter(c => c.enabled).length} of 8 channels active
                </span>
                <div style={{ display: 'flex', gap: 8 }}>
                    <button type="button" onClick={() => { setConfigs(loadChannelConfigs()); }} style={btnSecStyle} title="Reload from storage">
                        <RotateCcw size={13} />
                    </button>
                    <button type="button" onClick={applyToCaspar} style={{
                        ...btnPriStyle,
                        background: applyStatus === 'ok' ? 'rgba(34,197,94,0.2)' : applyStatus === 'err' ? 'rgba(239,68,68,0.2)' : undefined,
                    }}>
                        {applyStatus === 'ok' ? <><CheckCircle size={13} /> Applied</> : applyStatus === 'err' ? 'Failed' : 'Apply to CasparCG'}
                    </button>
                </div>
            </div>

            {/* 8 channel rows */}
            {configs.map(row)}
        </div>
    );
}

function Section({ color, icon, label, children }: { color: string; icon: React.ReactNode; label: string; children: React.ReactNode }) {
    return (
        <div style={{ background: 'rgba(255,255,255,0.03)', borderRadius: 6, padding: '10px 12px', border: `1px solid ${color}22` }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
                <span style={{ color }}>{icon}</span>
                <span style={{ fontSize: '0.72rem', fontWeight: 700, color, letterSpacing: '0.06em' }}>{label}</span>
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>{children}</div>
        </div>
    );
}

function LabeledInput({ label, value, onChange, placeholder, wide, type }: {
    label: string; value: string; onChange: (v: string) => void;
    placeholder?: string; wide?: boolean; type?: string;
}) {
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 2, flex: wide ? 1 : undefined }}>
            <span style={{ fontSize: '0.65rem', color: 'var(--text-secondary)' }}>{label}</span>
            <input
                type={type || 'text'}
                value={value}
                onChange={e => onChange(e.target.value)}
                placeholder={placeholder}
                style={{
                    background: 'rgba(0,0,0,0.3)', border: '1px solid rgba(255,255,255,0.1)',
                    borderRadius: 4, color: 'var(--text-primary)', fontSize: '0.75rem',
                    padding: '4px 8px', outline: 'none', width: wide ? '100%' : 120,
                    fontFamily: 'monospace',
                }}
            />
        </div>
    );
}

const selectStyle: React.CSSProperties = {
    background: 'rgba(0,0,0,0.3)', border: '1px solid rgba(255,255,255,0.1)',
    borderRadius: 4, color: 'var(--text-primary)', fontSize: '0.75rem',
    padding: '4px 8px', outline: 'none',
};

const btnSecStyle: React.CSSProperties = {
    display: 'flex', alignItems: 'center', gap: 6,
    padding: '5px 10px', borderRadius: 6, border: '1px solid rgba(255,255,255,0.12)',
    background: 'rgba(255,255,255,0.05)', color: 'var(--text-secondary)',
    fontSize: '0.75rem', cursor: 'pointer',
};

const btnPriStyle: React.CSSProperties = {
    display: 'flex', alignItems: 'center', gap: 6,
    padding: '5px 14px', borderRadius: 6, border: '1px solid rgba(99,102,241,0.4)',
    background: 'rgba(99,102,241,0.15)', color: '#a5b4fc',
    fontSize: '0.75rem', cursor: 'pointer', fontWeight: 600,
};
