/**
 * Asset Inspector — full analysis panel for a media item.
 * Shows: technical metadata, thumbnails, waveform/loudness,
 *        broadcast warnings, transcript, and LLM AI context.
 *
 * Opens via MediaLibrary context menu → "Inspect / Analyze".
 */
import React, { useState, useEffect, useCallback } from 'react';
import { MediaItem } from '../lib/types';
import {
    AssetAnalysis, FFmpegFeatures,
    analyzeAsset, getAnalysisResult, pollUntilDone, runLlmContext, detectFFmpegFeatures,
} from '../lib/contentAnalysis';
import {
    X, RefreshCw, Cpu, Film, Volume2, AlertTriangle, CheckCircle,
    FileText, Sparkles, Clock, Layers, BarChart2, Info, Wand2,
} from 'lucide-react';

interface Props {
    item: MediaItem;
    onClose: () => void;
}

type Tab = 'overview' | 'technical' | 'warnings' | 'transcript' | 'context';

const TAB_LABELS: Record<Tab, string> = {
    overview:   'Overview',
    technical:  'Technical',
    warnings:   'Warnings',
    transcript: 'Transcript',
    context:    'AI Context',
};

export function AssetInspector({ item, onClose }: Props) {
    const [tab, setTab]               = useState<Tab>('overview');
    const [analysis, setAnalysis]     = useState<AssetAnalysis | null>(null);
    const [features, setFeatures]     = useState<FFmpegFeatures | null>(null);
    const [status, setStatus]         = useState<'idle' | 'queued' | 'running' | 'done' | 'failed'>('idle');
    const [llmRunning, setLlmRunning] = useState(false);
    const [thumbIdx, setThumbIdx]     = useState(0);

    const filePath = item.filePath || '';

    // Load cached result on mount
    useEffect(() => {
        if (!filePath) return;
        detectFFmpegFeatures().then(f => setFeatures(f));
        getAnalysisResult(filePath).then(r => {
            if (r) { setAnalysis(r); setStatus('done'); }
        });
    }, [filePath]);

    const startAnalysis = useCallback(async (force = false) => {
        if (!filePath) return;
        setStatus('queued');
        await analyzeAsset(filePath, { force });
        setStatus('running');
        const result = await pollUntilDone(filePath, s => {
            setStatus(s === 'complete' ? 'done' : s === 'failed' ? 'failed' : 'running');
        });
        if (result) { setAnalysis(result); setStatus('done'); }
        else setStatus('failed');
    }, [filePath]);

    const startLlm = useCallback(async () => {
        if (!analysis) return;
        setLlmRunning(true);
        const ctx = await runLlmContext(analysis);
        if (ctx) setAnalysis(prev => prev ? { ...prev, context: ctx } : null);
        setLlmRunning(false);
    }, [analysis]);

    const thumbs = analysis?.video_analysis?.thumbnails || [];
    const warnings = buildWarnings(analysis);

    return (
        <div style={{
            position: 'fixed', inset: 0, zIndex: 1000,
            background: 'rgba(0,0,0,0.85)', display: 'flex', alignItems: 'center', justifyContent: 'center',
        }} onClick={onClose}>
            <div style={{
                width: '90vw', maxWidth: 900, maxHeight: '88vh',
                background: 'var(--surface, #1a1a2e)', borderRadius: 12,
                border: '1px solid rgba(255,255,255,0.1)',
                display: 'flex', flexDirection: 'column', overflow: 'hidden',
                boxShadow: '0 24px 80px rgba(0,0,0,0.8)',
            }} onClick={e => e.stopPropagation()}>

                {/* Header */}
                <div style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '14px 18px', borderBottom: '1px solid rgba(255,255,255,0.08)' }}>
                    <Film size={16} style={{ color: '#818cf8', flexShrink: 0 }} />
                    <div style={{ flex: 1, minWidth: 0 }}>
                        <div style={{ fontWeight: 700, fontSize: '0.9rem', color: 'var(--text-primary)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{item.title || item.filename || 'Asset'}</div>
                        <div style={{ fontSize: '0.7rem', color: 'var(--text-secondary)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{filePath}</div>
                    </div>
                    <StatusBadge status={status} />
                    <div style={{ display: 'flex', gap: 6 }}>
                        {status !== 'running' && status !== 'queued' && (
                            <ActionBtn icon={<RefreshCw size={13} />} label={status === 'done' ? 'Re-analyze' : 'Analyze'} onClick={() => startAnalysis(status === 'done')} />
                        )}
                        {analysis && !analysis.context.llmAnalyzedAt && (
                            <ActionBtn icon={<Sparkles size={13} />} label={llmRunning ? 'Thinking…' : 'AI Context'} onClick={startLlm} disabled={llmRunning} accent />
                        )}
                    </div>
                    <button onClick={onClose} style={{ background: 'none', border: 'none', color: 'var(--text-secondary)', cursor: 'pointer', padding: 4 }}><X size={16} /></button>
                </div>

                {/* Tabs */}
                <div style={{ display: 'flex', borderBottom: '1px solid rgba(255,255,255,0.08)', padding: '0 18px' }}>
                    {(Object.keys(TAB_LABELS) as Tab[]).map(t => (
                        <button key={t} onClick={() => setTab(t)} style={{
                            padding: '8px 14px', border: 'none', background: 'none', cursor: 'pointer',
                            fontSize: '0.75rem', fontWeight: tab === t ? 700 : 400,
                            color: tab === t ? '#a5b4fc' : 'var(--text-secondary)',
                            borderBottom: tab === t ? '2px solid #818cf8' : '2px solid transparent',
                            display: 'flex', alignItems: 'center', gap: 5,
                        }}>
                            {t === 'warnings' && warnings.length > 0 && <span style={{ color: '#f59e0b' }}><AlertTriangle size={11} /></span>}
                            {TAB_LABELS[t]}
                            {t === 'warnings' && warnings.length > 0 && <span style={{ background: '#f59e0b', color: '#000', borderRadius: 999, padding: '0 5px', fontSize: '0.6rem', fontWeight: 800 }}>{warnings.length}</span>}
                        </button>
                    ))}
                </div>

                {/* Body */}
                <div style={{ flex: 1, overflow: 'auto', padding: '16px 18px' }}>
                    {!analysis && status === 'idle' && (
                        <Placeholder onAnalyze={() => startAnalysis()} />
                    )}
                    {(status === 'queued' || status === 'running') && <Spinner label={status === 'queued' ? 'Analysis queued…' : 'Analyzing with FFmpeg…'} />}

                    {analysis && tab === 'overview'   && <OverviewTab   analysis={analysis} thumbs={thumbs} thumbIdx={thumbIdx} setThumbIdx={setThumbIdx} item={item} />}
                    {analysis && tab === 'technical'  && <TechnicalTab  analysis={analysis} features={features} />}
                    {analysis && tab === 'warnings'   && <WarningsTab   warnings={warnings} />}
                    {analysis && tab === 'transcript' && <TranscriptTab analysis={analysis} />}
                    {analysis && tab === 'context'    && <ContextTab    analysis={analysis} onRunLlm={startLlm} llmRunning={llmRunning} />}
                </div>
            </div>
        </div>
    );
}

// ── Sub-tabs ────────────────────────────────────────────────────────────────

function OverviewTab({ analysis, thumbs, thumbIdx, setThumbIdx, item }: any) {
    return (
        <div style={{ display: 'grid', gridTemplateColumns: '240px 1fr', gap: 16, alignItems: 'start' }}>
            {/* Thumbnail strip */}
            <div>
                {thumbs.length > 0 ? (
                    <>
                        <img
                            src={`file://${thumbs[thumbIdx]}`}
                            alt="thumbnail"
                            style={{ width: '100%', borderRadius: 6, objectFit: 'cover', background: '#000' }}
                        />
                        {thumbs.length > 1 && (
                            <div style={{ display: 'flex', gap: 4, marginTop: 6, flexWrap: 'wrap' }}>
                                {thumbs.map((t: string, i: number) => (
                                    <img key={i} src={`file://${t}`} alt="" onClick={() => setThumbIdx(i)}
                                        style={{ width: 42, height: 28, objectFit: 'cover', borderRadius: 3, cursor: 'pointer', border: i === thumbIdx ? '2px solid #818cf8' : '2px solid transparent', opacity: i === thumbIdx ? 1 : 0.6 }} />
                                ))}
                            </div>
                        )}
                    </>
                ) : (
                    <div style={{ width: '100%', height: 140, background: 'rgba(255,255,255,0.04)', borderRadius: 6, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'rgba(255,255,255,0.2)' }}>
                        <Film size={32} />
                    </div>
                )}
            </div>

            {/* Quick stats */}
            <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <StatRow label="Duration"  value={fmtDur(analysis.duration)} icon={<Clock size={12} />} />
                {analysis.video && <>
                    <StatRow label="Resolution" value={`${analysis.video.width}×${analysis.video.height}`} icon={<Layers size={12} />} />
                    <StatRow label="Frame rate" value={`${analysis.video.fps?.toFixed(2)} fps`} icon={<Film size={12} />} />
                    <StatRow label="Video codec" value={analysis.video.codec} icon={<Cpu size={12} />} />
                    <StatRow label="Scan type" value={analysis.video.scanType} icon={<BarChart2 size={12} />} />
                </>}
                {analysis.audio && <>
                    <StatRow label="Audio" value={`${analysis.audio.codec} ${analysis.audio.channels}ch`} icon={<Volume2 size={12} />} />
                    {analysis.audio.loudness.integrated !== null && (
                        <StatRow label="Loudness" value={`${analysis.audio.loudness.integrated?.toFixed(1)} LUFS`} icon={<BarChart2 size={12} />} />
                    )}
                </>}
                <StatRow label="File size" value={fmtSize(analysis.format.size)} icon={<Info size={12} />} />
                {analysis.context.contentType !== 'unknown' && (
                    <StatRow label="Content type" value={analysis.context.contentType} icon={<FileText size={12} />} />
                )}
                {analysis.context.riskFlags.length > 0 && (
                    <StatRow label="Flags" value={analysis.context.riskFlags.join(', ')} icon={<AlertTriangle size={12} />} warn />
                )}
            </div>
        </div>
    );
}

function TechnicalTab({ analysis, features }: { analysis: AssetAnalysis; features: FFmpegFeatures | null }) {
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
            {features && (
                <Section title="FFmpeg Capabilities" icon={<Cpu size={13} />}>
                    <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
                        {Object.entries(features.caps).map(([k, v]) => (
                            <span key={k} style={{ padding: '2px 8px', borderRadius: 999, fontSize: '0.65rem', fontWeight: 600,
                                background: v ? 'rgba(34,197,94,0.12)' : 'rgba(255,255,255,0.05)',
                                color: v ? '#22c55e' : 'rgba(255,255,255,0.25)' }}>
                                {k}
                            </span>
                        ))}
                    </div>
                    <p style={{ margin: '8px 0 0', fontSize: '0.7rem', color: 'var(--text-secondary)' }}>FFmpeg {features.version} — {features.ffmpegPath}</p>
                </Section>
            )}

            {analysis.video && (
                <Section title="Video Stream" icon={<Film size={13} />}>
                    <PropGrid props={{
                        Codec: analysis.video.codec,
                        Profile: analysis.video.profile,
                        Resolution: `${analysis.video.width}×${analysis.video.height}`,
                        'Frame rate': `${analysis.video.fps?.toFixed(3)} fps`,
                        'Pixel format': analysis.video.pixFmt,
                        Bitrate: fmtBr(analysis.video.bitrate),
                        'Scan type': analysis.video.scanType,
                    }} />
                </Section>
            )}

            {analysis.audio && (
                <Section title="Audio Stream" icon={<Volume2 size={13} />}>
                    <PropGrid props={{
                        Codec: analysis.audio.codec,
                        Channels: `${analysis.audio.channels} (${analysis.audio.channelLayout})`,
                        'Sample rate': `${analysis.audio.sampleRate} Hz`,
                        Bitrate: fmtBr(analysis.audio.bitrate),
                        'Integrated loudness': analysis.audio.loudness.integrated != null ? `${analysis.audio.loudness.integrated?.toFixed(1)} LUFS` : 'n/a',
                        'Loudness range': analysis.audio.loudness.lra != null ? `${analysis.audio.loudness.lra?.toFixed(1)} LU` : 'n/a',
                        'True peak': analysis.audio.loudness.truePeak != null ? `${analysis.audio.loudness.truePeak?.toFixed(1)} dBFS` : 'n/a',
                    }} />
                </Section>
            )}

            <Section title="Container" icon={<Info size={13} />}>
                <PropGrid props={{
                    Format: analysis.format.longName || analysis.format.name,
                    'File size': fmtSize(analysis.format.size),
                    'Total bitrate': fmtBr(analysis.format.bitrate),
                    'Analyzed at': analysis.analyzedAt,
                    'FFmpeg version': analysis.ffmpegVersion || 'unknown',
                    'Features used': (analysis.featuresUsed || []).join(', ') || 'basic',
                }} />
            </Section>
        </div>
    );
}

function WarningsTab({ warnings }: { warnings: Warning[] }) {
    if (!warnings.length) return (
        <div style={{ textAlign: 'center', padding: 32, color: 'var(--text-secondary)' }}>
            <CheckCircle size={32} style={{ color: '#22c55e', marginBottom: 8 }} />
            <div>No broadcast warnings detected</div>
        </div>
    );
    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            {warnings.map((w, i) => (
                <div key={i} style={{
                    background: `rgba(${w.level === 'error' ? '239,68,68' : '245,158,11'},0.08)`,
                    border: `1px solid rgba(${w.level === 'error' ? '239,68,68' : '245,158,11'},0.2)`,
                    borderRadius: 8, padding: '10px 14px',
                }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
                        <AlertTriangle size={13} style={{ color: w.level === 'error' ? '#ef4444' : '#f59e0b' }} />
                        <span style={{ fontWeight: 700, fontSize: '0.78rem', color: w.level === 'error' ? '#ef4444' : '#f59e0b' }}>{w.label}</span>
                    </div>
                    <div style={{ fontSize: '0.75rem', color: 'var(--text-secondary)' }}>{w.detail}</div>
                </div>
            ))}
        </div>
    );
}

function TranscriptTab({ analysis }: { analysis: AssetAnalysis }) {
    const tr = analysis.audio?.transcript;
    if (!tr) return (
        <div style={{ textAlign: 'center', padding: 32, color: 'var(--text-secondary)', fontSize: '0.85rem' }}>
            <FileText size={28} style={{ marginBottom: 8, opacity: 0.4 }} />
            <div>No transcript available.</div>
            <div style={{ marginTop: 8, fontSize: '0.75rem', opacity: 0.6 }}>
                Whisper transcription requires a Whisper-enabled FFmpeg build or an external STT provider.
            </div>
        </div>
    );
    return (
        <div style={{ background: 'rgba(255,255,255,0.03)', borderRadius: 8, padding: 16, lineHeight: 1.7, fontSize: '0.85rem', color: 'var(--text-primary)', whiteSpace: 'pre-wrap' }}>
            {tr}
        </div>
    );
}

function ContextTab({ analysis, onRunLlm, llmRunning }: { analysis: AssetAnalysis; onRunLlm: () => void; llmRunning: boolean }) {
    const ctx = analysis.context;
    const hasCtx = ctx.llmAnalyzedAt || ctx.summary;

    if (!hasCtx) return (
        <div style={{ textAlign: 'center', padding: 32, color: 'var(--text-secondary)' }}>
            <Wand2 size={28} style={{ marginBottom: 8, opacity: 0.4 }} />
            <div style={{ marginBottom: 16 }}>No AI context generated yet.</div>
            <button onClick={onRunLlm} disabled={llmRunning} style={{
                padding: '8px 18px', borderRadius: 8, border: '1px solid rgba(99,102,241,0.4)',
                background: 'rgba(99,102,241,0.15)', color: '#a5b4fc', cursor: 'pointer', fontWeight: 600,
            }}>
                {llmRunning ? 'Analyzing…' : 'Generate AI Context'}
            </button>
            <div style={{ marginTop: 10, fontSize: '0.7rem', opacity: 0.5 }}>
                Requires OpenRouter API key in Settings → Connections
            </div>
        </div>
    );

    return (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
            {ctx.summary && (
                <Section title="Summary" icon={<Sparkles size={13} />}>
                    <p style={{ margin: 0, lineHeight: 1.6, fontSize: '0.85rem' }}>{ctx.summary}</p>
                </Section>
            )}
            {ctx.contentType && ctx.contentType !== 'unknown' && (
                <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                    <span style={{ fontSize: '0.72rem', color: 'var(--text-secondary)' }}>Content type:</span>
                    <span style={{ background: 'rgba(99,102,241,0.15)', color: '#a5b4fc', padding: '2px 10px', borderRadius: 999, fontSize: '0.72rem', fontWeight: 700 }}>{ctx.contentType}</span>
                    {(ctx as any).confidence != null && <span style={{ fontSize: '0.68rem', color: 'rgba(255,255,255,0.3)' }}>{(ctx as any).confidence}% confidence</span>}
                </div>
            )}
            {ctx.topics?.length > 0 && (
                <Section title="Topics" icon={<FileText size={13} />}>
                    <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
                        {ctx.topics.map((t: string, i: number) => (
                            <span key={i} style={{ background: 'rgba(255,255,255,0.06)', padding: '3px 10px', borderRadius: 999, fontSize: '0.72rem' }}>{t}</span>
                        ))}
                    </div>
                </Section>
            )}
            {ctx.people?.length > 0 && (
                <Section title="People" icon={<Info size={13} />}>
                    <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
                        {ctx.people.map((p: string, i: number) => (
                            <span key={i} style={{ background: 'rgba(99,102,241,0.08)', padding: '3px 10px', borderRadius: 999, fontSize: '0.72rem' }}>{p}</span>
                        ))}
                    </div>
                </Section>
            )}
            {ctx.broadcastNotes && (
                <Section title="Broadcast Notes" icon={<AlertTriangle size={13} />}>
                    <p style={{ margin: 0, fontSize: '0.82rem', lineHeight: 1.6 }}>{ctx.broadcastNotes}</p>
                </Section>
            )}
            {ctx.adBreakSuggestions?.length > 0 && (
                <Section title="Suggested Ad Breaks" icon={<Clock size={13} />}>
                    <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
                        {ctx.adBreakSuggestions.map((t: number, i: number) => (
                            <span key={i} style={{ background: 'rgba(34,197,94,0.08)', padding: '3px 10px', borderRadius: 999, fontSize: '0.72rem', fontFamily: 'monospace' }}>{fmtDur(t)}</span>
                        ))}
                    </div>
                </Section>
            )}
            {ctx.llmAnalyzedAt && (
                <div style={{ fontSize: '0.65rem', color: 'rgba(255,255,255,0.2)', marginTop: 4 }}>
                    LLM context generated: {new Date(ctx.llmAnalyzedAt).toLocaleString()}
                    {' · '}<button onClick={onRunLlm} disabled={llmRunning} style={{ background: 'none', border: 'none', color: 'inherit', cursor: 'pointer', textDecoration: 'underline' }}>Regenerate</button>
                </div>
            )}
        </div>
    );
}

// ── Helpers ─────────────────────────────────────────────────────────────────

interface Warning { level: 'warning' | 'error'; label: string; detail: string }

function buildWarnings(a: AssetAnalysis | null): Warning[] {
    if (!a) return [];
    const w: Warning[] = [];
    a.context.riskFlags.forEach(f => {
        switch (f) {
            case 'long_silence':  w.push({ level: 'warning', label: 'Long Silence',      detail: `Extended silence detected (>5 s). Check audio.` }); break;
            case 'black_frames':  w.push({ level: 'warning', label: 'Black Frames',       detail: `Black frames detected lasting more than 2 s.` }); break;
            case 'freeze_detected': w.push({ level: 'error',   label: 'Video Freeze',     detail: `Freeze frames detected. Video may be corrupt or low quality.` }); break;
            case 'very_loud':     w.push({ level: 'error',     label: 'Very Loud Audio',  detail: `Integrated loudness above -10 LUFS — may clip or distort.` }); break;
            case 'very_quiet':    w.push({ level: 'warning',   label: 'Very Quiet Audio', detail: `Integrated loudness below -35 LUFS. Viewer may not hear audio.` }); break;
            case 'true_peak_clip': w.push({ level: 'error',    label: 'True Peak Clip',   detail: `True peak above -1 dBFS. Likely to distort on broadcast.` }); break;
        }
    });
    // EBU R128 compliance
    if (a.audio?.loudness?.integrated != null && Math.abs(a.audio.loudness.integrated - (-23)) > 1.5) {
        w.push({ level: 'warning', label: 'EBU R128 Non-Compliant', detail: `Target is -23 LUFS ±1.5. Measured: ${a.audio.loudness.integrated.toFixed(1)} LUFS.` });
    }
    return w;
}

function fmtDur(s: number) {
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = Math.floor(s % 60);
    return h ? `${h}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}` : `${m}:${String(sec).padStart(2,'0')}`;
}
function fmtSize(b: number) {
    if (!b) return 'n/a';
    if (b > 1e9) return `${(b/1e9).toFixed(2)} GB`;
    return `${(b/1e6).toFixed(1)} MB`;
}
function fmtBr(b: number) {
    if (!b) return 'n/a';
    if (b > 1e6) return `${(b/1e6).toFixed(2)} Mbps`;
    return `${(b/1e3).toFixed(0)} kbps`;
}

function StatRow({ label, value, icon, warn }: { label: string; value: any; icon?: React.ReactNode; warn?: boolean }) {
    return (
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
            {icon && <span style={{ color: 'var(--text-secondary)', flexShrink: 0 }}>{icon}</span>}
            <span style={{ fontSize: '0.72rem', color: 'var(--text-secondary)', width: 100, flexShrink: 0 }}>{label}</span>
            <span style={{ fontSize: '0.8rem', color: warn ? '#f59e0b' : 'var(--text-primary)', fontWeight: 500 }}>{String(value ?? 'n/a')}</span>
        </div>
    );
}

function PropGrid({ props }: { props: Record<string, any> }) {
    return (
        <div style={{ display: 'grid', gridTemplateColumns: '160px 1fr', gap: '4px 12px' }}>
            {Object.entries(props).map(([k, v]) => (
                <React.Fragment key={k}>
                    <span style={{ fontSize: '0.7rem', color: 'var(--text-secondary)' }}>{k}</span>
                    <span style={{ fontSize: '0.75rem', fontFamily: 'monospace', color: 'var(--text-primary)' }}>{String(v ?? 'n/a')}</span>
                </React.Fragment>
            ))}
        </div>
    );
}

function Section({ title, icon, children }: { title: string; icon: React.ReactNode; children: React.ReactNode }) {
    return (
        <div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 10 }}>
                <span style={{ color: '#818cf8' }}>{icon}</span>
                <span style={{ fontWeight: 700, fontSize: '0.75rem', color: 'var(--text-secondary)', letterSpacing: '0.05em', textTransform: 'uppercase' }}>{title}</span>
            </div>
            {children}
        </div>
    );
}

