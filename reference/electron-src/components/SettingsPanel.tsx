import React, { useState, useEffect } from 'react';
import { AutomationSettings } from '../lib/types';
import { mergeEngineApproaches, type EngineApproachesMergeInput } from '../lib/engineApproaches';
import { buildCasparMediaPathFromEngineConfig } from '../lib/casparRamdisk';
import { loadSettings, saveSettings, DEFAULT_SETTINGS } from '../lib/storage';
import { getOpenRouterModelPlaceholderHint, OPENROUTER_LS_API_KEY_LEGACY } from '../lib/aiHelper';
import {
    HERMES_MODEL_OVERRIDE_LS,
    HERMES_SUPERVISOR_LS,
    isHermesSupervisorEnabled,
    loadHermesSupervisorEnvDefault,
} from '../lib/hermesSupervisorFlags';
import { Settings, Save, RotateCcw, Clock, Film, Zap, BarChart2, Radio, FolderOpen, Share2, Plug, Wrench, RefreshCw, CheckCircle, XCircle, FileText, BookOpen, Wifi, WifiOff, MessageCircle, Tv2 } from 'lucide-react';
import { ChannelsPanel } from './ChannelsPanel';
import { MediaLibrary } from './MediaLibrary';
import { trackerApi } from '../lib/trackerApi';
import styles from './SettingsPanel.module.css';

interface SettingsPanelProps {
    onClose?: () => void;
}

type SettingsTab = 'settings' | 'connections' | 'obs_commands' | 'social' | 'media' | 'channels' | 'streams' | 'logs' | 'devnotes';

interface EngineStatus {
    ffmpeg: { available: boolean; version: string | null; path: string };
    ffplayout: { available: boolean; version: string | null; path: string };
    gstreamer: { available: boolean; version: string | null; path: string };
    dektec: { available: boolean; devices: any[]; path: string };
    nodecg: { available: boolean; version: string | null; path: string };
    mpv: { available: boolean; version: string | null; path: string };
}

interface ConnectionSettings {
    obs: { host: string; port: number; password: string };
    caspercg: { host: string; port: number };
    ffplayout: { apiUrl: string; username: string; password: string };
    phpApi: { url: string; apiKey: string; enabled: boolean };
}

const defaultConnections: ConnectionSettings = {
    obs: { host: '127.0.0.1', port: 4455, password: '' },
    caspercg: { host: '127.0.0.1', port: 5250 },
    ffplayout: { apiUrl: 'http://127.0.0.1:8787', username: 'admin', password: 'admin' },
    phpApi: { url: 'http://yoursite.com/tracker.php', apiKey: '', enabled: false },
};

