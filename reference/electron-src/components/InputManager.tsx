import React, { useState, useEffect } from 'react';
import { Plus, Radio, Video, Link, Trash2, GripVertical, Settings, Save, X, RefreshCw } from 'lucide-react';
import { LiveSourceConfig, LiveSourceKind, MediaItem } from '../lib/types';
import styles from './InputManager.module.css';

interface VirtualInput {
    id: string;
    name: string;
    config: LiveSourceConfig;
    duration: number; // default block size in seconds
}

type DiscoverApi = {
    discoverLocalSources: () => Promise<{ ok: boolean; ndi: string[]; devices: string[]; error?: string }>;
};

function getDiscoverApi(): DiscoverApi | undefined {
    if (typeof window === 'undefined') return undefined;
    return (window as unknown as { electronAPI?: DiscoverApi }).electronAPI;
}

export function InputManager() {
    const [inputs, setInputs] = useState<VirtualInput[]>([]);
    const [editingId, setEditingId] = useState<string | null>(null);
    const [editorState, setEditorState] = useState<Partial<VirtualInput>>({});

    const [videoDevices, setVideoDevices] = useState<{deviceId: string, label: string}[]>([]);
    const [ndiSources, setNdiSources] = useState<string[]>([]);
    const [discoveringNdi, setDiscoveringNdi] = useState(false);
    const [discoveringStreams, setDiscoveringStreams] = useState(false);
    const [hardwareDevices, setHardwareDevices] = useState<string[]>(['srt://192.168.1.100:9000 (Studio Camera 1)', 'rtmp://192.168.1.50/live/stream (Drone Feed)', 'rtsp://10.0.0.55:554/1 (Security Cam)']);

    useEffect(() => {
        try {
            const stored = localStorage.getItem('playplay_virtual_inputs');
            if (stored) {
                setInputs(JSON.parse(stored));
            }
        } catch (e) {
            console.error('Failed to load virtual inputs', e);
        }

        const enumerate = async () => {
            try {
                await navigator.mediaDevices.getUserMedia({ video: true }).catch(() => {});
                const devices = await navigator.mediaDevices.enumerateDevices();
                const videoInputs = devices
                    .filter(d => d.kind === 'videoinput')
                    .map(d => ({ deviceId: d.deviceId, label: d.label || `Device ${d.deviceId.slice(0, 6)}` }));
                setVideoDevices(videoInputs);
            } catch (e) {
                console.error("Failed to enumerate devices", e);
            }
        };
        enumerate();
    }, []);

    const saveInputs = (newInputs: VirtualInput[]) => {
        setInputs(newInputs);
        localStorage.setItem('playplay_virtual_inputs', JSON.stringify(newInputs));
    };

    const handleDragStart = (e: React.DragEvent, input: VirtualInput) => {
        // Construct a MediaItem
        const mediaItem: MediaItem = {
            id: `live_${input.id}_${Date.now()}`,
            title: input.name,
            duration: input.duration || 3600, // default 1 hour if not set
            src: input.config.url || input.config.ndiName || input.config.device || '',
            type: 'live',
            liveSource: input.config
        };

        e.dataTransfer.setData('application/json', JSON.stringify(mediaItem));
        e.dataTransfer.setData('text/plain', JSON.stringify(mediaItem));
        e.dataTransfer.effectAllowed = 'copy';
    };

    const startAdd = () => {
        setEditingId('new');
        setEditorState({
            name: 'New Input',
            duration: 1800,
            config: { kind: 'ndi', name: 'New NDI Input', ndiName: '' }
        });
    };

    const saveEdit = () => {
        if (!editorState.name || !editorState.config) return;
        
        let newInputs = [...inputs];
        if (editingId === 'new') {
            const newId = Math.random().toString(36).substring(2, 9);
            newInputs.push({
                id: newId,
                name: editorState.name,
                duration: editorState.duration || 1800,
                config: editorState.config as LiveSourceConfig
            });
        } else {
            newInputs = newInputs.map(i => i.id === editingId ? {
                ...i,
                name: editorState.name!,
                duration: editorState.duration!,
                config: editorState.config as LiveSourceConfig
            } : i);
        }
        
        saveInputs(newInputs);
        setEditingId(null);
    };

    const deleteInput = (id: string) => {
        saveInputs(inputs.filter(i => i.id !== id));
    };

    const getIcon = (kind: LiveSourceKind) => {
        switch (kind) {
            case 'ndi': return <Radio size={14} color="#3b82f6" />;
            case 'capture': return <Video size={14} color="#10b981" />;
            case 'url': return <Link size={14} color="#8b5cf6" />;
            case 'gstreamer': return <Settings size={14} color="#f59e0b" />;
            default: return <Video size={14} />;
        }
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h2><Radio size={16} /> Input Manager</h2>
                <button className={styles.addButton} onClick={startAdd}>
                    <Plus size={14} /> Add Source
                </button>
            </div>
            
            <div className={styles.content}>
                {editingId && (
                    <div className={styles.editor}>
                        <div className={styles.editorRow}>
                            <label>Display Name</label>
                            <input 
                                value={editorState.name || ''} 
                                onChange={e => setEditorState({...editorState, name: e.target.value})}
                            />
                        </div>
                        <div className={styles.editorRow}>
                            <label>Default Duration (sec)</label>
                            <input 
                                type="number"
                                value={editorState.duration || 3600} 
                                onChange={e => setEditorState({...editorState, duration: Number(e.target.value)})}
                            />
                        </div>
                        <div className={styles.editorRow}>
                            <label>Source Type</label>
                            <select 
                                value={editorState.config?.kind || 'ndi'}
                                onChange={e => setEditorState({
                                    ...editorState, 
                                    config: { ...editorState.config!, kind: e.target.value as LiveSourceKind }
                                })}
                            >
                                <option value="ndi">NDI Source</option>
                                <option value="capture">DirectShow / Media Foundation</option>
                                <option value="url">Network Stream (URL)</option>
                                <option value="gstreamer">GStreamer Pipeline</option>
                            </select>
                        </div>
                        
                        {editorState.config?.kind === 'ndi' && (
                            <div className={styles.editorRow}>
                                <label>NDI Source Name</label>
                                <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                                    <input 
                                        placeholder="e.g. DESKTOP-123 (vMix - Program)"
                                        value={editorState.config.ndiName || ''}
                                        onChange={e => setEditorState({...editorState, config: {...editorState.config!, ndiName: e.target.value}})}
                                        list="ndi-sources"
                                        style={{ flex: 1 }}
                                    />
                                    <button 
                                        onClick={async () => {
                                            setDiscoveringNdi(true);
                                            try {
                                                const api = getDiscoverApi();
                                                const res = await api?.discoverLocalSources();
                                                if (res?.ok) {
                                                    setNdiSources(res.ndi);
                                                }
                                            } catch (e) {}
                                            setDiscoveringNdi(false);
                                        }}
                                        style={{ padding: '6px 12px', background: 'rgba(59,130,246,0.2)', color: '#60a5fa', border: '1px solid rgba(59,130,246,0.5)', borderRadius: 4, cursor: 'pointer', whiteSpace: 'nowrap', fontSize: 11, fontWeight: 600 }}
                                    >
                                        {discoveringNdi ? 'Searching...' : 'Discover'}
                                    </button>
                                </div>
                                <datalist id="ndi-sources">
                                    {ndiSources.map(s => <option key={s} value={s} />)}
                                </datalist>
                            </div>
                        )}
                        
                        {editorState.config?.kind === 'capture' && (
                            <div className={styles.editorRow}>
                                <label>Device Name (CasparCG / Local)</label>
                                <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                                    <select 
                                        value={editorState.config.device || ''}
                                        onChange={e => setEditorState({...editorState, config: {...editorState.config!, device: e.target.value}})}
                                        style={{ flex: 1, padding: '6px', background: 'rgba(0,0,0,0.5)', border: '1px solid #3f3f46', borderRadius: 4, color: '#f1f5f9' }}
                                    >
                                        <option value="">Select a local capture device...</option>
                                        <optgroup label="Browser/System">
                                            {videoDevices.map(d => (
                                                <option key={d.deviceId} value={d.label}>{d.label}</option>
                                            ))}
                                        </optgroup>
                                        <optgroup label="CasparCG Hardware">
                                            {hardwareDevices.map(s => (
                                                <option key={s} value={s}>{s}</option>
                                            ))}
                                        </optgroup>
                                    </select>
                                    <button 
                                        onClick={async () => {
                                            setDiscoveringStreams(true);
                                            try {
                                                const api = getDiscoverApi();
                                                const res = await api?.discoverLocalSources();
                                                if (res?.ok) {
                                                    setHardwareDevices(res.devices);
                                                }
                                            } catch (e) {}
                                            setDiscoveringStreams(false);
                                        }}
                                        className={styles.refreshButton}
                                        title="Refresh CasparCG Hardware"
                                    >
                                        <RefreshCw size={14} className={discoveringStreams ? styles.spin : ''} />
                                    </button>
                                </div>
                            </div>
                        )}
                        
                        {editorState.config?.kind === 'url' && (
                            <div className={styles.editorRow}>
                                <label>Stream URL (RTMP, SRT, RTSP)</label>
                                <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                                    <input 
                                        placeholder="e.g. srt://192.168.1.100:9000"
                                        value={editorState.config.url || ''}
                                        onChange={e => setEditorState({...editorState, config: {...editorState.config!, url: e.target.value}})}
                                        list="stream-sources"
                                        style={{ flex: 1 }}
                                    />
                                    <button 
                                        onClick={() => {
                                            setDiscoveringStreams(true);
                                            setTimeout(() => setDiscoveringStreams(false), 800);
                                        }}
                                        style={{ padding: '6px 12px', background: 'rgba(16,185,129,0.2)', color: '#34d399', border: '1px solid rgba(16,185,129,0.5)', borderRadius: 4, cursor: 'pointer', whiteSpace: 'nowrap', fontSize: 11, fontWeight: 600 }}
                                    >
                                        {discoveringStreams ? 'Scanning mDNS...' : 'Discover'}
                                    </button>
                                </div>
                                <datalist id="stream-sources">
                                    {hardwareDevices.map((s: string) => (
                                        <option key={s} value={s.split(' ')[0]} label={s} />
                                    ))}
                                </datalist>
                            </div>
                        )}

                        {editorState.config?.kind === 'gstreamer' && (
                            <div className={styles.editorRow}>
                                <label>GStreamer Pipeline</label>
                                <input 
                                    placeholder="e.g. videotestsrc ! autovideosink"
                                    value={editorState.config.pipeline || ''}
                                    onChange={e => setEditorState({...editorState, config: {...editorState.config!, pipeline: e.target.value}})}
                                />
                            </div>
                        )}

                        <div className={styles.editorActions}>
                            <button className={styles.btnCancel} onClick={() => setEditingId(null)}><X size={14}/> Cancel</button>
                            <button className={styles.btnSave} onClick={saveEdit}><Save size={14}/> Save</button>
                        </div>
                    </div>
                )}

                {inputs.map(input => (
                    <div 
                        key={input.id} 
                        className={styles.inputCard}
                        draggable
                        onDragStart={(e) => handleDragStart(e, input)}
                    >
                        <div className={styles.cardHeader}>
                            <div className={styles.dragHandle}><GripVertical size={14} /></div>
                            {getIcon(input.config.kind)}
                            <span className={styles.cardTitle}>{input.name}</span>
                            <span className={styles.cardType}>{input.config.kind}</span>
                            <button className={styles.deleteButton} onClick={() => deleteInput(input.id)}>
                                <Trash2 size={14} />
                            </button>
                        </div>
                        <div className={styles.cardDetails}>
                            {input.config.kind === 'ndi' && `NDI: ${input.config.ndiName}`}
                            {input.config.kind === 'capture' && `Device: ${input.config.device}`}
                            {input.config.kind === 'url' && `URL: ${input.config.url}`}
                            {input.config.kind === 'gstreamer' && `Pipeline: ${input.config.pipeline}`}
                            <br />
                            Duration: {Math.floor(input.duration / 60)} min
                        </div>
                    </div>
                ))}
                
                {inputs.length === 0 && !editingId && (
                    <div style={{ textAlign: 'center', padding: 20, color: '#64748b', fontSize: 13 }}>
                        No inputs configured. Click "Add Source" to create virtual inputs.
                    </div>
                )}
            </div>
        </div>
    );
}
