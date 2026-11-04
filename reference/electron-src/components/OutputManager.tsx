import React, { useState, useEffect } from 'react';
import { Settings, X, Plus, Trash2, Tv, RefreshCw, Save } from 'lucide-react';
import { cn } from '../lib/utils';
import type { PlayoutChannel } from '../lib/types';

interface OutputManagerProps {
    channel: PlayoutChannel;
    casparCh: number;
    onClose: () => void;
}

interface CasparOutput {
    type: 'ndi' | 'screen' | 'decklink' | 'ffmpeg';
    id: string;
    details: string;
}

export function OutputManager({ channel, casparCh, onClose }: OutputManagerProps) {
    const [outputs, setOutputs] = useState<CasparOutput[]>([]);
    const [loading, setLoading] = useState(true);
    const [saving, setSaving] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [configPath, setConfigPath] = useState<string | null>(null);
    const [rawXml, setRawXml] = useState<string | null>(null);
    const [showAddMenu, setShowAddMenu] = useState(false);

    const loadConfig = async () => {
        try {
            setLoading(true);
            const api = (window as any).electronAPI;
            if (!api?.readCasparConfig) return;

            const res = await api.readCasparConfig();
            if (res.found && res.raw) {
                setConfigPath(res.path);
                setRawXml(res.raw);
                // Parse simple XML structure for the specific channel
                const channelBlockRegex = new RegExp(`<channel>([\\s\\S]*?)<\\/channel>`, 'g');
                let match;
                let chIndex = 1;
                let foundOutputs: CasparOutput[] = [];

                while ((match = channelBlockRegex.exec(res.raw)) !== null) {
                    if (chIndex === casparCh) {
                        const block = match[1];
                        
                        // Parse screen
                        if (block.includes('<screen>')) foundOutputs.push({ type: 'screen', id: 'screen', details: 'Preview Screen' });
                        else if (block.includes('<screen />')) foundOutputs.push({ type: 'screen', id: 'screen', details: 'Preview Screen' });
                        // Parse NDI
                        if (block.includes('<ndi>')) foundOutputs.push({ type: 'ndi', id: 'ndi', details: 'NDI Output' });
                        // Parse decklink
                        const dlMatches = block.matchAll(/<decklink>[\s\S]*?<device>(\d+)<\/device>[\s\S]*?<\/decklink>/g);
                        for (const dl of dlMatches) {
                            foundOutputs.push({ type: 'decklink', id: `dl-${dl[1]}`, details: `Decklink Device ${dl[1]}` });
                        }
                        break;
                    }
                    chIndex++;
                }
                setOutputs(foundOutputs);
            }
        } catch (e: any) {
            setError(e.message);
        } finally {
            setLoading(false);
        }
    };

    useEffect(() => {
        loadConfig();
    }, [casparCh]);

    const handleSave = async () => {
        if (!configPath || !rawXml) {
            onClose();
            return;
        }
        setSaving(true);
        try {
            const api = (window as any).electronAPI;
            
            // Build the new <consumers> block
            let newConsumersInner = '';
            for (const out of outputs) {
                if (out.type === 'screen') newConsumersInner += '\n        <screen />\n';
                else if (out.type === 'ndi') newConsumersInner += `\n        <ndi>\n          <name>Play-Play CH${casparCh}</name>\n          <allow-transparency>true</allow-transparency>\n        </ndi>\n`;
                else if (out.type === 'decklink') {
                    const devId = out.id.replace('dl-', '');
                    newConsumersInner += `\n        <decklink>\n          <device>${devId}</device>\n          <embedded-audio>true</embedded-audio>\n          <latency>normal</latency>\n          <keyer>external</keyer>\n          <key-only>false</key-only>\n          <buffer-depth>3</buffer-depth>\n        </decklink>\n`;
                }
            }

            const channelBlocks = rawXml.split('<channel>');
            if (channelBlocks.length > casparCh) {
                let targetBlock = channelBlocks[casparCh];
                const consumersRegex = /<consumers>[\s\S]*?<\/consumers>/;
                if (consumersRegex.test(targetBlock)) {
                    targetBlock = targetBlock.replace(consumersRegex, `<consumers>${newConsumersInner}      </consumers>`);
                } else {
                    targetBlock = targetBlock.replace('</channel>', `  <consumers>${newConsumersInner}      </consumers>\n    </channel>`);
                }
                channelBlocks[casparCh] = targetBlock;
                
                const newXml = channelBlocks.join('<channel>');
                await api.writeCasparConfig(configPath, newXml);
                
                // If there are real-time stream status, they are simulated for now.
                // Output to UI that it succeeded.
                alert('Configuration Saved! If Decklink or Screen was changed, a restart may be required.');
            }
        } catch (e: any) {
            alert('Error saving configuration: ' + e.message);
        } finally {
            setSaving(false);
            onClose();
        }
    };

    const handleAddOutput = (type: 'ndi' | 'screen' | 'decklink') => {
        if (outputs.find(o => o.type === type)) {
            alert(type.toUpperCase() + ' output is already added.');
            return;
        }
        const newOut: CasparOutput = { type, id: type, details: type === 'ndi' ? 'NDI Output' : 'Preview Screen' };
        if (type === 'decklink') {
            const devId = prompt('Enter Decklink Device Number (e.g. 1):', '1');
            if (!devId) return;
            newOut.id = `dl-${devId}`;
            newOut.details = `Decklink Device ${devId}`;
        }
        setOutputs([...outputs, newOut]);
        setShowAddMenu(false);
    };

    return (
        <div style={{
            position: 'fixed', top: 0, left: 0, right: 0, bottom: 0,
            backgroundColor: 'rgba(0,0,0,0.8)',
            display: 'flex', alignItems: 'center', justifyContent: 'center',
            zIndex: 1000
        }}>
            <div style={{
                background: '#18181b',
                border: '1px solid #27272a',
                borderRadius: 8,
                width: 600,
                display: 'flex',
                flexDirection: 'column',
                boxShadow: '0 20px 25px -5px rgba(0, 0, 0, 0.5)'
            }}>
                <div style={{ padding: '16px 20px', borderBottom: '1px solid #27272a', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
                        <Settings className="text-blue-500" size={20} />
                        <h2 style={{ margin: 0, fontSize: 16, color: '#f1f5f9' }}>Output Manager - {channel.name} (CH{casparCh})</h2>
                    </div>
                    <button onClick={onClose} style={{ background: 'transparent', border: 'none', color: '#94a3b8', cursor: 'pointer' }}>
                        <X size={20} />
                    </button>
                </div>

                <div style={{ padding: 20, flex: 1, minHeight: 300 }}>
                    {loading ? (
                        <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', height: '100%', color: '#94a3b8' }}>
                            <RefreshCw className="animate-spin" />
                            <span style={{ marginLeft: 10 }}>Loading configuration...</span>
                        </div>
                    ) : error ? (
                        <div style={{ color: '#ef4444', padding: 20, background: 'rgba(239,68,68,0.1)', borderRadius: 6 }}>
                            Error loading config: {error}
                        </div>
                    ) : (
                        <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
                            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', position: 'relative' }}>
                                <h3 style={{ margin: 0, fontSize: 14, color: '#e2e8f0' }}>Active Outputs</h3>
                                <button 
                                    onClick={() => setShowAddMenu(!showAddMenu)}
                                    style={{ display: 'flex', alignItems: 'center', gap: 6, background: '#3b82f6', color: 'white', border: 'none', padding: '6px 12px', borderRadius: 4, cursor: 'pointer', fontSize: 12 }}
                                >
                                    <Plus size={14} /> Add Output
                                </button>
                                {showAddMenu && (
                                    <div style={{ position: 'absolute', top: 35, right: 0, background: '#18181b', border: '1px solid #3f3f46', borderRadius: 6, zIndex: 10, overflow: 'hidden', boxShadow: '0 10px 15px -3px rgba(0,0,0,0.5)' }}>
                                        <button onClick={() => handleAddOutput('ndi')} style={{ display: 'block', width: '100%', padding: '8px 16px', background: 'transparent', border: 'none', color: '#f1f5f9', textAlign: 'left', cursor: 'pointer', borderBottom: '1px solid #27272a' }}>NDI Source</button>
                                        <button onClick={() => handleAddOutput('screen')} style={{ display: 'block', width: '100%', padding: '8px 16px', background: 'transparent', border: 'none', color: '#f1f5f9', textAlign: 'left', cursor: 'pointer', borderBottom: '1px solid #27272a' }}>Preview Screen</button>
                                        <button onClick={() => handleAddOutput('decklink')} style={{ display: 'block', width: '100%', padding: '8px 16px', background: 'transparent', border: 'none', color: '#f1f5f9', textAlign: 'left', cursor: 'pointer' }}>Decklink (SDI)</button>
                                    </div>
                                )}
                            </div>
                            
                            <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                                {outputs.length === 0 ? (
                                    <div style={{ color: '#64748b', fontSize: 13, fontStyle: 'italic' }}>No active outputs found for this channel.</div>
                                ) : (
                                    outputs.map(out => (
                                        <div key={out.id} style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', background: '#0f0f11', padding: '10px 14px', borderRadius: 6, border: '1px solid #27272a' }}>
                                            <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
                                                <Tv size={16} color={out.type === 'ndi' ? '#10b981' : '#3b82f6'} />
                                                <div>
                                                    <div style={{ color: '#f1f5f9', fontSize: 13, fontWeight: 500, textTransform: 'uppercase' }}>{out.type}</div>
                                                    <div style={{ color: '#94a3b8', fontSize: 11 }}>{out.details}</div>
                                                </div>
                                            </div>
                                            <button 
                                                onClick={() => setOutputs(outputs.filter(o => o.id !== out.id))}
                                                style={{ background: 'transparent', border: 'none', color: '#ef4444', cursor: 'pointer', padding: 4 }}
                                            >
                                                <Trash2 size={16} />
                                            </button>
                                        </div>
                                    ))
                                )}
                            </div>

                            <div style={{ marginTop: 20, padding: 12, background: 'rgba(59,130,246,0.1)', border: '1px solid rgba(59,130,246,0.2)', borderRadius: 6, fontSize: 12, color: '#bfdbfe' }}>
                                <strong>Note:</strong> Changes to SDI/Decklink outputs may require a CasparCG restart. Streams (SRT/RTMP/UDP) and NDI can be toggled dynamically.
                            </div>
                        </div>
                    )}
                </div>

                <div style={{ padding: '16px 20px', borderTop: '1px solid #27272a', display: 'flex', justifyContent: 'flex-end', gap: 10 }}>
                    <button onClick={onClose} style={{ padding: '8px 16px', background: 'transparent', border: '1px solid #3f3f46', color: '#e2e8f0', borderRadius: 4, cursor: 'pointer' }}>
                        Cancel
                    </button>
                    <button onClick={handleSave} disabled={saving} style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '8px 16px', background: '#3b82f6', border: 'none', color: '#fff', borderRadius: 4, cursor: 'pointer', opacity: saving ? 0.7 : 1 }}>
                        <Save size={16} /> {saving ? 'Saving...' : 'Save Configuration'}
                    </button>
                </div>
            </div>
        </div>
    );
}