export function SettingsPanel({ onClose }: SettingsPanelProps) {
    const [activeTab, setActiveTab] = useState<SettingsTab>('settings');
    const [settings, setSettings] = useState<AutomationSettings>(DEFAULT_SETTINGS);
    const [hasChanges, setHasChanges] = useState(false);

    // Engine & Connection state
    const [engineStatus, setEngineStatus] = useState<EngineStatus | null>(null);
    const [connections, setConnections] = useState<ConnectionSettings>(defaultConnections);
    const [enginePaths, setEnginePaths] = useState<Record<string, string>>({});
    const [loading, setLoading] = useState(false);
    const [testResults, setTestResults] = useState<Record<string, { success: boolean; message: string }>>({});
    const [trackerConnected, setTrackerConnected] = useState(false);
    const [trackerStatus, setTrackerStatus] = useState<string>('Disconnected');
    const [logs, setLogs] = useState<Array<{ timestamp: number; level: string; message: string; source?: string }>>([]);
    const [devNotes, setDevNotes] = useState<string>('');
    const [hostname, setHostname] = useState<string>('');
    const [instanceMapping, setInstanceMapping] = useState<{ siteId?: string; playerId?: string }>({});
    const [registeredInstances, setRegisteredInstances] = useState<any[]>([]);
    const [autostart, setAutostart] = useState<boolean>(false);
    const [openRouterKey, setOpenRouterKey] = useState<string>('');
    const [openRouterModel, setOpenRouterModel] = useState<string>('');
    const [hermesSupervisor, setHermesSupervisor] = useState(false);
    const [hermesSupervisorEnv, setHermesSupervisorEnv] = useState(false);
    const [hermesModelOverride, setHermesModelOverride] = useState<string>('');

    // Streams state
    type StreamProtocol = 'RTMP' | 'RTSP' | 'SRT' | 'NDI' | 'HLS' | 'UDP';
    interface StreamEntry { id: string; label: string; url: string; protocol: StreamProtocol; direction: 'in' | 'out'; enabled: boolean; }
    const STREAM_STORAGE_KEY = 'playplay_streams';
    const loadStreams = (): StreamEntry[] => { try { return JSON.parse(localStorage.getItem(STREAM_STORAGE_KEY) || '[]'); } catch { return []; } };
    const [streams, setStreams] = useState<StreamEntry[]>(loadStreams);
    const saveStreams = (s: StreamEntry[]) => { setStreams(s); localStorage.setItem(STREAM_STORAGE_KEY, JSON.stringify(s)); };
    const addStream = (direction: 'in' | 'out') => {
        const proto = direction === 'in' ? 'RTMP' : 'RTMP';
        const url = direction === 'in' ? 'rtmp://0.0.0.0:1935/live/stream' : 'rtmp://a.rtmp.youtube.com/live2/';
        const entry: StreamEntry = { id: `stream_${Date.now()}`, label: direction === 'in' ? 'New Input' : 'New Output', url, protocol: proto, direction, enabled: false };
        saveStreams([...streams, entry]);
    };
    const updateStream = (id: string, patch: Partial<StreamEntry>) => saveStreams(streams.map(s => s.id === id ? { ...s, ...patch } : s));
    const removeStream = (id: string) => saveStreams(streams.filter(s => s.id !== id));

    useEffect(() => {
        // Load all settings through unified manager
        try {
            const { settingsManager } = require('../lib/settingsManager');
            const allSettings = settingsManager.getSettings();
            setSettings(allSettings);
            setConnections(allSettings.connections);
            setEnginePaths(allSettings.enginePaths);
            if (allSettings.hostname) setHostname(allSettings.hostname);
            if (allSettings.instanceMapping) setInstanceMapping(allSettings.instanceMapping);
            if (allSettings.devNotes) setDevNotes(allSettings.devNotes);
        } catch (e) {
            // Fallback to individual loaders
            setSettings(loadSettings());
            loadConnectionSettings();
        }
        
        checkEngineStatus();
        checkTrackerStatus();
        loadLogs();
        loadHostname();
        loadInstanceMapping();
        loadRegisteredInstances();

        // Load autostart state from main process
        const api = (window as any).electronAPI;
        if (api?.getAutostart) {
            api.getAutostart().then((r: { enabled?: boolean }) => {
                if (r && typeof r.enabled === 'boolean') setAutostart(r.enabled);
            }).catch(() => {});
        }

        // Load OpenRouter creds from localStorage
        try {
            setOpenRouterKey(localStorage.getItem('openrouter_api_key') || '');
            setOpenRouterModel(localStorage.getItem('openrouter_model') ?? '');
            setHermesModelOverride(localStorage.getItem(HERMES_MODEL_OVERRIDE_LS) || '');
        } catch { /* noop */ }

        void loadHermesSupervisorEnvDefault().then((fromEnv) => {
            setHermesSupervisorEnv(fromEnv);
            void isHermesSupervisorEnabled().then(setHermesSupervisor);
        });

        // Check tracker status periodically
        const trackerInterval = setInterval(() => {
            checkTrackerStatus();
            loadRegisteredInstances();
        }, 5000);
        
        return () => clearInterval(trackerInterval);
    }, []);

    const loadHostname = async () => {
        const api = (window as any).electronAPI;
        if (api?.getHostname) {
            try {
                const h = await api.getHostname();
                setHostname(h);
                // Register instance on startup if connected
                if (trackerConnected && h) {
                    await trackerApi.registerInstance(h, {
                        timestamp: Date.now(),
                        version: '1.0.0',
                    });
                }
            } catch (e) {
                console.error('Failed to get hostname:', e);
            }
        }
    };


    const loadInstanceMapping = () => {
        try {
            const stored = localStorage.getItem('instance_mapping');
            if (stored) {
                setInstanceMapping(JSON.parse(stored));
            }
        } catch (e) {
            console.error('Failed to load instance mapping:', e);
        }
    };

    const saveInstanceMapping = async (siteId?: string, playerId?: string) => {
        const mapping = { siteId, playerId };
        setInstanceMapping(mapping);
        try {
            localStorage.setItem('instance_mapping', JSON.stringify(mapping));
            if (hostname) {
                await trackerApi.updateInstanceMapping(hostname, siteId || '', playerId);
            }
        } catch (e) {
            console.error('Failed to save instance mapping:', e);
        }
    };

    const loadRegisteredInstances = async () => {
        if (trackerConnected) {
            try {
                const instances = await trackerApi.getRegisteredInstances();
                setRegisteredInstances(instances);
            } catch (e) {
                console.error('Failed to load registered instances:', e);
            }
        }
    };

    const loadConnectionSettings = () => {
        try {
            const { settingsManager } = require('../lib/settingsManager');
            const settings = settingsManager.getSettings();
            setConnections(settings.connections);
            setEnginePaths(settings.enginePaths);
            if (settings.hostname) setHostname(settings.hostname);
            if (settings.instanceMapping) setInstanceMapping(settings.instanceMapping);
            if (settings.devNotes) setDevNotes(settings.devNotes);
        } catch (e) {
            console.error('Failed to load connection settings:', e);
            // Fallback to direct localStorage
            try {
                const stored = localStorage.getItem('connection_settings');
                if (stored) setConnections({ ...defaultConnections, ...JSON.parse(stored) });
                const paths = localStorage.getItem('engine_paths');
                if (paths) setEnginePaths(JSON.parse(paths));
            } catch (e2) {
                console.error('Fallback load failed:', e2);
            }
        }
    };

    const checkTrackerStatus = async () => {
        if (!connections.phpApi.enabled || !connections.phpApi.url) {
            setTrackerConnected(false);
            setTrackerStatus('Not configured');
            return;
        }

        // Use the actual connection state from trackerApi
        const connected = trackerApi.getConnectionState();
        setTrackerConnected(connected);
        
        if (connected) {
            // Verify connection is still alive
            try {
                const result = await trackerApi.getSiteStates();
                if (result !== null) {
                    setTrackerStatus('Connected');
                } else {
                    setTrackerConnected(false);
                    setTrackerStatus('Connection lost');
                }
            } catch (e) {
                setTrackerConnected(false);
                setTrackerStatus('Connection error');
            }
        } else {
            setTrackerStatus('Disconnected');
        }
    };

    const handleTrackerConnect = async () => {
        if (trackerConnected) {
            // Disconnect
            trackerApi.disconnect();
            setTrackerConnected(false);
            setTrackerStatus('Disconnected');
        } else {
            // Connect - use testConnection which establishes connection
            setLoading(true);
            const result = await trackerApi.testConnection();
            setLoading(false);
            if (result.success) {
                setTrackerConnected(true);
                setTrackerStatus('Connected');
            } else {
                setTrackerConnected(false);
                setTrackerStatus(`Failed: ${result.message}`);
            }
        }
    };

    useEffect(() => {
        // Listen to connection state changes
        const unsubscribe = trackerApi.onConnectionStateChange((connected) => {
            setTrackerConnected(connected);
            if (connected) {
                setTrackerStatus('Connected');
            } else {
                setTrackerStatus('Disconnected');
            }
        });
        return unsubscribe;
    }, []);

    const loadDevNotes = () => {
        try {
            const notes = localStorage.getItem('dev_notes');
            if (notes) setDevNotes(notes);
        } catch (e) {
            console.error('Failed to load dev notes:', e);
        }
    };

    const saveDevNotes = () => {
        try {
            localStorage.setItem('dev_notes', devNotes);
            console.log('[Dev Notes] Saved successfully');
        } catch (e) {
            console.error('Failed to save dev notes:', e);
        }
    };

    // Auto-save dev notes on change (debounced)
    useEffect(() => {
        if (activeTab === 'devnotes' && devNotes !== '') {
            const timeoutId = setTimeout(() => {
                saveDevNotes();
            }, 2000); // Auto-save after 2 seconds of no typing
            return () => clearTimeout(timeoutId);
        }
    }, [devNotes, activeTab]);

    const loadLogs = () => {
        try {
            const stored = localStorage.getItem('app_logs');
            if (stored) {
                const parsed = JSON.parse(stored);
                setLogs(parsed.slice(-1000)); // Keep last 1000 logs
            }
        } catch (e) {
            console.error('Failed to load logs:', e);
        }
    };

    const checkEngineStatus = async () => {
        setLoading(true);
        try {
            const api = (window as any).electronAPI;
            if (api?.checkEngineAvailability) {
                const status = await api.checkEngineAvailability(enginePaths);
                setEngineStatus(status);
            }
        } catch (e) {
            console.error('Failed to check engine status:', e);
        } finally {
            setLoading(false);
        }
    };

    const handleChange = <K extends keyof AutomationSettings>(key: K, value: AutomationSettings[K]) => {
        setSettings(prev => ({ ...prev, [key]: value }));
        setHasChanges(true);
    };

    const handleEngineApproachesPatch = (patch: EngineApproachesMergeInput) => {
        setSettings((prev) => {
            const mergedPrev = mergeEngineApproaches(prev.engineApproaches);
            const nextRam = patch.ramdisk
                ? { ...mergedPrev.ramdisk, ...patch.ramdisk }
                : mergedPrev.ramdisk;
            return {
                ...prev,
                engineApproaches: mergeEngineApproaches({
                    ...mergedPrev,
                    ...patch,
                    ramdisk: nextRam,
                }),
            };
        });
        setHasChanges(true);
    };

    const handleSave = () => {
        try {
            const { settingsManager } = require('../lib/settingsManager');
            // Save all settings through unified manager
            settingsManager.saveAll({
                ...settings,
                connections,
                enginePaths,
                hostname,
                instanceMapping,
                devNotes,
            });
        } catch (e) {
            // Fallback to direct storage
            saveSettings(settings);
            localStorage.setItem('connection_settings', JSON.stringify(connections));
            localStorage.setItem('engine_paths', JSON.stringify(enginePaths));
        }

        // Sync PHP Tracker config to backend
        const api = (window as any).electronAPI;
        if (api?.updateTrackerConfig) {
            api.updateTrackerConfig(connections.phpApi);
        }

        setHasChanges(false);
        checkEngineStatus();
        try {
            window.dispatchEvent(new CustomEvent('playplay-settings-saved'));
        } catch {
            /* noop */
        }
    };

    const handleReset = () => {
        setSettings(DEFAULT_SETTINGS);
        setConnections(defaultConnections);
        setEnginePaths({});
        setHasChanges(true);
    };

    const updateConnection = (section: keyof ConnectionSettings, key: string, value: any) => {
        setConnections(prev => ({
            ...prev,
            [section]: { ...prev[section], [key]: value }
        }));
        setHasChanges(true);
    };

    const testConnection = async (type: 'obs' | 'caspercg' | 'ffplayout' | 'phpapi') => {
        setLoading(true);
        setTestResults(prev => ({ ...prev, [type]: { success: false, message: 'Testing...' } }));

        try {
            let result = { success: false, message: 'Not implemented' };

            if (type === 'obs') {
                try {
                    // Simple WebSocket connection test
                    const testWs = new WebSocket(`ws://${connections.obs.host}:${connections.obs.port}`);
                    result = await new Promise<{ success: boolean; message: string }>((resolve) => {
                        const timeout = setTimeout(() => {
                            testWs.close();
                            resolve({ success: false, message: 'Connection timeout' });
                        }, 5000);
                        testWs.onopen = () => {
                            clearTimeout(timeout);
                            testWs.close();
                            resolve({ success: true, message: 'OBS WebSocket reachable' });
                        };
                        testWs.onerror = () => {
                            clearTimeout(timeout);
                            resolve({ success: false, message: 'Connection refused' });
                        };
                    });
                } catch (e: any) {
                    result = { success: false, message: e.message || 'Connection failed' };
                }
            } else if (type === 'caspercg') {
                const api = (window as any).electronAPI;
                if (api?.sendAMCPCommand) {
                    try {
                        const res = await api.sendAMCPCommand(
                            connections.caspercg.host,
                            connections.caspercg.port,
                            'VERSION'
                        );
                        result = res.success
                            ? { success: true, message: `CasperCG ${res.data?.trim()}` }
                            : { success: false, message: res.error || 'Connection failed' };
                    } catch (e: any) {
                        result = { success: false, message: e.message };
                    }
                } else {
                    result = { success: false, message: 'Not in Electron' };
                }
            } else if (type === 'ffplayout') {
                const api = (window as any).electronAPI;
                if (api?.ffplayoutApi?.login) {
                    try {
                        const loginRes = await api.ffplayoutApi.login(
                            connections.ffplayout.username,
                            connections.ffplayout.password,
                            connections.ffplayout.apiUrl
                        );
                        if (loginRes.success) {
                            result = { success: true, message: 'FFplayout Authenticated OK' };
                        } else {
                            result = { success: false, message: loginRes.error || 'Authentication failed' };
                        }
                    } catch (e: any) {
                        result = { success: false, message: e.message };
                    }
                } else {
                    // Fallback to simple fetch if login API not available
                    try {
                        const res = await fetch(`${connections.ffplayout.apiUrl}/api/channel/1`, {
                            method: 'GET',
                            headers: { 'Content-Type': 'application/json' }
                        });
                        result = res.ok
                            ? { success: true, message: 'FFplayout API OK' }
                            : { success: false, message: `HTTP ${res.status}` };
                    } catch (e: any) {
                        result = { success: false, message: e.message };
                    }
                }
            } else if (type === 'phpapi') {
                // Use trackerApi.testConnection which establishes persistent connection
                const testResult = await trackerApi.testConnection();
                result = testResult;
                // After successful test, connection state is automatically updated
            }

            setTestResults(prev => ({ ...prev, [type]: result }));
        } finally {
            setLoading(false);
        }
    };

    const browseEnginePath = async (engine: string) => {
        const api = (window as any).electronAPI;
        if (api?.openFileDialog) {
            const result = await api.openFileDialog({
                filters: [{ name: 'Executable', extensions: ['exe'] }],
                properties: ['openFile']
            });
            if (result && !result.canceled && result.filePaths.length > 0) {
                setEnginePaths(prev => ({ ...prev, [engine]: result.filePaths[0] }));
                setHasChanges(true);
            }
        }
    };

    const renderTabs = () => (
        <div className={styles.tabs}>
            <button className={`${styles.tab} ${activeTab === 'settings' ? styles.activeTab : ''}`} onClick={() => setActiveTab('settings')}>
                <Settings size={14} /> Automation
            </button>
            <button className={`${styles.tab} ${activeTab === 'connections' ? styles.activeTab : ''}`} onClick={() => setActiveTab('connections')}>
                <Plug size={14} /> Connections
            </button>
            <button className={`${styles.tab} ${activeTab === 'obs_commands' ? styles.activeTab : ''}`} onClick={() => setActiveTab('obs_commands')}>
                <Zap size={14} /> OBS Cmds
            </button>
            <button className={`${styles.tab} ${activeTab === 'social' ? styles.activeTab : ''}`} onClick={() => setActiveTab('social')}>
                <Share2 size={14} /> Social
            </button>
            <button className={`${styles.tab} ${activeTab === 'media' ? styles.activeTab : ''}`} onClick={() => setActiveTab('media')}>
                <FolderOpen size={14} /> Media
            </button>
            <button className={`${styles.tab} ${activeTab === 'channels' ? styles.activeTab : ''}`} onClick={() => setActiveTab('channels')}>
                <Tv2 size={14} /> Channels
            </button>
            <button className={`${styles.tab} ${activeTab === 'streams' ? styles.activeTab : ''}`} onClick={() => setActiveTab('streams')}>
                <Radio size={14} /> Streams
            </button>
            <button className={`${styles.tab} ${activeTab === 'logs' ? styles.activeTab : ''}`} onClick={() => setActiveTab('logs')}>
                <FileText size={14} /> Logs
            </button>
            <button className={`${styles.tab} ${activeTab === 'devnotes' ? styles.activeTab : ''}`} onClick={() => setActiveTab('devnotes')}>
                <BookOpen size={14} /> Notes
            </button>
        </div>
    );

    if (activeTab === 'media') {
        return (
            <div className={styles.container}>
                {renderTabs()}
                <div style={{ flex: 1, overflow: 'hidden' }}>
                    <MediaLibrary />
                </div>
            </div>
        );
    }

    return (
        <div className={styles.container}>
            {renderTabs()}

            <div className={styles.content}>
                {/* Automation Settings Tab */}
                {activeTab === 'settings' && (
                    <>
                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Film size={14} /><span>Program Automation</span></div>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.alwaysPlayLatestEpisode} onChange={(e) => handleChange('alwaysPlayLatestEpisode', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Always play latest episode for series</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.shuffleAdsInHour} onChange={(e) => handleChange('shuffleAdsInHour', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Shuffle ads within each hour</span>
                            </label>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Clock size={14} /><span>Gap Fill Settings</span></div>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.fillGapsWithIds} onChange={(e) => handleChange('fillGapsWithIds', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Fill gaps with Station IDs</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.fillGapsWithBumpers} onChange={(e) => handleChange('fillGapsWithBumpers', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Fill gaps with Bumpers</span>
                            </label>
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>Max fill duration (seconds)</label>
                                <input type="number" className={styles.numberInput} value={settings.maxFillDuration} onChange={(e) => handleChange('maxFillDuration', parseInt(e.target.value) || 60)} min={0} max={300} />
                            </div>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Zap size={14} /><span>Playback</span></div>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.autoAdvance} onChange={(e) => handleChange('autoAdvance', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Auto-advance to next item</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.loopAtEndOfSchedule} onChange={(e) => handleChange('loopAtEndOfSchedule', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Loop at end of schedule</span>
                            </label>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Tv2 size={14} /><span>Playout engine approaches</span></div>
                            <p style={{ color: 'var(--text-secondary)', marginBottom: '0.75rem', fontSize: '0.85rem' }}>
                                Optional transport paths evaluated in this build. Defaults match prior behavior (Caspar on, auto-push on, MPV off). MPV options apply on next app launch (PlayoutEngineManager initializes once).
                            </p>
                            {(() => {
                                const ea = mergeEngineApproaches(settings.engineApproaches);
                                return (
                                    <>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.casparTransport} onChange={(e) => handleEngineApproachesPatch({ casparTransport: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>CasparCG as primary transport (AMCP play/stop when Caspar is running)</span>
                                        </label>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.casparAutoconfigureOnLaunch} onChange={(e) => handleEngineApproachesPatch({ casparAutoconfigureOnLaunch: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>Auto-configure and start Caspar on app launch</span>
                                        </label>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.casparHourAutoPush} onChange={(e) => handleEngineApproachesPatch({ casparHourAutoPush: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>Caspar hour auto-push at :00</span>
                                        </label>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.casparStartupCatchup} onChange={(e) => handleEngineApproachesPatch({ casparStartupCatchup: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>Caspar startup / restart catch-up (boss-clock push)</span>
                                        </label>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.mpvBroadcastEngine} onChange={(e) => handleEngineApproachesPatch({ mpvBroadcastEngine: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>MPV broadcast engine (Electron IPC)</span>
                                        </label>
                                        <label className={styles.toggle}>
                                            <input type="checkbox" checked={ea.mpvFallbackToElectron} onChange={(e) => handleEngineApproachesPatch({ mpvFallbackToElectron: e.target.checked })} />
                                            <span className={styles.toggleSlider}></span>
                                            <span className={styles.toggleLabel}>Fall back to built-in HTML5 if MPV fails to start</span>
                                        </label>
                                        <div style={{ marginTop: '0.85rem', paddingTop: '0.75rem', borderTop: '1px solid rgba(255,255,255,0.08)' }}>
                                            <div style={{ fontSize: '0.78rem', fontWeight: 700, color: 'var(--text-secondary)', marginBottom: 6 }}>RAM disk / fast drive (play engines)</div>
                                            <p style={{ color: 'var(--text-muted)', marginBottom: '0.6rem', fontSize: '0.78rem', lineHeight: 1.35 }}>
                                                Default root <code style={{ fontSize: '0.72rem' }}>B:/</code> matches a typical ImDisk letter. Caspar <code style={{ fontSize: '0.72rem' }}>&lt;media-path&gt;</code> updates on the next configure/start. Electron temp requires a full app restart after Save.
                                            </p>
                                            <div className={styles.inputGroup}>
                                                <label className={styles.inputLabel}>Ramdisk root</label>
                                                <input
                                                    type="text"
                                                    className={styles.textInput}
                                                    value={ea.ramdisk.root}
                                                    onChange={(e) => handleEngineApproachesPatch({ ramdisk: { root: e.target.value } })}
                                                    placeholder="B:/"
                                                />
                                            </div>
                                            <div className={styles.inputGroup}>
                                                <label className={styles.inputLabel}>Caspar media subfolder</label>
                                                <input
                                                    type="text"
                                                    className={styles.textInput}
                                                    value={ea.ramdisk.casparMediaFolder}
                                                    onChange={(e) => handleEngineApproachesPatch({ ramdisk: { casparMediaFolder: e.target.value } })}
                                                />
                                            </div>
                                            <label className={styles.toggle}>
                                                <input type="checkbox" checked={ea.ramdisk.enableCasparMediaOnRamdisk} onChange={(e) => handleEngineApproachesPatch({ ramdisk: { enableCasparMediaOnRamdisk: e.target.checked } })} />
                                                <span className={styles.toggleSlider}></span>
                                                <span className={styles.toggleLabel}>Point Caspar media-path at ramdisk (next Caspar configure)</span>
                                            </label>
                                            {ea.ramdisk.enableCasparMediaOnRamdisk ? (
                                                <p style={{ color: 'var(--text-muted)', fontSize: '0.72rem', margin: '4px 0 8px' }}>
                                                    Resolved: <code style={{ fontSize: '0.68rem', wordBreak: 'break-all' }}>{buildCasparMediaPathFromEngineConfig(ea) || '(invalid root)'}</code>
                                                </p>
                                            ) : null}
                                            <div className={styles.inputGroup}>
                                                <label className={styles.inputLabel}>Electron temp subfolder</label>
                                                <input
                                                    type="text"
                                                    className={styles.textInput}
                                                    value={ea.ramdisk.electronTempFolder}
                                                    onChange={(e) => handleEngineApproachesPatch({ ramdisk: { electronTempFolder: e.target.value } })}
                                                />
                                            </div>
                                            <label className={styles.toggle}>
                                                <input type="checkbox" checked={ea.ramdisk.enableElectronTempOnRamdisk} onChange={(e) => handleEngineApproachesPatch({ ramdisk: { enableElectronTempOnRamdisk: e.target.checked } })} />
                                                <span className={styles.toggleSlider}></span>
                                                <span className={styles.toggleLabel}>Use ramdisk for Electron temp (restart after Save; or set PLAYPLAY_RAMDISK_ROOT)</span>
                                            </label>
                                        </div>
                                    </>
                                );
                            })()}
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><BarChart2 size={14} /><span>Analytics</span></div>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.trackPlayHistory} onChange={(e) => handleChange('trackPlayHistory', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Track play history</span>
                            </label>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><MessageCircle size={14} /><span>TTS Announcement Policy</span></div>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={!!settings.ttsAnnouncementsEnabled} onChange={(e) => handleChange('ttsAnnouncementsEnabled', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Enable announcements</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={!!settings.ttsAnnounceHourlyTime} onChange={(e) => handleChange('ttsAnnounceHourlyTime', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Announce each hour</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={settings.ttsWarnNoContent3Min !== false} onChange={(e) => handleChange('ttsWarnNoContent3Min', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Warn 3 minutes before empty hour</span>
                            </label>
                            <label className={styles.toggle}>
                                <input type="checkbox" checked={!!settings.ttsAnnounceQueuedStarts} onChange={(e) => handleChange('ttsAnnounceQueuedStarts', e.target.checked)} />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Announce queued free-play starts</span>
                            </label>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Settings size={14} /><span>System</span></div>
                            <label className={styles.toggle}>
                                <input
                                    type="checkbox"
                                    checked={autostart}
                                    onChange={async (e) => {
                                        const next = e.target.checked;
                                        setAutostart(next);
                                        const api = (window as any).electronAPI;
                                        if (api?.setAutostart) {
                                            const r = await api.setAutostart(next);
                                            if (r && r.ok === false) {
                                                console.error('[Settings] autostart failed:', r.error);
                                            }
                                        }
                                    }}
                                />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>Start with Windows on login</span>
                            </label>
                        </section>

                        <section className={styles.section}>
                            <div className={styles.sectionHeader}><Plug size={14} /><span>OpenRouter (AI)</span></div>
                            <p style={{ color: 'var(--text-secondary)', marginBottom: '0.75rem', fontSize: '0.85rem' }}>
                                Used by AI scheduling and content tools. Get a key at <span style={{ fontFamily: 'monospace' }}>openrouter.ai/keys</span>.
                                You can rely on <span style={{ fontFamily: 'monospace' }}>OPENROUTER_API_KEY</span> / <span style={{ fontFamily: 'monospace' }}>OPENROUTER_MODEL</span> on the Electron process instead of pasting here — see <span style={{ fontFamily: 'monospace' }}>docs/OPENROUTER.md</span>.
                            </p>
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>API Key</label>
                                <input
                                    type="password"
                                    className={styles.select}
                                    value={openRouterKey}
                                    onChange={(e) => {
                                        setOpenRouterKey(e.target.value);
                                        try {
                                            localStorage.setItem('openrouter_api_key', e.target.value);
                                            localStorage.setItem(OPENROUTER_LS_API_KEY_LEGACY, e.target.value);
                                        } catch { /* noop */ }
                                    }}
                                    placeholder="sk-or-v1-…"
                                    autoComplete="off"
                                    spellCheck={false}
                                />
                            </div>
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>Default Model</label>
                                <input
                                    type="text"
                                    className={styles.select}
                                    value={openRouterModel}
                                    onChange={(e) => {
                                        const v = e.target.value;
                                        setOpenRouterModel(v);
                                        try {
                                            if (v.trim()) localStorage.setItem('openrouter_model', v.trim());
                                            else localStorage.removeItem('openrouter_model');
                                        } catch { /* noop */ }
                                    }}
                                    placeholder={getOpenRouterModelPlaceholderHint()}
                                    spellCheck={false}
                                />
                                <p style={{ color: 'var(--text-secondary)', fontSize: '0.78rem', marginTop: '0.35rem' }}>
                                    Leave blank to use <span style={{ fontFamily: 'monospace' }}>OPENROUTER_MODEL</span> or the app built-in default (<span style={{ fontFamily: 'monospace' }}>docs/OPENROUTER.md</span>).
                                </p>
                            </div>
                            <label className={styles.toggle} style={{ marginTop: '0.75rem' }}>
                                <input
                                    type="checkbox"
                                    checked={hermesSupervisor}
                                    onChange={(e) => {
                                        const on = e.target.checked;
                                        setHermesSupervisor(on);
                                        try {
                                            localStorage.setItem(HERMES_SUPERVISOR_LS, on ? '1' : '0');
                                        } catch { /* noop */ }
                                    }}
                                />
                                <span className={styles.toggleSlider}></span>
                                <span className={styles.toggleLabel}>
                                    Hermes supervisor (in-app OpenRouter) — validate JSON plan and retry (max 3 calls)
                                </span>
                            </label>
                            {hermesSupervisorEnv && (
                                <p style={{ color: 'var(--text-secondary)', fontSize: '0.8rem', marginTop: '0.35rem' }}>
                                    Process env <span style={{ fontFamily: 'monospace' }}>PLAYPLAY_HERMES_SUPERVISOR=1</span> is set. Until you change this toggle, the effective default follows that flag; turning off stores an explicit off state.
                                </p>
                            )}
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>Hermes model override (optional)</label>
                                <input
                                    type="text"
                                    className={styles.select}
                                    value={hermesModelOverride}
                                    onChange={(e) => {
                                        const v = e.target.value;
                                        setHermesModelOverride(v);
                                        try {
                                            if (v.trim()) localStorage.setItem(HERMES_MODEL_OVERRIDE_LS, v.trim());
                                            else localStorage.removeItem(HERMES_MODEL_OVERRIDE_LS);
                                        } catch { /* noop */ }
                                    }}
                                    placeholder="Blank = use Settings model, else OPENROUTER_MODEL / built-in"
                                    spellCheck={false}
                                />
                            </div>
                            <p style={{ color: 'var(--text-secondary)', fontSize: '0.8rem', marginTop: '0.35rem' }}>
                                Internal supervisor: <span style={{ fontFamily: 'monospace' }}>docs/HERMES_SUPERVISOR.md</span>. Nous Hermes Agent CLI: <span style={{ fontFamily: 'monospace' }}>docs/HERMES_NOUS_AGENT.md</span>. Last run: <span style={{ fontFamily: 'monospace' }}>localStorage.playplay_hermes_last_run</span>; learnings: <span style={{ fontFamily: 'monospace' }}>userData/playout-logs/hermes-supervisor-learning.log</span>.
                            </p>
                        </section>
                    </>
                )}

                {/* Connections Tab */}
                {activeTab === 'connections' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><Plug size={14} /><span>Connection Settings</span></div>

                        <div className={styles.connectionsGrid}>
                            {/* OBS */}
                            <div className={styles.connectionBox}>
                                <h4>🎬 OBS WebSocket</h4>
                                <div style={{ display: 'grid', gridTemplateColumns: '1fr 100px', gap: '1rem' }}>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Host</label>
                                        <input type="text" className={styles.select} value={connections.obs.host} onChange={(e) => updateConnection('obs', 'host', e.target.value)} />
                                    </div>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Port</label>
                                        <input type="number" className={styles.numberInput} value={connections.obs.port} onChange={(e) => updateConnection('obs', 'port', parseInt(e.target.value))} />
                                    </div>
                                </div>
                                <div className={styles.inputGroup}>
                                    <label className={styles.inputLabel}>Password</label>
                                    <input type="password" className={styles.select} value={connections.obs.password} onChange={(e) => updateConnection('obs', 'password', e.target.value)} placeholder="Optional" />
                                </div>
                                <button onClick={() => testConnection('obs')} disabled={loading} style={{ marginTop: 'auto', padding: '0.5rem 1rem', background: 'var(--primary)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer' }}>
                                    Test Connection
                                </button>
                                {testResults.obs && (
                                    <div style={{ marginTop: '0.5rem', padding: '0.5rem', borderRadius: '4px', background: testResults.obs.success ? 'rgba(16,185,129,0.1)' : 'rgba(239,68,68,0.1)', color: testResults.obs.success ? '#10b981' : '#ef4444' }}>
                                        {testResults.obs.success ? '✓' : '✗'} {testResults.obs.message}
                                    </div>
                                )}
                            </div>

                            {/* CasperCG */}
                            <div className={styles.connectionBox}>
                                <h4>📺 CasperCG</h4>
                                <div style={{ display: 'grid', gridTemplateColumns: '1fr 100px', gap: '1rem' }}>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Host</label>
                                        <input type="text" className={styles.select} value={connections.caspercg.host} onChange={(e) => updateConnection('caspercg', 'host', e.target.value)} />
                                    </div>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Port</label>
                                        <input type="number" className={styles.numberInput} value={connections.caspercg.port} onChange={(e) => updateConnection('caspercg', 'port', parseInt(e.target.value))} />
                                    </div>
                                </div>
                                <button onClick={() => testConnection('caspercg')} disabled={loading} style={{ marginTop: 'auto', padding: '0.5rem 1rem', background: 'var(--primary)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer' }}>
                                    Test Connection
                                </button>
                                {testResults.caspercg && (
                                    <div style={{ marginTop: '0.5rem', padding: '0.5rem', borderRadius: '4px', background: testResults.caspercg.success ? 'rgba(16,185,129,0.1)' : 'rgba(239,68,68,0.1)', color: testResults.caspercg.success ? '#10b981' : '#ef4444' }}>
                                        {testResults.caspercg.success ? '✓' : '✗'} {testResults.caspercg.message}
                                    </div>
                                )}
                            </div>

                            {/* FFplayout */}
                            <div className={styles.connectionBox}>
                                <h4>📼 FFplayout API</h4>
                                <div className={styles.inputGroup}>
                                    <label className={styles.inputLabel}>API URL</label>
                                    <input type="text" className={styles.select} value={connections.ffplayout.apiUrl} onChange={(e) => updateConnection('ffplayout', 'apiUrl', e.target.value)} />
                                </div>
                                <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '1rem' }}>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Username</label>
                                        <input type="text" className={styles.select} value={connections.ffplayout.username} onChange={(e) => updateConnection('ffplayout', 'username', e.target.value)} />
                                    </div>
                                    <div className={styles.inputGroup}>
                                        <label className={styles.inputLabel}>Password</label>
                                        <input type="password" className={styles.select} value={connections.ffplayout.password} onChange={(e) => updateConnection('ffplayout', 'password', e.target.value)} />
                                    </div>
                                </div>
                                <button onClick={() => testConnection('ffplayout')} disabled={loading} style={{ marginTop: 'auto', padding: '0.5rem 1rem', background: 'var(--primary)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer' }}>
                                    Test Connection
                                </button>
                                {testResults.ffplayout && (
                                    <div style={{ marginTop: '0.5rem', padding: '0.5rem', borderRadius: '4px', background: testResults.ffplayout.success ? 'rgba(16,185,129,0.1)' : 'rgba(239,68,68,0.1)', color: testResults.ffplayout.success ? '#10b981' : '#ef4444' }}>
                                        {testResults.ffplayout.success ? '✓' : '✗'} {testResults.ffplayout.message}
                                    </div>
                                )}
                            </div>

                            {/* PHP API */}
                            <div className={styles.connectionBox}>
                                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                                    <h4>🌐 PHP Tracker API</h4>
                                    <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                                        {trackerConnected ? (
                                            <Wifi size={16} style={{ color: '#10b981' }} />
                                        ) : (
                                            <WifiOff size={16} style={{ color: '#ef4444' }} />
                                        )}
                                        <span style={{ fontSize: '0.85rem', color: trackerConnected ? '#10b981' : '#ef4444' }}>
                                            {trackerStatus}
                                        </span>
                                    </div>
                                </div>
                                
                                <div className={styles.inputGroup}>
                                    <label className={styles.inputLabel}>Tracker URL</label>
                                    <input type="text" className={styles.select} value={connections.phpApi.url} onChange={(e) => updateConnection('phpApi', 'url', e.target.value)} placeholder="http://yoursite.com/tracker.php" />
                                </div>
                                <div className={styles.inputGroup}>
                                    <label className={styles.inputLabel}>API Key (optional)</label>
                                    <input type="password" className={styles.select} value={connections.phpApi.apiKey} onChange={(e) => updateConnection('phpApi', 'apiKey', e.target.value)} placeholder="Optional API key" />
                                </div>
                                
                                <div style={{ display: 'flex', gap: '8px', marginTop: 'auto' }}>
                                    <button 
                                        onClick={handleTrackerConnect} 
                                        disabled={loading || !connections.phpApi.enabled}
                                        style={{ 
                                            flex: 1,
                                            padding: '0.5rem', 
                                            background: trackerConnected ? '#ef4444' : '#10b981', 
                                            color: 'white', 
                                            border: 'none', 
                                            borderRadius: '6px', 
                                            cursor: 'pointer',
                                            fontSize: '0.8rem'
                                        }}
                                    >
                                        {trackerConnected ? 'Disconnect' : 'Connect'}
                                    </button>
                                    <button onClick={() => testConnection('phpapi')} disabled={loading} style={{ flex: 1, padding: '0.5rem', background: 'var(--primary)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer', fontSize: '0.8rem' }}>
                                        Test
                                    </button>
                                </div>
                                
                                <label className={styles.toggle} style={{ marginTop: '0.5rem' }}>
                                    <input type="checkbox" checked={connections.phpApi.enabled} onChange={(e) => updateConnection('phpApi', 'enabled', e.target.checked)} />
                                    <span className={styles.toggleSlider}></span>
                                    <span className={styles.toggleLabel}>Enable API sync</span>
                                </label>

                                {testResults.phpapi && (
                                    <div style={{ marginTop: '0.5rem', padding: '0.5rem', borderRadius: '4px', background: testResults.phpapi.success ? 'rgba(16,185,129,0.1)' : 'rgba(239,68,68,0.1)', color: testResults.phpapi.success ? '#10b981' : '#ef4444' }}>
                                        {testResults.phpapi.success ? '✓' : '✗'} {testResults.phpapi.message}
                                    </div>
                                )}
                            </div>
                        </div>

                        {hostname && (
                            <div style={{ marginTop: '1.5rem', padding: '1rem', background: 'var(--surface-highlight)', borderRadius: '10px', border: '1px solid var(--border)', fontSize: '0.9rem' }}>
                                <div style={{ marginBottom: '0.75rem', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                                    <span><strong>Current Instance:</strong> {hostname}</span>
                                    {instanceMapping.siteId && (
                                        <span style={{ color: 'var(--accent)', fontWeight: 600 }}>
                                            Active: {instanceMapping.siteId}
                                            {instanceMapping.playerId && ` (${instanceMapping.playerId})`}
                                        </span>
                                    )}
                                </div>
                                <div style={{ display: 'flex', gap: '0.75rem' }}>
                                    <select 
                                        value={instanceMapping.siteId || ''} 
                                        onChange={(e) => saveInstanceMapping(e.target.value || undefined, instanceMapping.playerId)}
                                        style={{ padding: '0.5rem', borderRadius: '6px', border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text-primary)', fontSize: '0.85rem', flex: 1 }}
                                    >
                                        <option value="">Select Site...</option>
                                        <option value="wgsr">WGSR</option>
                                        <option value="wmdv">WMDV</option>
                                        <option value="wyat">WYAT</option>
                                        <option value="starnews">StarNews.plus</option>
                                        <option value="happyradio">HappyRadio.plus</option>
                                        <option value="remote-node">Remote Node</option>
                                    </select>
                                    <input 
                                        type="text" 
                                        placeholder="Player ID (optional)" 
                                        value={instanceMapping.playerId || ''} 
                                        onChange={(e) => saveInstanceMapping(instanceMapping.siteId, e.target.value || undefined)}
                                        style={{ padding: '0.5rem', borderRadius: '6px', border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text-primary)', fontSize: '0.85rem', flex: 1 }}
                                    />
                                </div>
                            </div>
                        )}
                    </section>
                )}

                {/* OBS Commands Tab */}
                {activeTab === 'obs_commands' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><Zap size={14} /><span>OBS Commands Management</span></div>
                        <p style={{ color: 'var(--text-secondary)', marginBottom: '1rem', fontSize: '0.85rem' }}>
                            Configure reusable commands for OBS control. These can be attached to playlist items.
                        </p>
                        <div style={{ padding: '2rem', textAlign: 'center', background: 'var(--surface)', borderRadius: '8px', border: '1px dashed var(--border)' }}>
                            <Zap size={32} style={{ color: 'var(--accent)', marginBottom: '1rem', opacity: 0.5 }} />
                            <p>OBS Command management is currently handled in the dedicated OBS Controller panel.</p>
                            <button 
                                onClick={() => {
                                    if (onClose) onClose();
                                    // Trigger navigation or open sidebar
                                    window.dispatchEvent(new CustomEvent('open-side-panel', { detail: { panel: 'obs' } }));
                                }}
                                style={{ marginTop: '1rem', padding: '0.5rem 1.5rem', background: 'var(--accent)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer' }}
                            >
                                Open OBS Controller
                            </button>
                        </div>
                    </section>
                )}

                {/* Social Tab */}
                {activeTab === 'social' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><Share2 size={14} /><span>Social Media & Webhooks</span></div>
                        <p style={{ color: 'var(--text-secondary)', marginBottom: '1rem', fontSize: '0.85rem' }}>
                            Configure automated social media posting and external webhook notifications.
                        </p>
                        
                        <div className={styles.inputGroup}>
                            <label className={styles.inputLabel}>Facebook Webhook URL</label>
                            <input 
                                type="text" 
                                className={styles.select} 
                                value={settings.facebookWebhookUrl || ''} 
                                onChange={(e) => handleChange('facebookWebhookUrl', e.target.value)} 
                                placeholder="https://your-api.com/fb-webhook"
                            />
                        </div>

                        <label className={styles.toggle}>
                            <input 
                                type="checkbox" 
                                checked={settings.facebookWebhookEnabled !== false} 
                                onChange={(e) => handleChange('facebookWebhookEnabled', e.target.checked)} 
                            />
                            <span className={styles.toggleSlider}></span>
                            <span className={styles.toggleLabel}>Enable webhook notifications</span>
                        </label>

                        <div style={{ marginTop: '1rem', borderTop: '1px solid var(--border)', paddingTop: '1rem' }}>
                            <h4 style={{ fontSize: '0.9rem', marginBottom: '0.5rem' }}>Facebook Graph API (Native)</h4>
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>Page ID</label>
                                <input 
                                    type="text" 
                                    className={styles.select} 
                                    value={settings.facebookGraphPageId || ''} 
                                    onChange={(e) => handleChange('facebookGraphPageId', e.target.value)} 
                                />
                            </div>
                            <div className={styles.inputGroup} style={{ marginTop: '0.5rem' }}>
                                <label className={styles.inputLabel}>Access Token</label>
                                <input 
                                    type="password" 
                                    className={styles.select} 
                                    value={settings.facebookGraphAccessToken || ''} 
                                    onChange={(e) => handleChange('facebookGraphAccessToken', e.target.value)} 
                                />
                            </div>
                        </div>

                        <div style={{ marginTop: '1rem', borderTop: '1px solid var(--border)', paddingTop: '1rem' }}>
                            <h4 style={{ fontSize: '0.9rem', marginBottom: '0.5rem' }}>AI Integration</h4>
                            <div className={styles.inputGroup}>
                                <label className={styles.inputLabel}>OpenAI API Key (for post generation)</label>
                                <input 
                                    type="password" 
                                    className={styles.select} 
                                    value={settings.openaiApiKey || ''} 
                                    onChange={(e) => handleChange('openaiApiKey', e.target.value)} 
                                />
                            </div>
                        </div>
                    </section>
                )}

                {/* Logs Tab */}
                {activeTab === 'logs' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><FileText size={14} /><span>Application Logs</span></div>
                        <div style={{ marginBottom: '1rem', display: 'flex', gap: '0.5rem' }}>
                            <button onClick={loadLogs} style={{ padding: '0.5rem 1rem', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: '6px', cursor: 'pointer' }}>
                                <RefreshCw size={14} style={{ marginRight: '4px' }} />
                                Refresh
                            </button>
                            <button onClick={() => {
                                localStorage.removeItem('app_logs');
                                setLogs([]);
                            }} style={{ padding: '0.5rem 1rem', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: '6px', cursor: 'pointer' }}>
                                Clear Logs
                            </button>
                        </div>
                        <div style={{ 
                            background: 'var(--surface)', 
                            borderRadius: '8px', 
                            padding: '1rem', 
                            maxHeight: '500px', 
                            overflowY: 'auto',
                            fontFamily: 'monospace',
                            fontSize: '0.8rem'
                        }}>
                            {logs.length === 0 ? (
                                <div style={{ color: 'var(--text-secondary)', textAlign: 'center', padding: '2rem' }}>
                                    No logs available
                                </div>
                            ) : (
                                logs.map((log, idx) => (
                                    <div key={idx} style={{ 
                                        padding: '0.5rem', 
                                        borderBottom: '1px solid var(--border)',
                                        color: log.level === 'error' ? '#ef4444' : log.level === 'warning' ? '#f59e0b' : 'var(--text-primary)'
                                    }}>
                                        <span style={{ color: 'var(--text-secondary)', marginRight: '0.5rem' }}>
                                            {new Date(log.timestamp).toLocaleString()}
                                        </span>
                                        <span style={{ 
                                            marginRight: '0.5rem',
                                            fontWeight: 'bold',
                                            color: log.level === 'error' ? '#ef4444' : log.level === 'warning' ? '#f59e0b' : '#3b82f6'
                                        }}>
                                            [{log.level.toUpperCase()}]
                                        </span>
                                        {log.source && (
                                            <span style={{ color: 'var(--text-secondary)', marginRight: '0.5rem' }}>
                                                {log.source}:
                                            </span>
                                        )}
                                        <span>{log.message}</span>
                                    </div>
                                ))
                            )}
                        </div>
                    </section>
                )}

                {/* Channels Tab */}
                {activeTab === 'channels' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><Tv2 size={14} /><span>Channel Outputs</span></div>
                        <p style={{ color: 'var(--text-secondary)', marginBottom: '1rem', fontSize: '0.85rem' }}>
                            Configure 8 channels. CH 1–4 are active by default. Each channel maps to a CasparCG channel and can output NDI, SRT, RTMP, and HLS simultaneously.
                        </p>
                        <ChannelsPanel />
                    </section>
                )}

                {/* Streams Tab */}
                {activeTab === 'streams' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><Radio size={14} /><span>Stream Sources &amp; Destinations</span></div>
                        <p style={{ color: 'var(--text-secondary)', marginBottom: '1rem', fontSize: '0.85rem' }}>
                            Configure stream inputs (pull into playlist) and outputs (push to platform). Drag any entry onto the playlist to queue it.
                        </p>

                        {/* Inputs */}
                        <div style={{ marginBottom: '1.5rem' }}>
                            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: '0.75rem' }}>
                                <span style={{ fontWeight: 700, fontSize: '0.8rem', textTransform: 'uppercase', letterSpacing: '0.06em', color: 'var(--text-secondary)' }}>Stream Inputs</span>
                                <button onClick={() => addStream('in')} style={{ padding: '4px 12px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, color: 'var(--text-primary)', cursor: 'pointer', fontSize: '0.8rem' }}>+ Add Input</button>
                            </div>
                            {streams.filter(s => s.direction === 'in').length === 0 && (
                                <div style={{ padding: '1rem', textAlign: 'center', color: 'var(--text-secondary)', fontSize: '0.8rem', background: 'var(--surface)', borderRadius: 8, border: '1px dashed var(--border)' }}>
                                    No stream inputs. Add an RTMP, SRT, NDI or HLS source.
                                </div>
                            )}
                            {streams.filter(s => s.direction === 'in').map(entry => (
                                <StreamCard key={entry.id} entry={entry} onUpdate={patch => updateStream(entry.id, patch)} onRemove={() => removeStream(entry.id)} />
                            ))}
                        </div>

                        {/* Outputs */}
                        <div>
                            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: '0.75rem' }}>
                                <span style={{ fontWeight: 700, fontSize: '0.8rem', textTransform: 'uppercase', letterSpacing: '0.06em', color: 'var(--text-secondary)' }}>Stream Outputs</span>
                                <button onClick={() => addStream('out')} style={{ padding: '4px 12px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, color: 'var(--text-primary)', cursor: 'pointer', fontSize: '0.8rem' }}>+ Add Output</button>
                            </div>
                            {streams.filter(s => s.direction === 'out').length === 0 && (
                                <div style={{ padding: '1rem', textAlign: 'center', color: 'var(--text-secondary)', fontSize: '0.8rem', background: 'var(--surface)', borderRadius: 8, border: '1px dashed var(--border)' }}>
                                    No stream outputs. Add an RTMP destination (YouTube, Facebook, Twitch…).
                                </div>
                            )}
                            {streams.filter(s => s.direction === 'out').map(entry => (
                                <StreamCard key={entry.id} entry={entry} onUpdate={patch => updateStream(entry.id, patch)} onRemove={() => removeStream(entry.id)} />
                            ))}
                        </div>
                    </section>
                )}

                {/* Dev Notes Tab */}
                {activeTab === 'devnotes' && (
                    <section className={styles.section}>
                        <div className={styles.sectionHeader}><BookOpen size={14} /><span>Developer Notes</span></div>
                        <p style={{ color: 'var(--text-secondary)', marginBottom: '1rem', fontSize: '0.85rem' }}>
                            Internal documentation and notes for development team.
                        </p>
                        <textarea
                            value={devNotes}
                            onChange={(e) => setDevNotes(e.target.value)}
                            onBlur={saveDevNotes}
                            placeholder="Enter developer notes here..."
                            style={{
                                width: '100%',
                                minHeight: '400px',
                                padding: '1rem',
                                background: 'var(--surface)',
                                border: '1px solid var(--border)',
                                borderRadius: '8px',
                                color: 'var(--text-primary)',
                                fontFamily: 'monospace',
                                fontSize: '0.9rem',
                                resize: 'vertical'
                            }}
                        />
                        <div style={{ marginTop: '1rem', display: 'flex', gap: '0.5rem' }}>
                            <button onClick={saveDevNotes} style={{ padding: '0.5rem 1rem', background: 'var(--primary)', color: 'white', border: 'none', borderRadius: '6px', cursor: 'pointer' }}>
                                <Save size={14} style={{ marginRight: '4px' }} />
                                Save Notes
                            </button>
                        </div>
                    </section>
                )}
            </div>

            <div className={styles.footer}>
                <button className={styles.resetBtn} onClick={handleReset}>
                    <RotateCcw size={14} /> Reset
                </button>
                <button className={styles.saveBtn} onClick={handleSave} disabled={!hasChanges}>
                    <Save size={14} /> Save Settings
                </button>
            </div>
        </div>
    );
}

// ---- StreamCard sub-component ----
type StreamProtocolOuter = 'RTMP' | 'RTSP' | 'SRT' | 'NDI' | 'HLS' | 'UDP';
interface StreamEntryOuter { id: string; label: string; url: string; protocol: StreamProtocolOuter; direction: 'in' | 'out'; enabled: boolean; }

const PROTOCOL_COLORS: Record<StreamProtocolOuter, string> = {
    RTMP: '#ef4444', RTSP: '#f97316', SRT: '#a855f7', NDI: '#22c55e', HLS: '#3b82f6', UDP: '#eab308',
};

function StreamCard({ entry, onUpdate, onRemove }: { entry: StreamEntryOuter; onUpdate: (p: Partial<StreamEntryOuter>) => void; onRemove: () => void }) {
    const protocols: StreamProtocolOuter[] = ['RTMP', 'RTSP', 'SRT', 'NDI', 'HLS', 'UDP'];
    const color = PROTOCOL_COLORS[entry.protocol];

    const handleDragStart = (e: React.DragEvent) => {
        const item = { id: entry.id, label: entry.label, url: entry.url, type: 'stream', protocol: entry.protocol, direction: entry.direction };
        e.dataTransfer.setData('application/playplay-stream', JSON.stringify(item));
        e.dataTransfer.setData('text/plain', entry.label);
    };

    return (
        <div
            draggable
            onDragStart={handleDragStart}
            style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', marginBottom: 6, background: 'var(--surface)', border: `1px solid ${entry.enabled ? color : 'var(--border)'}`, borderRadius: 8, cursor: 'grab' }}
        >
            <span style={{ padding: '2px 8px', borderRadius: 999, background: color, color: '#fff', fontSize: '0.65rem', fontWeight: 800, letterSpacing: '0.04em', minWidth: 44, textAlign: 'center' }}>
                {entry.protocol}
            </span>
            <input
                value={entry.label}
                onChange={e => onUpdate({ label: e.target.value })}
                placeholder="Label"
                style={{ width: 110, padding: '3px 6px', background: 'transparent', border: '1px solid var(--border)', borderRadius: 4, color: 'var(--text-primary)', fontSize: '0.8rem' }}
            />
            <input
                value={entry.url}
                onChange={e => onUpdate({ url: e.target.value })}
                placeholder={entry.direction === 'in' ? 'rtmp://…' : 'rtmp://…/stream-key'}
                style={{ flex: 1, padding: '3px 6px', background: 'transparent', border: '1px solid var(--border)', borderRadius: 4, color: 'var(--text-primary)', fontSize: '0.8rem', fontFamily: 'monospace' }}
            />
            <select
                value={entry.protocol}
                onChange={e => onUpdate({ protocol: e.target.value as StreamProtocolOuter })}
                style={{ padding: '3px 6px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 4, color: 'var(--text-primary)', fontSize: '0.75rem' }}
            >
                {protocols.map(p => <option key={p} value={p}>{p}</option>)}
            </select>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: '0.75rem', color: 'var(--text-secondary)', cursor: 'pointer', userSelect: 'none' }}>
                <input type="checkbox" checked={entry.enabled} onChange={e => onUpdate({ enabled: e.target.checked })} />
                Live
            </label>
            <button onClick={onRemove} title="Remove" style={{ background: 'none', border: 'none', color: 'var(--text-secondary)', cursor: 'pointer', padding: '2px 4px', fontSize: '0.9rem' }}>✕</button>
        </div>
    );
}
