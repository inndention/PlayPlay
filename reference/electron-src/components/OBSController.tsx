import React, { useState, useEffect, useRef } from 'react';
import { OBSSession, OBSCommand } from '../lib/types';
import { OBSWebSocketClient, OBSSessionHealth, getOBSClient, pruneOBSClients } from '../lib/obsWebSocket';
import { loadConnectionSettings } from '../lib/settingsManager';
import { loadOBSCommands, saveOBSCommands } from '../lib/storage';
import { scheduleOBSCommand } from '../lib/obsScheduler';
import {
    Radio, Disc, Wifi, WifiOff, Circle,
    RefreshCw, ChevronDown, ChevronUp, Terminal,
    Play, Plus, Trash, Save, X, Edit2, Clock
} from 'lucide-react';
import styles from './OBSController.module.css';

interface OBSControllerProps {
    sessions: OBSSession[];
    onSessionsChange: (sessions: OBSSession[]) => void;
    onClientsChange?: (clients: Map<string, OBSWebSocketClient>) => void;
}

interface SceneInfo {
    sceneName: string;
}

const generateId = () => Math.random().toString(36).substr(2, 9);

type OBSTab = 'sessions' | 'commands';

// Settings > Connections > OBS feeds this session. Its password goes to starobs only.
const SETTINGS_SESSION_ID = 'obs_main';

function healthNote(health: OBSSessionHealth | undefined): string | null {
    if (!health) return null;
    if (health.state === 'auth_failed') return 'OBS rejected the password. Check Settings > Connections > OBS.';
    if (health.state === 'reconnecting') return `Waiting for OBS${health.lastError ? `: ${health.lastError}` : ''}`;
    if (health.state === 'connecting') return 'Connecting to OBS...';
    if (health.state === 'unknown') return health.lastError;
    return null;
}

