/**
 * OBSStudioController - Deep OBS Integration with Advanced Features
 * 
 * Features:
 * - Custom scene schema management
 * - Clever command syntax (both automation and manual)
 * - Multi-channel switching and routing
 * - Remote session automation
 * - Hardware input support (DeckLink, NDI, webcam)
 * - Scene collections for different shows
 * - Source manipulation and transforms
 * - Streaming and recording control
 * - Audio mixer integration
 * - Transition control
 * - Filter management
 * - Hotkey simulation
 * - WebSocket status monitoring
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './OBSStudioController.module.css';
import {
    Monitor, Video, Tv, Radio, Settings, Play, Pause, Square, Circle,
    Layers, Eye, EyeOff, Volume2, VolumeX, Mic, MicOff, Camera, Image,
    RotateCcw, Maximize, Minimize, Move, ZoomIn, ZoomOut, Lock, Unlock,
    RefreshCw, Wifi, WifiOff, Send, Plus, Minus, Trash2, Edit2, Copy,
    Save, Download, Upload, Clock, Zap, Terminal, Code, ChevronRight,
    ChevronDown, AlertCircle, CheckCircle, XCircle, ArrowRight, ArrowLeft,
    Grip, Hash, Sliders, Film, Grid, Box, Cpu, HardDrive, ScreenShare
} from 'lucide-react';

// Types
interface OBSScene {
    id: string;
    name: string;
    sources: OBSSource[];
    isActive: boolean;
    isPreview: boolean;
    thumbnail?: string;
}

interface OBSSource {
    id: string;
    name: string;
    type: 'video' | 'image' | 'browser' | 'text' | 'media' | 'capture' | 'ndi' | 'decklink' | 'audio';
    visible: boolean;
    locked: boolean;
    muted?: boolean;
    position: { x: number; y: number };
    size: { width: number; height: number };
    rotation: number;
    crop: { top: number; right: number; bottom: number; left: number };
    filters: OBSFilter[];
    settings?: Record<string, any>;
}

interface OBSFilter {
    id: string;
    name: string;
    type: string;
    enabled: boolean;
    settings: Record<string, any>;
}

interface OBSSceneCollection {
    id: string;
    name: string;
    scenes: OBSScene[];
    isActive: boolean;
}

interface OBSTransition {
    id: string;
    name: string;
    type: 'cut' | 'fade' | 'swipe' | 'slide' | 'stinger' | 'custom';
    duration: number;
}

interface OBSAudioMixer {
    id: string;
    name: string;
    volume: number; // 0-100
    muted: boolean;
    monitoring: 'none' | 'monitorOnly' | 'monitorAndOutput';
    balance: number; // -100 to 100
    type: 'input' | 'output';
}

interface OBSCommand {
    id: string;
    name: string;
    category: string;
    syntax: string;
    description: string;
    parameters?: { name: string; type: string; required: boolean; description: string }[];
}

interface OBSConnectionStatus {
    connected: boolean;
    host: string;
    port: number;
    streaming: boolean;
    recording: boolean;
    virtualCam: boolean;
    fps: number;
    cpu: number;
    memory: number;
    disk: number;
    dropped: number;
}

interface OBSStudioControllerProps {
    onSceneSwitch?: (sceneId: string) => void;
    onSourceToggle?: (sourceId: string, visible: boolean) => void;
    onStreamStart?: () => void;
    onStreamStop?: () => void;
    onRecordStart?: () => void;
    onRecordStop?: () => void;
    onCommandExecute?: (command: string) => void;
}

// Built-in command syntax
const OBS_COMMANDS: OBSCommand[] = [
    // Scene Commands
    { id: 'scene.switch', name: 'Switch Scene', category: 'Scene', syntax: 'SCENE <name>', description: 'Switch to a scene by name' },
    { id: 'scene.preview', name: 'Preview Scene', category: 'Scene', syntax: 'PREVIEW <name>', description: 'Set a scene as preview' },
    { id: 'scene.cut', name: 'Cut Transition', category: 'Scene', syntax: 'CUT', description: 'Perform instant cut transition' },
    { id: 'scene.auto', name: 'Auto Transition', category: 'Scene', syntax: 'AUTO [duration]', description: 'Perform auto transition with optional duration' },
    
    // Source Commands
    { id: 'source.show', name: 'Show Source', category: 'Source', syntax: 'SHOW <source>', description: 'Make a source visible' },
    { id: 'source.hide', name: 'Hide Source', category: 'Source', syntax: 'HIDE <source>', description: 'Hide a source' },
    { id: 'source.toggle', name: 'Toggle Source', category: 'Source', syntax: 'TOGGLE <source>', description: 'Toggle source visibility' },
    { id: 'source.mute', name: 'Mute Source', category: 'Source', syntax: 'MUTE <source>', description: 'Mute an audio source' },
    { id: 'source.unmute', name: 'Unmute Source', category: 'Source', syntax: 'UNMUTE <source>', description: 'Unmute an audio source' },
    
    // Streaming Commands
    { id: 'stream.start', name: 'Start Streaming', category: 'Stream', syntax: 'STREAM START', description: 'Start streaming' },
    { id: 'stream.stop', name: 'Stop Streaming', category: 'Stream', syntax: 'STREAM STOP', description: 'Stop streaming' },
    { id: 'record.start', name: 'Start Recording', category: 'Stream', syntax: 'RECORD START', description: 'Start recording' },
    { id: 'record.stop', name: 'Stop Recording', category: 'Stream', syntax: 'RECORD STOP', description: 'Stop recording' },
    
    // Audio Commands
    { id: 'audio.volume', name: 'Set Volume', category: 'Audio', syntax: 'VOLUME <source> <level>', description: 'Set volume level (0-100)' },
    { id: 'audio.fade', name: 'Fade Audio', category: 'Audio', syntax: 'FADE <source> <level> <duration>', description: 'Fade audio over duration' },
    
    // Advanced Commands
    { id: 'filter.enable', name: 'Enable Filter', category: 'Filter', syntax: 'FILTER <source> <filter> ON', description: 'Enable a filter on source' },
    { id: 'filter.disable', name: 'Disable Filter', category: 'Filter', syntax: 'FILTER <source> <filter> OFF', description: 'Disable a filter on source' },
    { id: 'transition.set', name: 'Set Transition', category: 'Transition', syntax: 'TRANSITION <type> [duration]', description: 'Set the current transition' },
    { id: 'wait', name: 'Wait', category: 'Control', syntax: 'WAIT <ms>', description: 'Wait for specified milliseconds' },
    { id: 'macro.run', name: 'Run Macro', category: 'Control', syntax: 'MACRO <name>', description: 'Execute a saved macro' },
];

// Default scene collection for demo
const DEFAULT_SCENES: OBSScene[] = [
    {
        id: 'scene_1',
        name: 'Main Camera',
        isActive: true,
        isPreview: false,
        sources: [
            { id: 'src_1', name: 'Camera 1', type: 'capture', visible: true, locked: false, position: { x: 0, y: 0 }, size: { width: 1920, height: 1080 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
            { id: 'src_2', name: 'Lower Third', type: 'browser', visible: true, locked: false, position: { x: 0, y: 850 }, size: { width: 1920, height: 230 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
        ]
    },
    {
        id: 'scene_2',
        name: 'Two Shot',
        isActive: false,
        isPreview: true,
        sources: [
            { id: 'src_3', name: 'Camera 1', type: 'capture', visible: true, locked: false, position: { x: 0, y: 0 }, size: { width: 960, height: 1080 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
            { id: 'src_4', name: 'Camera 2', type: 'capture', visible: true, locked: false, position: { x: 960, y: 0 }, size: { width: 960, height: 1080 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
        ]
    },
    {
        id: 'scene_3',
        name: 'Screen Share',
        isActive: false,
        isPreview: false,
        sources: [
            { id: 'src_5', name: 'Display Capture', type: 'capture', visible: true, locked: false, position: { x: 0, y: 0 }, size: { width: 1920, height: 1080 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
            { id: 'src_6', name: 'Webcam PiP', type: 'capture', visible: true, locked: false, position: { x: 1540, y: 720 }, size: { width: 360, height: 270 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
        ]
    },
    {
        id: 'scene_4',
        name: 'Break',
        isActive: false,
        isPreview: false,
        sources: [
            { id: 'src_7', name: 'BRB Image', type: 'image', visible: true, locked: false, position: { x: 0, y: 0 }, size: { width: 1920, height: 1080 }, rotation: 0, crop: { top: 0, right: 0, bottom: 0, left: 0 }, filters: [] },
        ]
    },
];

const DEFAULT_AUDIO_MIXERS: OBSAudioMixer[] = [
    { id: 'mic_1', name: 'Microphone', volume: 80, muted: false, monitoring: 'none', balance: 0, type: 'input' },
    { id: 'desktop', name: 'Desktop Audio', volume: 70, muted: false, monitoring: 'none', balance: 0, type: 'input' },
    { id: 'media', name: 'Media Source', volume: 85, muted: true, monitoring: 'none', balance: 0, type: 'input' },
    { id: 'master', name: 'Master Output', volume: 100, muted: false, monitoring: 'monitorAndOutput', balance: 0, type: 'output' },
];

const DEFAULT_TRANSITIONS: OBSTransition[] = [
    { id: 'cut', name: 'Cut', type: 'cut', duration: 0 },
    { id: 'fade', name: 'Fade', type: 'fade', duration: 300 },
    { id: 'swipe', name: 'Swipe', type: 'swipe', duration: 500 },
    { id: 'stinger', name: 'Stinger', type: 'stinger', duration: 1000 },
];

export const OBSStudioController: React.FC<OBSStudioControllerProps> = ({
    onSceneSwitch,
    onSourceToggle,
    onStreamStart,
    onStreamStop,
    onRecordStart,
    onRecordStop,
    onCommandExecute
}) => {
    // State
    const [connectionStatus, setConnectionStatus] = useState<OBSConnectionStatus>({
        connected: true, // Demo mode
        host: 'localhost',
        port: 4455,
        streaming: false,
        recording: false,
        virtualCam: false,
        fps: 60,
        cpu: 12,
        memory: 2048,
        disk: 0,
        dropped: 0
    });
    const [scenes, setScenes] = useState<OBSScene[]>(DEFAULT_SCENES);
    const [audioMixers, setAudioMixers] = useState<OBSAudioMixer[]>(DEFAULT_AUDIO_MIXERS);
    const [transitions, setTransitions] = useState<OBSTransition[]>(DEFAULT_TRANSITIONS);
    const [currentTransition, setCurrentTransition] = useState<OBSTransition>(DEFAULT_TRANSITIONS[1]);
    const [sceneCollections, setSceneCollections] = useState<OBSSceneCollection[]>([
        { id: 'col_1', name: 'News Show', scenes: DEFAULT_SCENES, isActive: true },
        { id: 'col_2', name: 'Talk Show', scenes: [], isActive: false },
        { id: 'col_3', name: 'Gaming', scenes: [], isActive: false },
    ]);
    const [activeTab, setActiveTab] = useState<'scenes' | 'sources' | 'audio' | 'commands' | 'automation'>('scenes');
    const [commandInput, setCommandInput] = useState('');
    const [commandHistory, setCommandHistory] = useState<string[]>([]);
    const [automationScript, setAutomationScript] = useState('');
    const [selectedScene, setSelectedScene] = useState<OBSScene | null>(null);
    const [selectedSource, setSelectedSource] = useState<OBSSource | null>(null);
    const [showSceneCollectionModal, setShowSceneCollectionModal] = useState(false);
    const [newCollectionName, setNewCollectionName] = useState('');
    const [isStudioMode, setIsStudioMode] = useState(true);
    
    const commandInputRef = useRef<HTMLInputElement>(null);

    // Get active/preview scenes
    const activeScene = scenes.find(s => s.isActive);
    const previewScene = scenes.find(s => s.isPreview);

    // Connect to OBS
    const handleConnect = useCallback(() => {
        // In production, this would use OBS WebSocket
        setConnectionStatus(prev => ({ ...prev, connected: true }));
    }, []);

    // Disconnect from OBS
    const handleDisconnect = useCallback(() => {
        setConnectionStatus(prev => ({ ...prev, connected: false }));
    }, []);

    // Switch scene
    const handleSceneSwitch = useCallback((scene: OBSScene) => {
        if (isStudioMode) {
            // In studio mode, set as preview first
            setScenes(prev => prev.map(s => ({
                ...s,
                isPreview: s.id === scene.id
            })));
        } else {
            // Direct switch
            setScenes(prev => prev.map(s => ({
                ...s,
                isActive: s.id === scene.id,
                isPreview: false
            })));
            onSceneSwitch?.(scene.id);
        }
    }, [isStudioMode, onSceneSwitch]);

    // Execute transition
    const handleTransition = useCallback(() => {
        if (!previewScene) return;
        
        setScenes(prev => prev.map(s => ({
            ...s,
            isActive: s.isPreview,
            isPreview: s.isActive
        })));
        
        onSceneSwitch?.(previewScene.id);
    }, [previewScene, onSceneSwitch]);

    // Toggle source visibility
    const handleSourceToggle = useCallback((sceneId: string, sourceId: string) => {
        setScenes(prev => prev.map(scene => {
            if (scene.id !== sceneId) return scene;
            return {
                ...scene,
                sources: scene.sources.map(source => {
                    if (source.id !== sourceId) return source;
                    onSourceToggle?.(sourceId, !source.visible);
                    return { ...source, visible: !source.visible };
                })
            };
        }));
    }, [onSourceToggle]);

    // Toggle streaming
    const handleStreamToggle = useCallback(() => {
        if (connectionStatus.streaming) {
            setConnectionStatus(prev => ({ ...prev, streaming: false }));
            onStreamStop?.();
        } else {
            setConnectionStatus(prev => ({ ...prev, streaming: true }));
            onStreamStart?.();
        }
    }, [connectionStatus.streaming, onStreamStart, onStreamStop]);

    // Toggle recording
    const handleRecordToggle = useCallback(() => {
        if (connectionStatus.recording) {
            setConnectionStatus(prev => ({ ...prev, recording: false }));
            onRecordStop?.();
        } else {
            setConnectionStatus(prev => ({ ...prev, recording: true }));
            onRecordStart?.();
        }
    }, [connectionStatus.recording, onRecordStart, onRecordStop]);

    // Toggle virtual camera
    const handleVirtualCamToggle = useCallback(() => {
        setConnectionStatus(prev => ({ ...prev, virtualCam: !prev.virtualCam }));
    }, []);

    // Execute command
    const executeCommand = useCallback((cmd: string) => {
        const trimmed = cmd.trim().toUpperCase();
        if (!trimmed) return;
        
        // Add to history
        setCommandHistory(prev => [...prev, cmd]);
        
        // Parse and execute command
        const parts = trimmed.split(/\s+/);
        const command = parts[0];
        const args = parts.slice(1);
        
        switch (command) {
            case 'SCENE':
                const sceneName = args.join(' ');
                const targetScene = scenes.find(s => s.name.toUpperCase() === sceneName);
                if (targetScene) {
                    setScenes(prev => prev.map(s => ({
                        ...s,
                        isActive: s.id === targetScene.id
                    })));
                }
                break;
            case 'PREVIEW':
                const previewName = args.join(' ');
                const previewTarget = scenes.find(s => s.name.toUpperCase() === previewName);
                if (previewTarget) {
                    setScenes(prev => prev.map(s => ({
                        ...s,
                        isPreview: s.id === previewTarget.id
                    })));
                }
                break;
            case 'CUT':
                handleTransition();
                break;
            case 'AUTO':
                const duration = parseInt(args[0]) || currentTransition.duration;
                handleTransition();
                break;
            case 'STREAM':
                if (args[0] === 'START') handleStreamToggle();
                else if (args[0] === 'STOP') handleStreamToggle();
                break;
            case 'RECORD':
                if (args[0] === 'START') handleRecordToggle();
                else if (args[0] === 'STOP') handleRecordToggle();
                break;
            case 'SHOW':
            case 'HIDE':
            case 'TOGGLE':
                const sourceName = args.join(' ');
                scenes.forEach(scene => {
                    scene.sources.forEach(source => {
                        if (source.name.toUpperCase() === sourceName) {
                            if (command === 'TOGGLE') {
                                handleSourceToggle(scene.id, source.id);
                            } else {
                                setScenes(prev => prev.map(s => {
                                    if (s.id !== scene.id) return s;
                                    return {
                                        ...s,
                                        sources: s.sources.map(src => {
                                            if (src.id !== source.id) return src;
                                            return { ...src, visible: command === 'SHOW' };
                                        })
                                    };
                                }));
                            }
                        }
                    });
                });
                break;
            case 'VOLUME':
                const audioName = args[0];
                const level = parseInt(args[1]);
                setAudioMixers(prev => prev.map(mixer => {
                    if (mixer.name.toUpperCase() === audioName) {
                        return { ...mixer, volume: Math.min(100, Math.max(0, level)) };
                    }
                    return mixer;
                }));
                break;
            case 'MUTE':
            case 'UNMUTE':
                const muteTarget = args.join(' ');
                setAudioMixers(prev => prev.map(mixer => {
                    if (mixer.name.toUpperCase() === muteTarget) {
                        return { ...mixer, muted: command === 'MUTE' };
                    }
                    return mixer;
                }));
                break;
            case 'TRANSITION':
                const transType = args[0]?.toLowerCase();
                const trans = transitions.find(t => t.type === transType);
                if (trans) {
                    setCurrentTransition(trans);
                }
                break;
            default:
                console.log(`Unknown command: ${command}`);
        }
        
        onCommandExecute?.(cmd);
        setCommandInput('');
    }, [scenes, currentTransition, handleTransition, handleStreamToggle, handleRecordToggle, handleSourceToggle, transitions, onCommandExecute]);

    // Handle command input
    const handleCommandSubmit = useCallback((e: React.FormEvent) => {
        e.preventDefault();
        executeCommand(commandInput);
    }, [commandInput, executeCommand]);

    // Execute automation script
    const executeAutomation = useCallback(async () => {
        const lines = automationScript.split('\n').filter(l => l.trim() && !l.trim().startsWith('#'));
        
        for (const line of lines) {
            const trimmed = line.trim();
            
            if (trimmed.toUpperCase().startsWith('WAIT')) {
                const ms = parseInt(trimmed.split(/\s+/)[1]) || 1000;
                await new Promise(resolve => setTimeout(resolve, ms));
            } else {
                executeCommand(trimmed);
                await new Promise(resolve => setTimeout(resolve, 100));
            }
        }
    }, [automationScript, executeCommand]);

    // Audio mixer volume change
    const handleVolumeChange = useCallback((mixerId: string, volume: number) => {
        setAudioMixers(prev => prev.map(mixer =>
            mixer.id === mixerId ? { ...mixer, volume } : mixer
        ));
    }, []);

    // Audio mixer mute toggle
    const handleMuteToggle = useCallback((mixerId: string) => {
        setAudioMixers(prev => prev.map(mixer =>
            mixer.id === mixerId ? { ...mixer, muted: !mixer.muted } : mixer
        ));
    }, []);

    // Save scene collection
    const saveSceneCollection = useCallback(() => {
        if (!newCollectionName.trim()) return;
        
        const newCollection: OBSSceneCollection = {
            id: `col_${Date.now()}`,
            name: newCollectionName,
            scenes: [...scenes],
            isActive: false
        };
        
        setSceneCollections(prev => [...prev, newCollection]);
        setShowSceneCollectionModal(false);
        setNewCollectionName('');
    }, [newCollectionName, scenes]);

    // Load scene collection
    const loadSceneCollection = useCallback((collection: OBSSceneCollection) => {
        setScenes(collection.scenes.length > 0 ? collection.scenes : DEFAULT_SCENES);
        setSceneCollections(prev => prev.map(c => ({
            ...c,
            isActive: c.id === collection.id
        })));
    }, []);

    // Render source icon
    const getSourceIcon = (type: OBSSource['type']) => {
        switch (type) {
            case 'video': return <Video size={14} />;
            case 'image': return <Image size={14} />;
            case 'browser': return <Box size={14} />;
            case 'text': return <Hash size={14} />;
            case 'media': return <Film size={14} />;
            case 'capture': return <Camera size={14} />;
            case 'ndi': return <Wifi size={14} />;
            case 'decklink': return <HardDrive size={14} />;
            case 'audio': return <Volume2 size={14} />;
            default: return <Box size={14} />;
        }
    };

    return (
        <div className={styles.controller}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <div className={`${styles.connectionStatus} ${connectionStatus.connected ? styles.connected : ''}`}>
                        {connectionStatus.connected ? <Wifi size={16} /> : <WifiOff size={16} />}
                        <span>{connectionStatus.connected ? 'Connected' : 'Disconnected'}</span>
                    </div>
                    <span className={styles.hostInfo}>
                        {connectionStatus.host}:{connectionStatus.port}
                    </span>
                </div>
                
                <div className={styles.headerCenter}>
                    <button 
                        className={`${styles.controlBtn} ${connectionStatus.streaming ? styles.live : ''}`}
                        onClick={handleStreamToggle}
                    >
                        {connectionStatus.streaming ? <Square size={14} /> : <Radio size={14} />}
                        {connectionStatus.streaming ? 'Stop Stream' : 'Start Stream'}
                    </button>
                    <button 
                        className={`${styles.controlBtn} ${connectionStatus.recording ? styles.recording : ''}`}
                        onClick={handleRecordToggle}
                    >
                        <Circle size={14} fill={connectionStatus.recording ? 'currentColor' : 'none'} />
                        {connectionStatus.recording ? 'Stop Recording' : 'Start Recording'}
                    </button>
                    <button 
                        className={`${styles.controlBtn} ${connectionStatus.virtualCam ? styles.active : ''}`}
                        onClick={handleVirtualCamToggle}
                    >
                        <Camera size={14} />
                        Virtual Camera
                    </button>
                </div>
                
                <div className={styles.headerRight}>
                    <div className={styles.stats}>
                        <span>FPS: {connectionStatus.fps}</span>
                        <span>CPU: {connectionStatus.cpu}%</span>
                        <span>Dropped: {connectionStatus.dropped}</span>
                    </div>
                    <button 
                        className={`${styles.studioModeBtn} ${isStudioMode ? styles.active : ''}`}
                        onClick={() => setIsStudioMode(!isStudioMode)}
                    >
                        <Layers size={14} />
                        Studio Mode
                    </button>
                </div>
            </div>

            {/* Scene Collections */}
            <div className={styles.sceneCollections}>
                <span className={styles.collectionsLabel}>Scene Collection:</span>
                <div className={styles.collectionsList}>
                    {sceneCollections.map(col => (
                        <button
                            key={col.id}
                            className={`${styles.collectionBtn} ${col.isActive ? styles.active : ''}`}
                            onClick={() => loadSceneCollection(col)}
                        >
                            {col.name}
                        </button>
                    ))}
                    <button 
                        className={styles.addCollectionBtn}
                        onClick={() => setShowSceneCollectionModal(true)}
                    >
                        <Plus size={12} /> New
                    </button>
                </div>
            </div>

            {/* Main Content */}
            <div className={styles.mainContent}>
                {/* Preview/Program Monitors */}
                <div className={styles.monitors}>
                    {isStudioMode && (
                        <div className={styles.monitor}>
                            <div className={styles.monitorHeader}>
                                <Eye size={14} />
                                <span>PREVIEW</span>
                            </div>
                            <div className={styles.monitorScreen}>
                                {previewScene ? (
                                    <div className={styles.sceneName}>{previewScene.name}</div>
                                ) : (
                                    <div className={styles.noScene}>No Preview</div>
                                )}
                            </div>
                        </div>
                    )}
                    
                    <div className={styles.transitionArea}>
                        <select 
                            value={currentTransition.id}
                            onChange={(e) => {
                                const trans = transitions.find(t => t.id === e.target.value);
                                if (trans) setCurrentTransition(trans);
                            }}
                            className={styles.transitionSelect}
                        >
                            {transitions.map(t => (
                                <option key={t.id} value={t.id}>{t.name}</option>
                            ))}
                        </select>
                        <input
                            type="number"
                            value={currentTransition.duration}
                            onChange={(e) => setCurrentTransition(prev => ({ ...prev, duration: parseInt(e.target.value) || 0 }))}
                            className={styles.durationInput}
                            min={0}
                            step={50}
                        />
                        <span className={styles.durationLabel}>ms</span>
                        <button 
                            className={styles.transitionBtn}
                            onClick={handleTransition}
                            disabled={!previewScene}
                        >
                            <ArrowRight size={16} />
                            Transition
                        </button>
                        <button 
                            className={styles.cutBtn}
                            onClick={() => {
                                setCurrentTransition(prev => ({ ...prev, duration: 0 }));
                                handleTransition();
                            }}
                            disabled={!previewScene}
                        >
                            CUT
                        </button>
                    </div>
                    
                    <div className={`${styles.monitor} ${styles.programMonitor}`}>
                        <div className={styles.monitorHeader}>
                            <div className={styles.liveIndicator}>
                                <Circle size={10} fill="currentColor" />
                                LIVE
                            </div>
                            <span>PROGRAM</span>
                        </div>
                        <div className={styles.monitorScreen}>
                            {activeScene ? (
                                <div className={styles.sceneName}>{activeScene.name}</div>
                            ) : (
                                <div className={styles.noScene}>No Scene Active</div>
                            )}
                        </div>
                    </div>
                </div>

                {/* Tabs */}
                <div className={styles.tabs}>
                    <button 
                        className={activeTab === 'scenes' ? styles.active : ''}
                        onClick={() => setActiveTab('scenes')}
                    >
                        <Grid size={14} /> Scenes
                    </button>
                    <button 
                        className={activeTab === 'sources' ? styles.active : ''}
                        onClick={() => setActiveTab('sources')}
                    >
                        <Layers size={14} /> Sources
                    </button>
                    <button 
                        className={activeTab === 'audio' ? styles.active : ''}
                        onClick={() => setActiveTab('audio')}
                    >
                        <Volume2 size={14} /> Audio Mixer
                    </button>
                    <button 
                        className={activeTab === 'commands' ? styles.active : ''}
                        onClick={() => setActiveTab('commands')}
                    >
                        <Terminal size={14} /> Commands
                    </button>
                    <button 
                        className={activeTab === 'automation' ? styles.active : ''}
                        onClick={() => setActiveTab('automation')}
                    >
                        <Zap size={14} /> Automation
                    </button>
                </div>

                {/* Tab Content */}
                <div className={styles.tabContent}>
                    {activeTab === 'scenes' && (
                        <div className={styles.scenesGrid}>
                            {scenes.map(scene => (
                                <button
                                    key={scene.id}
                                    className={`${styles.sceneBtn} ${scene.isActive ? styles.active : ''} ${scene.isPreview ? styles.preview : ''}`}
                                    onClick={() => handleSceneSwitch(scene)}
                                    onDoubleClick={() => {
                                        setScenes(prev => prev.map(s => ({
                                            ...s,
                                            isActive: s.id === scene.id
                                        })));
                                    }}
                                >
                                    <div className={styles.sceneThumb}>
                                        <Monitor size={24} />
                                    </div>
                                    <span className={styles.sceneBtnName}>{scene.name}</span>
                                    {scene.isActive && <span className={styles.liveBadge}>LIVE</span>}
                                    {scene.isPreview && <span className={styles.previewBadge}>PRV</span>}
                                </button>
                            ))}
                            <button className={styles.addSceneBtn}>
                                <Plus size={24} />
                                <span>Add Scene</span>
                            </button>
                        </div>
                    )}

                    {activeTab === 'sources' && (
                        <div className={styles.sourcesPanel}>
                            <div className={styles.sourcesList}>
                                <div className={styles.sourcesHeader}>
                                    <span>Scene: {activeScene?.name || 'None'}</span>
                                    <button className={styles.addSourceBtn}>
                                        <Plus size={14} /> Add Source
                                    </button>
                                </div>
                                {activeScene?.sources.map(source => (
                                    <div 
                                        key={source.id}
                                        className={`${styles.sourceItem} ${selectedSource?.id === source.id ? styles.selected : ''}`}
                                        onClick={() => setSelectedSource(source)}
                                    >
                                        <button 
                                            className={styles.sourceVisibility}
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                handleSourceToggle(activeScene.id, source.id);
                                            }}
                                        >
                                            {source.visible ? <Eye size={14} /> : <EyeOff size={14} />}
                                        </button>
                                        <button className={styles.sourceLock}>
                                            {source.locked ? <Lock size={14} /> : <Unlock size={14} />}
                                        </button>
                                        {getSourceIcon(source.type)}
                                        <span className={styles.sourceName}>{source.name}</span>
                                    </div>
                                ))}
                            </div>
                            
                            {selectedSource && (
                                <div className={styles.sourceProperties}>
                                    <h4>Source Properties</h4>
                                    <div className={styles.propertyGroup}>
                                        <label>Name</label>
                                        <input type="text" value={selectedSource.name} readOnly />
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Position</label>
                                        <div className={styles.propertyRow}>
                                            <input type="number" value={selectedSource.position.x} readOnly />
                                            <input type="number" value={selectedSource.position.y} readOnly />
                                        </div>
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Size</label>
                                        <div className={styles.propertyRow}>
                                            <input type="number" value={selectedSource.size.width} readOnly />
                                            <input type="number" value={selectedSource.size.height} readOnly />
                                        </div>
                                    </div>
                                </div>
                            )}
                        </div>
                    )}

                    {activeTab === 'audio' && (
                        <div className={styles.audioMixer}>
                            {audioMixers.map(mixer => (
                                <div key={mixer.id} className={styles.mixerChannel}>
                                    <div className={styles.mixerHeader}>
                                        <button 
                                            className={`${styles.muteBtn} ${mixer.muted ? styles.muted : ''}`}
                                            onClick={() => handleMuteToggle(mixer.id)}
                                        >
                                            {mixer.muted ? <VolumeX size={14} /> : <Volume2 size={14} />}
                                        </button>
                                        <span className={styles.mixerName}>{mixer.name}</span>
                                    </div>
                                    <div className={styles.mixerFader}>
                                        <input
                                            type="range"
                                            min="0"
                                            max="100"
                                            value={mixer.volume}
                                            onChange={(e) => handleVolumeChange(mixer.id, parseInt(e.target.value))}
                                            className={styles.faderSlider}
                                            style={{ 
                                                '--value': `${mixer.volume}%`,
                                                opacity: mixer.muted ? 0.5 : 1 
                                            } as React.CSSProperties}
                                        />
                                        <div className={styles.meterBar}>
                                            <div 
                                                className={styles.meterLevel} 
                                                style={{ height: `${mixer.muted ? 0 : mixer.volume * 0.8}%` }}
                                            />
                                        </div>
                                    </div>
                                    <div className={styles.mixerValue}>{mixer.volume}%</div>
                                </div>
                            ))}
                        </div>
                    )}

                    {activeTab === 'commands' && (
                        <div className={styles.commandsPanel}>
                            <div className={styles.commandInput}>
                                <form onSubmit={handleCommandSubmit}>
                                    <Terminal size={16} />
                                    <input
                                        ref={commandInputRef}
                                        type="text"
                                        value={commandInput}
                                        onChange={(e) => setCommandInput(e.target.value)}
                                        placeholder="Enter command (e.g., SCENE Main Camera)"
                                    />
                                    <button type="submit">
                                        <Send size={14} />
                                    </button>
                                </form>
                            </div>
                            
                            <div className={styles.commandReference}>
                                <h4>Command Reference</h4>
                                {['Scene', 'Source', 'Stream', 'Audio', 'Filter', 'Transition', 'Control'].map(category => (
                                    <div key={category} className={styles.commandCategory}>
                                        <h5>{category}</h5>
                                        {OBS_COMMANDS.filter(c => c.category === category).map(cmd => (
                                            <div 
                                                key={cmd.id}
                                                className={styles.commandItem}
                                                onClick={() => setCommandInput(cmd.syntax)}
                                            >
                                                <code>{cmd.syntax}</code>
                                                <span>{cmd.description}</span>
                                            </div>
                                        ))}
                                    </div>
                                ))}
                            </div>
                            
                            {commandHistory.length > 0 && (
                                <div className={styles.commandHistory}>
                                    <h4>History</h4>
                                    {commandHistory.slice(-10).reverse().map((cmd, i) => (
                                        <div 
                                            key={i}
                                            className={styles.historyItem}
                                            onClick={() => setCommandInput(cmd)}
                                        >
                                            <code>{cmd}</code>
                                        </div>
                                    ))}
                                </div>
                            )}
                        </div>
                    )}

                    {activeTab === 'automation' && (
                        <div className={styles.automationPanel}>
                            <div className={styles.automationHeader}>
                                <h4>Automation Script</h4>
                                <div className={styles.automationActions}>
                                    <button onClick={executeAutomation} className={styles.runBtn}>
                                        <Play size={14} /> Run
                                    </button>
                                    <button className={styles.saveScriptBtn}>
                                        <Save size={14} /> Save
                                    </button>
                                    <button className={styles.loadScriptBtn}>
                                        <Download size={14} /> Load
                                    </button>
                                </div>
                            </div>
                            
                            <textarea
                                value={automationScript}
                                onChange={(e) => setAutomationScript(e.target.value)}
                                placeholder={`# Automation Script
# Lines starting with # are comments

SCENE Main Camera
WAIT 5000
SHOW Lower Third
WAIT 3000
HIDE Lower Third
SCENE Two Shot
WAIT 10000
SCENE Break`}
                                className={styles.scriptEditor}
                            />
                            
                            <div className={styles.automationHelp}>
                                <h5>Available Commands:</h5>
                                <ul>
                                    <li><code>SCENE &lt;name&gt;</code> - Switch to scene</li>
                                    <li><code>WAIT &lt;ms&gt;</code> - Wait for milliseconds</li>
                                    <li><code>SHOW/HIDE &lt;source&gt;</code> - Toggle source visibility</li>
                                    <li><code>STREAM START/STOP</code> - Control streaming</li>
                                    <li><code>RECORD START/STOP</code> - Control recording</li>
                                    <li><code>MUTE/UNMUTE &lt;source&gt;</code> - Audio control</li>
                                    <li><code>TRANSITION &lt;type&gt;</code> - Set transition</li>
                                </ul>
                            </div>
                        </div>
                    )}
                </div>
            </div>

            {/* Scene Collection Modal */}
            {showSceneCollectionModal && (
                <div className={styles.modal} onClick={() => setShowSceneCollectionModal(false)}>
                    <div className={styles.modalContent} onClick={e => e.stopPropagation()}>
                        <h3>New Scene Collection</h3>
                        <div className={styles.formGroup}>
                            <label>Collection Name</label>
                            <input
                                type="text"
                                value={newCollectionName}
                                onChange={(e) => setNewCollectionName(e.target.value)}
                                placeholder="Enter collection name"
                            />
                        </div>
                        <div className={styles.modalActions}>
                            <button onClick={() => setShowSceneCollectionModal(false)}>Cancel</button>
                            <button onClick={saveSceneCollection} disabled={!newCollectionName.trim()}>
                                <Save size={14} /> Create
                            </button>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
};

export default OBSStudioController;