function StatusBadge({ status }: { status: string }) {
    const map: Record<string, { color: string; label: string }> = {
        idle:    { color: 'rgba(255,255,255,0.2)',  label: 'Not analyzed' },
        queued:  { color: '#f59e0b', label: 'Queued' },
        running: { color: '#3b82f6', label: 'Analyzing…' },
        done:    { color: '#22c55e', label: 'Complete' },
        failed:  { color: '#ef4444', label: 'Failed' },
    };
    const s = map[status] || map.idle;
    return <span style={{ fontSize: '0.65rem', fontWeight: 700, color: s.color, padding: '3px 8px', borderRadius: 999, border: `1px solid ${s.color}40` }}>{s.label}</span>;
}

function ActionBtn({ icon, label, onClick, disabled, accent }: any) {
    return (
        <button onClick={onClick} disabled={disabled} style={{
            display: 'flex', alignItems: 'center', gap: 5, padding: '5px 12px',
            borderRadius: 6, border: accent ? '1px solid rgba(99,102,241,0.4)' : '1px solid rgba(255,255,255,0.1)',
            background: accent ? 'rgba(99,102,241,0.15)' : 'rgba(255,255,255,0.05)',
            color: accent ? '#a5b4fc' : 'var(--text-secondary)',
            cursor: disabled ? 'not-allowed' : 'pointer', fontSize: '0.75rem', opacity: disabled ? 0.5 : 1,
        }}>
            {icon}{label}
        </button>
    );
}

function Placeholder({ onAnalyze }: { onAnalyze: () => void }) {
    return (
        <div style={{ textAlign: 'center', padding: '48px 24px' }}>
            <BarChart2 size={40} style={{ color: 'rgba(255,255,255,0.15)', marginBottom: 16 }} />
            <div style={{ color: 'var(--text-secondary)', marginBottom: 20 }}>This asset hasn't been analyzed yet.</div>
            <button onClick={onAnalyze} style={{
                padding: '10px 24px', borderRadius: 8, border: '1px solid rgba(99,102,241,0.4)',
                background: 'rgba(99,102,241,0.15)', color: '#a5b4fc', cursor: 'pointer', fontWeight: 600,
            }}>
                Analyze with FFmpeg
            </button>
        </div>
    );
}

function Spinner({ label }: { label: string }) {
    return (
        <div style={{ display: 'flex', alignItems: 'center', gap: 12, padding: 32, color: 'var(--text-secondary)' }}>
            <RefreshCw size={20} style={{ animation: 'spin 1s linear infinite', color: '#818cf8' }} />
            {label}
        </div>
    );
}
