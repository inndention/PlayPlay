/**
 * SupervisorDashboard — slim UI shell talking to the C++ Supervisor.
 *
 * Demonstrates the post-Phase-4 architecture:
 *   - No in-renderer playout engines, no PlayoutEngineManager
 *   - All channel control via REST (port 8080)
 *   - All telemetry via WebSocket (port 8081)
 *
 * Drop this in alongside the legacy PlayoutController during migration.
 */

import React, { useCallback, useMemo, useState } from 'react';
import { useSupervisor, ChannelLiveState } from '../hooks/useSupervisor';
import type { TransportCommand } from '../lib/supervisorClient';

interface ChannelRowProps {
    channel: ChannelLiveState;
    onTransport: (id: string, cmd: TransportCommand) => void;
    onKill: (id: string) => void;
}

const ChannelRow: React.FC<ChannelRowProps> = ({ channel, onTransport, onKill }) => {
    const fps = channel.framesPerSec ? channel.framesPerSec.toFixed(1) : '—';
    const stale = Date.now() - (channel.lastHeartbeatMs || 0) > 1000;
    const indicator = !channel.healthy || stale ? '🔴' : '🟢';

    return (
        <tr style={{ borderBottom: '1px solid #333' }}>
            <td style={{ padding: '6px 10px' }}>{indicator}</td>
            <td style={{ padding: '6px 10px', fontFamily: 'monospace' }}>{channel.id}</td>
            <td style={{ padding: '6px 10px' }}>{channel.name ?? channel.id}</td>
            <td style={{ padding: '6px 10px' }}>{channel.state ?? 'unknown'}</td>
            <td style={{ padding: '6px 10px', fontFamily: 'monospace' }}>{channel.frameNumber ?? '—'}</td>
            <td style={{ padding: '6px 10px', fontFamily: 'monospace' }}>{fps}</td>
            <td style={{ padding: '6px 10px' }}>
                <button onClick={() => onTransport(channel.id, { action: 'play' })}>▶</button>
                <button onClick={() => onTransport(channel.id, { action: 'pause' })}>⏸</button>
                <button onClick={() => onTransport(channel.id, { action: 'stop' })}>⏹</button>
                <button onClick={() => onTransport(channel.id, { action: 'skip' })}>⏭</button>
                <button onClick={() => onKill(channel.id)} style={{ marginLeft: 8, color: '#f66' }}>kill</button>
            </td>
        </tr>
    );
};

export const SupervisorDashboard: React.FC = () => {
    const { connected, channels, health, client } = useSupervisor();
    const [newId, setNewId] = useState('');
    const [newName, setNewName] = useState('');
    const [busy, setBusy] = useState(false);

    const channelList = useMemo(
        () => Object.values(channels).sort((a, b) => a.id.localeCompare(b.id)),
        [channels]
    );

    const handleTransport = useCallback(async (id: string, cmd: TransportCommand) => {
        try {
            await client.sendTransport(id, cmd);
        } catch (err) {
            console.error('[SupervisorDashboard] transport failed', err);
        }
    }, [client]);

    const handleKill = useCallback(async (id: string) => {
        if (!confirm(`Kill channel ${id}?`)) return;
        try {
            await client.killChannel(id);
        } catch (err) {
            console.error('[SupervisorDashboard] kill failed', err);
        }
    }, [client]);

    const handleCreate = useCallback(async () => {
        if (!newId.trim()) return;
        setBusy(true);
        try {
            await client.createChannel({ id: newId.trim(), name: newName.trim() || newId.trim() });
            setNewId('');
            setNewName('');
        } catch (err) {
            console.error('[SupervisorDashboard] create failed', err);
            alert(`Create failed: ${(err as Error).message}`);
        } finally {
            setBusy(false);
        }
    }, [client, newId, newName]);

    return (
        <div style={{ padding: 16, color: '#eee', background: '#111', minHeight: '100vh', fontFamily: 'sans-serif' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 16, marginBottom: 16 }}>
                <h2 style={{ margin: 0 }}>Play-Play Supervisor</h2>
                <span style={{ color: connected ? '#7f7' : '#f77' }}>
                    {connected ? '● connected' : '○ disconnected'}
                </span>
                {health && (
                    <span style={{
                        padding: '2px 8px',
                        borderRadius: 4,
                        background: health.status === 'healthy' ? '#2a4' : health.status === 'degraded' ? '#a82' : '#a22',
                    }}>
                        {health.status} ({health.score}/100) · {health.activeChannels}/{health.activeChannels + health.failedChannels}
                    </span>
                )}
            </div>

            <table style={{ width: '100%', borderCollapse: 'collapse', background: '#1a1a1a', marginBottom: 24 }}>
                <thead>
                    <tr style={{ background: '#222', textAlign: 'left' }}>
                        <th style={{ padding: '8px 10px' }}></th>
                        <th style={{ padding: '8px 10px' }}>ID</th>
                        <th style={{ padding: '8px 10px' }}>Name</th>
                        <th style={{ padding: '8px 10px' }}>State</th>
                        <th style={{ padding: '8px 10px' }}>Frame</th>
                        <th style={{ padding: '8px 10px' }}>FPS</th>
                        <th style={{ padding: '8px 10px' }}>Transport</th>
                    </tr>
                </thead>
                <tbody>
                    {channelList.length === 0 ? (
                        <tr><td colSpan={7} style={{ padding: 16, textAlign: 'center', opacity: 0.6 }}>No channels</td></tr>
                    ) : (
                        channelList.map((ch) => (
                            <ChannelRow key={ch.id} channel={ch} onTransport={handleTransport} onKill={handleKill} />
                        ))
                    )}
                </tbody>
            </table>

            <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                <input
                    placeholder="channel id"
                    value={newId}
                    onChange={(e) => setNewId(e.target.value)}
                    style={{ padding: 6, background: '#222', color: '#eee', border: '1px solid #444' }}
                />
                <input
                    placeholder="name"
                    value={newName}
                    onChange={(e) => setNewName(e.target.value)}
                    style={{ padding: 6, background: '#222', color: '#eee', border: '1px solid #444' }}
                />
                <button onClick={handleCreate} disabled={busy || !newId.trim()} style={{ padding: '6px 16px' }}>
                    {busy ? 'Creating…' : '+ Create channel'}
                </button>
            </div>
        </div>
    );
};

export default SupervisorDashboard;
