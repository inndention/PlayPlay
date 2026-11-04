import React, { useState, useEffect, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import {
    Wifi,
    Plus,
    Trash2,
    Edit2,
    Link2,
    Unlink,
    Server,
    RefreshCw,
    ArrowLeft,
    Check,
    X,
    AlertCircle,
} from 'lucide-react';
import styles from './RemoteSessionsPage.module.css';

export interface RemoteSessionConfig {
    id: string;
    name: string;
    host: string;
    port: number;
    lastConnected?: number;
    status?: 'connected' | 'disconnected' | 'error';
    errorMessage?: string;
}

const STORAGE_KEY = 'playplay_remote_sessions';

function loadSessions(): RemoteSessionConfig[] {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        const parsed = raw ? JSON.parse(raw) : [];
        return Array.isArray(parsed) ? parsed.map((s: any) => ({
            id: s.id || `sess_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`,
            name: s.name || 'Unnamed',
            host: s.host || 'localhost',
            port: typeof s.port === 'number' ? s.port : 8080,
            lastConnected: s.lastConnected,
            status: 'disconnected' as const,
        })) : [];
    } catch {
        return [];
    }
}

function saveSessions(sessions: RemoteSessionConfig[]) {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(sessions.map(s => ({
        id: s.id,
        name: s.name,
        host: s.host,
        port: s.port,
        lastConnected: s.lastConnected,
    }))));
}