export function OBSController({ sessions, onSessionsChange, onClientsChange }: OBSControllerProps) {
    const [scenes, setScenes] = useState<Map<string, SceneInfo[]>>(new Map());
    const [currentScenes, setCurrentScenes] = useState<Map<string, string>>(new Map());
    const [health, setHealth] = useState<Map<string, OBSSessionHealth>>(new Map());
    const [expandedSession, setExpandedSession] = useState<string | null>(sessions[0]?.id || null);
    const [activeTab, setActiveTab] = useState<OBSTab>('sessions');
    const [commandsSessionId, setCommandsSessionId] = useState<string>(sessions[0]?.id || '');

    // Custom Commands State
    const [commands, setCommands] = useState<OBSCommand[]>([]);
    const [isAddingCmd, setIsAddingCmd] = useState(false);
    const [editingCmd, setEditingCmd] = useState<OBSCommand | null>(null);
    const [cmdForm, setCmdForm] = useState<Partial<OBSCommand>>({});

    // Clients and their listeners outlive a render, so they read the latest props through refs.
    const clientsRef = useRef<Map<string, OBSWebSocketClient>>(new Map());
    const sessionsRef = useRef(sessions);
    const onSessionsChangeRef = useRef(onSessionsChange);
    const onClientsChangeRef = useRef(onClientsChange);
    const sessionKey = sessions.map(s => s.id).join('|');

    useEffect(() => {
        sessionsRef.current = sessions;
        onSessionsChangeRef.current = onSessionsChange;
        onClientsChangeRef.current = onClientsChange;
    });

    // Sync commands session when sessions change
    useEffect(() => {
        const valid = sessions.some(s => s.id === commandsSessionId);
        if (!valid && sessions[0]) setCommandsSessionId(sessions[0].id);
    }, [sessions, commandsSessionId]);

    // Load commands
    useEffect(() => {
        setCommands(loadOBSCommands());
    }, []);

    // Save commands
    useEffect(() => {
        saveOBSCommands(commands);
    }, [commands]);

    const commitSessions = (next: OBSSession[]) => {
        sessionsRef.current = next;
        onSessionsChangeRef.current(next);
    };

    // Pushes status only when it changed, so an unchanged status does not re-render the parent.
    const applyStatus = (updated: OBSSession) => {
        const current = sessionsRef.current;
        const prev = current.find(s => s.id === updated.id);
        if (!prev) return;
        if (
            Boolean(prev.isConnected) === Boolean(updated.isConnected) &&
            Boolean(prev.isRecording) === Boolean(updated.isRecording) &&
            Boolean(prev.isStreaming) === Boolean(updated.isStreaming)
        ) return;
        commitSessions(current.map(s => s.id === updated.id
            ? { ...s, isConnected: updated.isConnected, isRecording: updated.isRecording, isStreaming: updated.isStreaming }
            : s));
    };

    const handleObsEvent = (sessionId: string, event: string, data: Record<string, unknown>) => {
        console.log(`[OBS ${sessionId}] Event:`, event, data);
        if (event === 'CurrentProgramSceneChanged') {
            setCurrentScenes(prev => new Map(prev).set(sessionId, data.sceneName as string));
        }
    };

    // Settings feed the default session through starobs. The password is sent in the PUT and not kept.
    const configureFromSettings = async (client: OBSWebSocketClient, session: OBSSession) => {
        if (session.id !== SETTINGS_SESSION_ID) {
            await client.configure({ host: session.host, port: session.port, autoConnect: session.autoConnect });
            return;
        }
        const obs = loadConnectionSettings().obs;
        const host = (obs.host || '').trim();
        const port = Number(obs.port);
        if (!host || !Number.isInteger(port) || port < 1 || port > 65535) {
            throw new Error('OBS host or port in Settings is invalid');
        }
        await client.configure({ host, port, password: obs.password ?? '', autoConnect: session.autoConnect });
        const current = sessionsRef.current.find(s => s.id === session.id);
        if (current && (current.host !== host || current.port !== port)) {
            commitSessions(sessionsRef.current.map(s => s.id === session.id ? { ...s, host, port } : s));
        }
    };

    // One client per session id. The clients are never disconnected here: starobs owns the OBS
    // connection, so a re-render or an unmount must not touch it.
    useEffect(() => {
        pruneOBSClients(sessions.map(s => s.id));
        const clients = new Map<string, OBSWebSocketClient>();
        const detachers = sessions.map(session => {
            const client = getOBSClient(session);
            clients.set(session.id, client);
            const detach = client.attach({
                onStatusChange: applyStatus,
                onHealthChange: next => setHealth(prev => new Map(prev).set(session.id, next)),
                onEvent: (event, data) => handleObsEvent(session.id, event, data),
            });
            configureFromSettings(client, session).catch(err => console.error(`[OBS ${session.name}] starobs configure failed:`, err));
            return detach;
        });
        clientsRef.current = clients;

        // Notify parent of clients
        onClientsChangeRef.current?.(new Map(clients));

        return () => detachers.forEach(detach => detach());
    }, [sessionKey]);

    const handleConnect = async (sessionId: string) => {
        const session = sessionsRef.current.find(s => s.id === sessionId);
        const client = clientsRef.current.get(sessionId);
        if (!session || !client) return;
        try {
            await configureFromSettings(client, session);
            await client.connect();
        } catch (err) {
            console.error(`[OBS ${session.name}] Connect failed:`, err);
        }
    };

    const handleDisconnect = async (sessionId: string) => {
        const client = clientsRef.current.get(sessionId);
        if (!client) return;
        try {
            await client.disconnect();
        } catch (err) {
            console.error(`[OBS ${sessionId}] Disconnect failed:`, err);
        }
    };

    const handleRefreshScenes = async (sessionId: string) => {
        const client = clientsRef.current.get(sessionId);
        if (client && client.isConnected) {
            try {
                const result = await client.getSceneList();
                setScenes(prev => new Map(prev).set(sessionId, result.scenes));
                setCurrentScenes(prev => new Map(prev).set(sessionId, result.currentProgramSceneName));
            } catch (err) {
                console.error('Failed to get scenes:', err);
            }
        }
    };

    const handleSceneChange = async (sessionId: string, sceneName: string) => {
        const client = clientsRef.current.get(sessionId);
        if (client && client.isConnected) {
            try {
                await client.setCurrentScene(sceneName);
                setCurrentScenes(prev => new Map(prev).set(sessionId, sceneName));
            } catch (err) {
                console.error('Failed to change scene:', err);
            }
        }
    };

    const handleToggleRecord = async (sessionId: string) => {
        const client = clientsRef.current.get(sessionId);
        if (client && client.isConnected) await client.toggleRecord();
    };

    const handleToggleStream = async (sessionId: string) => {
        const client = clientsRef.current.get(sessionId);
        if (client && client.isConnected) await client.toggleStream();
    };

    // --- COMMAND HANDLERS ---
    const handleSendCommand = async (sessionId: string, cmd: OBSCommand) => {
        const client = clientsRef.current.get(sessionId);
        if (client && client.isConnected) {
            try {
                // Support both old format (requestType) and new format (commandType)
                if (cmd.commandType) {
                    // New format - use command executor
                    const { executeOBSCommand } = await import('../lib/obsCommandExecutor');
                    await executeOBSCommand(cmd, client, {
                        date: new Date(),
                    });
                } else if (cmd.requestType) {
                    // Old format - backward compatibility
                    const data = cmd.requestData ? JSON.parse(cmd.requestData) : {};
                    await client.call(cmd.requestType, data);
                } else {
                    throw new Error('Command has no commandType or requestType');
                }
                console.log(`Sent command ${cmd.label} to ${sessionId}`);
            } catch (err) {
                console.error(`Failed to send command ${cmd.label}:`, err);
            }
        }
    };

    const handleSaveCommand = () => {
        // Support both old and new formats
        if (!cmdForm.label || (!cmdForm.requestType && !cmdForm.commandType)) return;

        if (editingCmd) {
            setCommands(prev => prev.map(c => c.id === editingCmd.id ? { ...editingCmd, ...cmdForm } as OBSCommand : c));
            setEditingCmd(null);
        } else {
            const newCmd: OBSCommand = {
                id: generateId(),
                label: cmdForm.label,
                // New format
                commandType: cmdForm.commandType || 'custom',
                sessionId: cmdForm.sessionId,
                sceneName: cmdForm.sceneName,
                sourceName: cmdForm.sourceName,
                recordFileName: cmdForm.recordFileName,
                recordDirectory: cmdForm.recordDirectory,
                // Old format (for backward compatibility)
                requestType: cmdForm.requestType,
                requestData: cmdForm.requestData,
                description: cmdForm.description,
            };
            setCommands(prev => [...prev, newCmd]);
        }
        setIsAddingCmd(false);
        setCmdForm({});
    };

    const handleDeleteCommand = (id: string) => {
        if (confirm('Delete this command?')) {
            setCommands(prev => prev.filter(c => c.id !== id));
        }
    };

    const startEdit = (cmd: OBSCommand) => {
        setEditingCmd(cmd);
        setCmdForm(cmd);
        setIsAddingCmd(true);
    };

    const effectiveCommandsSessionId = commandsSessionId || sessions[0]?.id || '';

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Radio size={16} />
                <span>OBS Control</span>
            </div>

            <div className={styles.tabBar}>
                <button
                    type="button"
                    className={`${styles.tab} ${activeTab === 'sessions' ? styles.tabActive : ''}`}
                    onClick={() => setActiveTab('sessions')}
                >
                    Sessions
                </button>
                <button
                    type="button"
                    className={`${styles.tab} ${activeTab === 'commands' ? styles.tabActive : ''}`}
                    onClick={() => setActiveTab('commands')}
                >
                    Commands
                </button>
            </div>

            {activeTab === 'sessions' && sessions.map(session => {
                const isExpanded = expandedSession === session.id;
                const sessionScenes = scenes.get(session.id) || [];
                const currentScene = currentScenes.get(session.id);

                return (
                    <div key={session.id} className={styles.session}>
                        <button
                            className={styles.sessionHeader}
                            onClick={() => setExpandedSession(isExpanded ? null : session.id)}
                        >
                            <span className={`${styles.statusDot} ${session.isConnected ? styles.connected : ''}`} />
                            <span className={styles.sessionName}>{session.name}</span>
                            <div className={styles.sessionStatus}>
                                {session.isRecording && <Circle size={10} className={styles.recording} />}
                                {session.isStreaming && <Radio size={10} className={styles.streaming} />}
                            </div>
                            {isExpanded ? <ChevronUp size={14} /> : <ChevronDown size={14} />}
                        </button>

                        {isExpanded && (
                            <div className={styles.sessionContent}>
                                {/* Connection Controls */}
                                <div className={styles.connectionRow}>
                                    {session.isConnected ? (
                                        <button
                                            className={`${styles.btn} ${styles.btnDanger}`}
                                            onClick={() => handleDisconnect(session.id)}
                                        >
                                            <WifiOff size={12} /> Disconnect
                                        </button>
                                    ) : (
                                        <button
                                            className={`${styles.btn} ${styles.btnPrimary}`}
                                            onClick={() => handleConnect(session.id)}
                                        >
                                            <Wifi size={12} /> Connect
                                        </button>
                                    )}
                                    <span className={styles.hostInfo}>
                                        {session.host}:{session.port}
                                    </span>
                                </div>
                                {!session.isConnected && healthNote(health.get(session.id)) && (
                                    <div className={styles.noScenes}>{healthNote(health.get(session.id))}</div>
                                )}

                                {session.isConnected && (
                                    <>
                                        {/* Record/Stream Controls */}
                                        <div className={styles.controlRow}>
                                            <button
                                                className={`${styles.btn} ${session.isRecording ? styles.btnActive : ''}`}
                                                onClick={() => handleToggleRecord(session.id)}
                                            >
                                                <Disc size={12} /> {session.isRecording ? 'Stop Rec' : 'Record'}
                                            </button>
                                            <button
                                                className={`${styles.btn} ${session.isStreaming ? styles.btnActive : ''}`}
                                                onClick={() => handleToggleStream(session.id)}
                                            >
                                                <Radio size={12} /> {session.isStreaming ? 'Stop Stream' : 'Stream'}
                                            </button>
                                        </div>

                                        {/* Scene Controls */}
                                        <div className={styles.scenesSection}>
                                            <div className={styles.scenesHeader}>
                                                <span>Scenes</span>
                                                <button
                                                    className={styles.refreshBtn}
                                                    onClick={() => handleRefreshScenes(session.id)}
                                                >
                                                    <RefreshCw size={12} />
                                                </button>
                                            </div>
                                            <div className={styles.scenesList}>
                                                {sessionScenes.map(scene => (
                                                    <button
                                                        key={scene.sceneName}
                                                        className={`${styles.sceneBtn} ${currentScene === scene.sceneName ? styles.activeScene : ''}`}
                                                        onClick={() => handleSceneChange(session.id, scene.sceneName)}
                                                    >
                                                        {scene.sceneName}
                                                    </button>
                                                ))}
                                                {sessionScenes.length === 0 && (
                                                    <span className={styles.noScenes}>Click refresh to load scenes</span>
                                                )}
                                            </div>
                                        </div>
                                    </>
                                )}
                            </div>
                        )}
                    </div>
                );
            })}

            {activeTab === 'sessions' && sessions.length === 0 && (
                <div className={styles.empty}>
                    No OBS sessions configured.<br />
                    Add sessions in Settings.
                </div>
            )}

            {activeTab === 'commands' && (
                <div className={styles.commandsSection}>
                    {sessions.length > 0 && (
                        <div className={styles.connectionRow} style={{ marginBottom: '0.5rem' }}>
                            <span className={styles.hostInfo}>Send to:</span>
                            <select
                                className={styles.formInput}
                                value={effectiveCommandsSessionId}
                                onChange={e => setCommandsSessionId(e.target.value)}
                                style={{ flex: 1, maxWidth: '200px' }}
                            >
                                {sessions.map(s => (
                                    <option key={s.id} value={s.id}>{s.name}</option>
                                ))}
                            </select>
                        </div>
                    )}
                    <div className={styles.commandsHeader}>
                        <span>Custom Commands</span>
                        {!isAddingCmd && (
                            <button className={styles.iconBtn} onClick={() => { setIsAddingCmd(true); setEditingCmd(null); setCmdForm({}); }}>
                                <Plus size={12} />
                            </button>
                        )}
                    </div>

                    {isAddingCmd ? (
                        <div className={styles.commandForm}>
                            <input
                                className={styles.formInput}
                                placeholder="Label (e.g. Set Scene)"
                                value={cmdForm.label || ''}
                                onChange={e => setCmdForm({ ...cmdForm, label: e.target.value })}
                            />
                            <input
                                className={styles.formInput}
                                placeholder="Request Type (e.g. SetCurrentProgramScene)"
                                value={cmdForm.requestType || ''}
                                onChange={e => setCmdForm({ ...cmdForm, requestType: e.target.value })}
                            />
                            <input
                                className={styles.formInput}
                                placeholder='JSON Data (e.g. {"sceneName": "Cam 1"})'
                                value={cmdForm.requestData || ''}
                                onChange={e => setCmdForm({ ...cmdForm, requestData: e.target.value })}
                            />
                            <div className={styles.formRow}>
                                <button className={styles.btn} onClick={handleSaveCommand} disabled={!cmdForm.label || (!cmdForm.requestType && !cmdForm.commandType)}>
                                    <Save size={12} /> Save
                                </button>
                                <button className={`${styles.btn} ${styles.btnDanger}`} onClick={() => { setIsAddingCmd(false); setEditingCmd(null); }}>
                                    <X size={12} /> Cancel
                                </button>
                            </div>
                        </div>
                    ) : (
                        <div className={styles.commandList}>
                            {commands.map(cmd => (
                                <div key={cmd.id} className={styles.commandRow}>
                                    <Terminal size={12} className={styles.textMuted} />
                                    <span className={styles.commandLabel} title={`${cmd.requestType || cmd.commandType} ${cmd.requestData || ''}`}>
                                        {cmd.label}
                                    </span>
                                    <button
                                        className={styles.iconBtn}
                                        onClick={() => handleSendCommand(effectiveCommandsSessionId, cmd)}
                                        title="Send Command"
                                        disabled={!effectiveCommandsSessionId}
                                    >
                                        <Play size={12} />
                                    </button>
                                    <button
                                        className={styles.iconBtn}
                                        onClick={async () => {
                                            const t = await (await import('../lib/promptPolyfill')).safePrompt('Run at (HH:MM 24h, today)?', '19:00');
                                            if (!t) return;
                                            const [h, m] = t.split(':').map(n => parseInt(n, 10));
                                            if (isNaN(h) || isNaN(m)) return;
                                            const dt = new Date();
                                            dt.setHours(h, m, 0, 0);
                                            scheduleOBSCommand(cmd.id, dt.getTime(), cmd.label);
                                            alert(`Scheduled ${cmd.label} at ${dt.toLocaleTimeString()}`);
                                        }}
                                        title="Schedule (today HH:MM)"
                                    >
                                        <Clock size={12} />
                                    </button>
                                    <button className={styles.iconBtn} onClick={() => startEdit(cmd)}>
                                        <Edit2 size={12} />
                                    </button>
                                    <button className={styles.iconBtn} onClick={() => handleDeleteCommand(cmd.id)}>
                                        <Trash size={12} />
                                    </button>
                                </div>
                            ))}
                            {commands.length === 0 && <span className={styles.noScenes}>No commands saved</span>}
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}

// Default sessions for initial setup
export const DEFAULT_OBS_SESSIONS: OBSSession[] = [
    {
        id: 'obs_main',
        name: 'Main OBS',
        host: '127.0.0.1',
        port: 4455,
        autoConnect: true,
    },
];