export function RemoteSessionsPage() {
    const navigate = useNavigate();
    const [sessions, setSessions] = useState<RemoteSessionConfig[]>(loadSessions);
    const [editingId, setEditingId] = useState<string | null>(null);
    const [form, setForm] = useState({ name: '', host: 'localhost', port: 8080 });
    const [adding, setAdding] = useState(false);
    const [testingId, setTestingId] = useState<string | null>(null);
    const [connectError, setConnectError] = useState<string | null>(null);

    const persist = useCallback((next: RemoteSessionConfig[]) => {
        setSessions(next);
        saveSessions(next);
    }, []);

    const addSession = () => {
        if (!form.name.trim() || !form.host.trim()) return;
        const newSession: RemoteSessionConfig = {
            id: `sess_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`,
            name: form.name.trim(),
            host: form.host.trim(),
            port: form.port,
            status: 'disconnected',
        };
        persist([...sessions, newSession]);
        setForm({ name: '', host: 'localhost', port: 8080 });
        setAdding(false);
    };

    const updateSession = (id: string, updates: Partial<RemoteSessionConfig>) => {
        const next = sessions.map(s => s.id === id ? { ...s, ...updates } : s);
        persist(next);
        setEditingId(null);
    };

    const removeSession = (id: string) => {
        persist(sessions.filter(s => s.id !== id));
        setEditingId(null);
    };

    const testConnection = async (session: RemoteSessionConfig) => {
        setTestingId(session.id);
        setConnectError(null);
        try {
            const base = `http://${session.host}:${session.port}`;
            const r = await fetch(`${base}/output`, { method: 'HEAD', signal: AbortSignal.timeout(5000) });
            const ok = r.ok || r.status === 404 || r.status === 405;
            const next = sessions.map(s => s.id === session.id
                ? { ...s, status: (ok ? 'connected' : 'error') as RemoteSessionConfig['status'], lastConnected: Date.now() }
                : s);
            persist(next);
        } catch (e: any) {
            const msg = e?.message || String(e);
            setConnectError(msg);
            const next = sessions.map(s => s.id === session.id
                ? { ...s, status: 'error' as const, errorMessage: msg }
                : s);
            persist(next);
        } finally {
            setTestingId(null);
        }
    };

    const openRemoteOutput = (session: RemoteSessionConfig) => {
        const url = `http://${session.host}:${session.port}/output`;
        window.open(url, '_blank', 'noopener');
        updateSession(session.id, { lastConnected: Date.now() });
    };

    return (
        <div className={styles.page}>
            <header className={styles.header}>
                <button type="button" className={styles.backBtn} onClick={() => navigate(-1)} title="Back">
                    <ArrowLeft size={20} />
                </button>
                <h1 className={styles.title}>
                    <Wifi size={24} />
                    Remote Sessions
                </h1>
                <p className={styles.subtitle}>
                    Connect to remote PLAY PLAY instances (output, API, and browser source URLs).
                </p>
            </header>

            {connectError && (
                <div className={styles.bannerError}>
                    <AlertCircle size={18} />
                    <span>{connectError}</span>
                    <button type="button" onClick={() => setConnectError(null)}><X size={16} /></button>
                </div>
            )}

            <section className={styles.section}>
                <div className={styles.sectionHeader}>
                    <h2>Saved sessions</h2>
                    <button
                        type="button"
                        className={styles.primaryBtn}
                        onClick={() => { setAdding(true); setForm({ name: '', host: 'localhost', port: 8080 }); }}
                    >
                        <Plus size={16} /> Add session
                    </button>
                </div>

                {adding && (
                    <div className={styles.card}>
                        <h3>New remote session</h3>
                        <div className={styles.formRow}>
                            <label>Name</label>
                            <input
                                value={form.name}
                                onChange={e => setForm(f => ({ ...f, name: e.target.value }))}
                                placeholder="e.g. Studio B"
                            />
                        </div>
                        <div className={styles.formRow}>
                            <label>Host</label>
                            <input
                                value={form.host}
                                onChange={e => setForm(f => ({ ...f, host: e.target.value }))}
                                placeholder="localhost or IP"
                            />
                        </div>
                        <div className={styles.formRow}>
                            <label>Port</label>
                            <input
                                type="number"
                                value={form.port}
                                onChange={e => setForm(f => ({ ...f, port: parseInt(e.target.value, 10) || 8080 }))}
                            />
                        </div>
                        <div className={styles.formActions}>
                            <button type="button" className={styles.secondaryBtn} onClick={() => setAdding(false)}>Cancel</button>
                            <button type="button" className={styles.primaryBtn} onClick={addSession} disabled={!form.name.trim()}>
                                <Check size={14} /> Save
                            </button>
                        </div>
                    </div>
                )}

                <ul className={styles.sessionList}>
                    {sessions.length === 0 && !adding && (
                        <li className={styles.empty}>No sessions yet. Add one to connect to a remote instance.</li>
                    )}
                    {sessions.map(s => (
                        <li key={s.id} className={styles.sessionCard}>
                            <div className={styles.sessionMain}>
                                <Server size={20} className={styles.sessionIcon} />
                                <div className={styles.sessionInfo}>
                                    <span className={styles.sessionName}>{s.name}</span>
                                    <span className={styles.sessionHost}>{s.host}:{s.port}</span>
                                    {s.status === 'connected' && <span className={styles.badgeConnected}>Reachable</span>}
                                    {s.status === 'error' && <span className={styles.badgeError}>{s.errorMessage || 'Unreachable'}</span>}
                                </div>
                            </div>
                            <div className={styles.sessionActions}>
                                <button
                                    type="button"
                                    className={styles.iconBtn}
                                    onClick={() => testConnection(s)}
                                    disabled={testingId === s.id}
                                    title="Test connection"
                                >
                                    {testingId === s.id ? <RefreshCw size={14} className={styles.spin} /> : <Link2 size={14} />}
                                </button>
                                <button
                                    type="button"
                                    className={styles.iconBtn}
                                    onClick={() => openRemoteOutput(s)}
                                    title="Open remote output in new tab"
                                >
                                    <Unlink size={14} />
                                </button>
                                <button type="button" className={styles.iconBtn} onClick={() => setEditingId(editingId === s.id ? null : s.id)} title="Edit">
                                    <Edit2 size={14} />
                                </button>
                                <button type="button" className={styles.iconBtnDanger} onClick={() => removeSession(s.id)} title="Remove">
                                    <Trash2 size={14} />
                                </button>
                            </div>
                            {editingId === s.id && (
                                <div className={styles.editForm}>
                                    <div className={styles.formRow}>
                                        <label>Name</label>
                                        <input
                                            defaultValue={s.name}
                                            onBlur={e => updateSession(s.id, { name: e.target.value.trim() || s.name })}
                                        />
                                    </div>
                                    <div className={styles.formRow}>
                                        <label>Host</label>
                                        <input
                                            defaultValue={s.host}
                                            onBlur={e => updateSession(s.id, { host: e.target.value.trim() || s.host })}
                                        />
                                    </div>
                                    <div className={styles.formRow}>
                                        <label>Port</label>
                                        <input
                                            type="number"
                                            defaultValue={s.port}
                                            onBlur={e => updateSession(s.id, { port: parseInt(e.target.value, 10) || s.port })}
                                        />
                                    </div>
                                    <button type="button" className={styles.secondaryBtn} onClick={() => setEditingId(null)}>Done</button>
                                </div>
                            )}
                        </li>
                    ))}
                </ul>
            </section>

            <section className={styles.section}>
                <h2>Quick reference</h2>
                <div className={styles.helpCard}>
                    <p><strong>Output URL</strong> — <code>http://&lt;host&gt;:&lt;port&gt;/output</code> — Use in OBS Browser Source.</p>
                    <p><strong>API state</strong> — <code>http://&lt;host&gt;:&lt;port&gt;/api/state</code> — POST with playout state (JSON).</p>
                    <p>Ensure the remote instance is running and the output server is on the same port (default 8080).</p>
                </div>
            </section>
        </div>
    );
}

export default RemoteSessionsPage;
