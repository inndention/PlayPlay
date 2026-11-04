import React, { useState, useRef, useEffect, useCallback, useMemo } from 'react';

import { useLocation, useNavigate } from 'react-router-dom';

import { persistentLogger } from '../lib/persistentLogger';

import { logError } from '../lib/errorBus';

import { LogViewer } from './LogViewer';

import { MediaLibrary } from './MediaLibrary';

import { PlaylistEditor } from './PlaylistEditor';

import { PlayerEngine } from './PlayerEngine';

import { StudioClock } from './StudioClock';

import { UnifiedScheduler } from './UnifiedScheduler';

import { SettingsPanel } from './SettingsPanel';

import { AnalyticsDashboard } from './AnalyticsDashboard';

import { CGOverlay, useCGController } from './CGOverlay';

import { CGBuilder } from './CGBuilder';

import { CGLivePanel, useCGState } from './CGLivePanel';

import { OBSController, DEFAULT_OBS_SESSIONS } from './OBSController';

import { executeOBSCommandsForItem, type OBSClientResolver } from '../lib/obsCommandExecutor';

import { executeAtemCommandForItem } from '../lib/atemCommandExecutor';

import { OBSWebSocketClient } from '../lib/obsWebSocket';

import { loadOBSCommands } from '../lib/storage';

import { DDRPanel, createEmptyDDRPlaylist } from './DDRPlayer';

import { VideoRouter } from './VideoRouter';

import { ContextMenu, useContextMenu } from './ContextMenu';

import { Tooltip } from './Tooltip';

import { HelpPanel } from './HelpPanel';

// CasperCG panel removed - using built-in engine only

import { useResizer } from './ResizableColumns';

import { ScheduleTicker } from './ScheduleTicker';

import { PrecisionTimeline } from './PrecisionTimeline';

import type { TimelineTrack } from './PrecisionTimeline';

import { MultiSiteTicker } from './MultiSiteTicker';

import { FacebookStoryPoster } from './FacebookStoryPoster';

import { SocialCampaignPanel } from './SocialCampaignPanel';

import { AutomationBoard } from './AutomationBoard';

import { ErrorTallyBox } from './ErrorTallyBox';

// MultiGridView functionality now in UnifiedScheduler

// Removed ComprehensiveMediaLibrary - using MediaLibrary instead

import { TVGuideTickers } from './TVGuideTickers';

import { BroadcastControlMapper } from './BroadcastControlMapper';

import { MultiChannelManager } from './MultiChannelManager';

import { BroadcastMuxManager } from './BroadcastMuxManager';

import { useChannelBreakout } from './ChannelBreakout';

import { ChannelTabs } from './ChannelTabs';

import { PreviewGrid } from './PreviewGrid';

import { MediaItem, MediaType, PlaylistItem, SmartSlotConfig, CGElement, OBSSession, OBSCommand, DDRPlaylist, DDRChannel, HourSlot, WeeklySchedule, PlayoutChannel, MultiChannelState, StarfileFileItem, MediaLibraryItemClickOptions, isSmartSlotConfig, AsPlayedEntry } from '../lib/types';

import { useToast } from './Toast';

import { usePlayplayAgentToastBridge } from '../hooks/usePlayplayAgentToastBridge';

import { loadCGElements, saveCGElements, loadSettings, recordPlayEvent, loadWeeklySchedule, saveWeeklySchedule, loadLocalMedia, loadLibraryMedia, loadAsPlayedLog } from '../lib/storage';

import { mergeEngineApproaches } from '../lib/engineApproaches';

import { logPlayoutEvent } from '../lib/playoutLog';

import { appendEventSummary } from '../lib/eventSummaryClient';

import {

    getCasparVideoModePreference,

    setCasparVideoModePreference,

    CASPAR_VIDEO_MODE_OPTIONS,

    type CasparVideoModeToken,

} from '../lib/casparVideoMode';

import { getCasparMediaPathForCurrentSettings } from '../lib/casparRamdisk';

import { ClientManager } from './ClientManager';

import { TrafficReport } from './TrafficReport';

import { MasterDashboard } from './MasterDashboard';

import { SocialMediaStudio } from './SocialMediaStudio';

import { TalentMonitor } from './TalentMonitor';

import { CGDesigner } from './CGDesigner';

import { OBSStudioController } from './OBSStudioController';

import { TrafficBillingDashboard } from './TrafficBillingDashboard';

import { P2PFileSync } from './P2PFileSync';

import { StarfileWorkspace } from './StarfileWorkspace';

import { SegmentEditor } from './SegmentEditor';

import { ErrorBoundary } from './ErrorBoundary';

import { safeHandler } from '../lib/safeHandlers';

import { useSafeAsync } from '../hooks/useSafeAsync';

import { CampaignManagementPage } from '../pages/CampaignManagementPage';

import { PlayoutEngineManager, PlayoutEngineConfig, PlaybackState as EnginePlaybackState } from '../lib/playoutEngines';

import { dbFetchMedia, dbLogAnalyticsEvent, dbSaveMedia } from '../lib/mediaDbClient';

import { bumpLibraryPriorityUp, bumpLibraryPriorityDown, toggleLibraryPriorityFreeze } from '../lib/mediaPriority';

import { getEngine, AutomationEngine } from '../lib/automationEngine';

import { DerivedState as AutomationState, RendererCommand } from '../lib/automationStateMachine';

// MPV broadcast engine removed - using built-in engine only

import { resolveMediaUrlSync, resolveMediaUrl, getEnvironment, setMediaHttpPort } from '../lib/fileResolver';

import { refreshSiteFiller } from '../lib/siteContentFiller';

import { buildMediaMenuItems, buildPlaylistMenuItems } from '../lib/contextMenuActions';

import { executeContextMenuAction } from '../lib/contextMenuRegistry';

import { CGTemplateShelf } from './CGTemplateShelf';

import { UtilityMenu } from './UtilityMenu';

import { FloatingAIWidget } from './FloatingAIWidget';

import { generateTTSAudio } from '../lib/whisperTTS';

import {

    Play,

    Pause,

    Square,

    SkipForward,

    Maximize,

    ExternalLink,

    Anchor,

    Clock,

    Calendar,

    BarChart2,

    Settings,

    Layers,

    Radio,

    Film,

    HelpCircle,

    Briefcase,

    FileText,

    Download,

    FolderOpen,

    ShieldAlert,

    Copy,

    Link2,

    Sparkles,

    Server,

    Share2,

    Monitor,

    Zap,

    Grid,

    Satellite,

    MessageSquare,

    MessageCircle,

    Video,

    DollarSign,

    Cloud,

    Palette,

    Target,

    Wifi,

    Wrench,

    RefreshCw,

    Tv,

    Plus,

    Minimize2,

} from 'lucide-react';

import { PSIPEvent } from '../lib/psipGenerator';

import { useHourAutoPush } from '../hooks/useHourAutoPush';

import { useStartupCatchup } from '../hooks/useStartupCatchup';

import {

    computeBossClockPlayoutPosition,

    buildCasparRunlistFromBossSeek,

    buildCasparRunlistForBossClock,

} from '../lib/casparBossClock';

import { InputManager } from './InputManager';

import styles from './PlayoutController.module.css';



// Check if running in Electron

const isElectron = !!(window as any).electronAPI;



const generateId = () => Math.random().toString(36).substr(2, 9);



const createGapMedia = (durationMs: number): MediaItem => ({

    id: `gap_${generateId()}`,

    title: 'GAP',

    duration: Math.floor(durationMs / 1000),

    src: '',

    type: 'gap',

});



type ViewMode = 'playout' | 'scheduler' | 'timeline' | 'traffic' | 'analytics' | 'settings' | 'master' | 'multigrid' | 'library' | 'tvguide' | 'broadcast' | 'mux' | 'multichannel' | 'campaigns' | 'social' | 'talent' | 'billing' | 'filesync' | 'starfile' | 'utilities';

type SidePanel = 'none' | 'cg' | 'obs' | 'ddr' | 'router' | 'caspercg' | 'cgdesigner' | 'obsadvanced';



interface AsRunDisplayEntry {

    source: 'as-played' | 'control-service';

    actualStart: number;

    actualEnd: number;

    duration: number;

    completed: boolean;

    status?: string;

    hourSlot?: string;

    itemId?: string;

    reason?: string;

}



interface MediaAsRunReport {

    media: MediaItem;

    generatedAt: number;

    entries: AsRunDisplayEntry[];

}



interface PlayoutControllerProps {

    /** Core-only mode: scheduling by hour, as-played, reporting, segmenting, browser source playout only */

    coreOnly?: boolean;

}



/** Resolve media src for HTTP browser output (OBS /output pages). */

function preparePlaylistItemMediaHttp(item: PlaylistItem | undefined): PlaylistItem | undefined {

    if (!item) return undefined;

    const media = item.media;

    if (!media) return item;

    const resolved = resolveMediaUrlSync(media);

    return {

        ...item,

        media: {

            ...media,

            src: resolved || '',

        },

    };

}



/** Per-channel playlist snapshot slice used for OBS / Electron HTTP `/api/state/ch-*` (inactive rails). */

type BrowserInactiveSnapshot = {

    playlist: PlaylistItem[];

    activeItemId: string | null;

    isPlaying: boolean;

    playStartTime: number | null;

    initialSeekTime: number | null;

};



/** Last known media playhead + wall clock — extrapolate time for browser URLs when rail is inactive. */

type ChannelPlaybackSync = { wallMs: number; mediaSec: number };



/**

 * Browser program output state for any multichannel rail (inactive or active mirrored elsewhere).

 * Inactive snapshots must honour `snap.isPlaying` and a drifting `currentTime` so `/output2` … keep

 * lockstep with `/output`; the legacy helper always shipped `isPlaying: false` and `currentTime: 0`.

 */

function buildBrowserOutputStateForChannel(

    snap: BrowserInactiveSnapshot | undefined,

    sync: ChannelPlaybackSync | undefined,

    cgElements: CGElement[],

    cgLayers: unknown[],

): {

    currentMedia: PlaylistItem | undefined;

    nextMedia: PlaylistItem | undefined;

    isPlaying: boolean;

    cgElements: CGElement[];

    cgLayers: unknown[];

    currentTime: number;

    seekTime: number;

    timestamp: number;

} {

    const pl = snap?.playlist || [];

    const idx = snap?.activeItemId ? pl.findIndex(i => i.id === snap.activeItemId) : -1;

    const cur = idx >= 0 ? pl[idx] : undefined;

    const nxt = idx >= 0 && idx < pl.length - 1 ? pl[idx + 1] : undefined;

    const isPlaying = !!(snap?.isPlaying);

    let currentTime = 0;

    if (sync) {

        if (isPlaying) {

            currentTime = sync.mediaSec + (Date.now() - sync.wallMs) / 1000;

        } else {

            currentTime = sync.mediaSec;

        }

    } else if (isPlaying && snap?.playStartTime) {

        const base = snap.initialSeekTime ?? cur?.timeIn ?? 0;

        currentTime = base + (Date.now() - snap.playStartTime) / 1000;

    } else {

        currentTime = snap?.initialSeekTime ?? cur?.timeIn ?? 0;

    }

    if (cur) {

        const lo = typeof cur.timeIn === 'number' ? cur.timeIn : 0;

        const hi =

            typeof cur.timeOut === 'number'

                ? cur.timeOut

                : typeof cur.media?.duration === 'number'

                    ? cur.media.duration

                    : undefined;

        if (typeof hi === 'number') currentTime = Math.max(lo, Math.min(currentTime, hi));

    }

    return {

        currentMedia: preparePlaylistItemMediaHttp(cur),

        nextMedia: preparePlaylistItemMediaHttp(nxt),

        isPlaying,

        cgElements,

        cgLayers,

        currentTime,

        seekTime: cur?.timeIn || snap?.initialSeekTime || 0,

        timestamp: Date.now(),

    };

}



/**

 * CasparCG channel panel — replaces the old browser-source URL list.

 *

 * Each broadcast channel here corresponds to a CasparCG channel + its

 * <screen> consumer, which spawns a window titled "PlayPlay CHn".

 * In OBS: Add Source → Window Capture → pick "PlayPlay CH1" / CH2 / …

 *

 * The "push" action sends the renderer's playlist for that channel to

 * CasparCG's AMCP port (5250) — it runs in CasparCG, not in the browser.

 */

function ObsUrlPanel({

    channels,

    casparPhysicalChannels,

    snapshotsRef,

    activeChannelId,

    activePlaylist,

    casparStatus,

    obsSessions,

    onChannelObsSessionChange,

}: {

    channels: PlayoutChannel[];

    /** Caspar `<screen>` / AMCP channel count — use multichannel max, not `channels.length`, so spare CH windows exist. */

    casparPhysicalChannels: number;

    snapshotsRef: React.MutableRefObject<Record<string, any>>;

    activeChannelId: string;

    activePlaylist: PlaylistItem[];

    casparStatus: {

        running: boolean;

        pid: number | null;

        videoMode?: string;

        seekFps?: number;

        allowAbsoluteMediaPaths?: boolean;

    };

    obsSessions: OBSSession[];

    onChannelObsSessionChange: (channelId: string, sessionId: string | undefined) => void;

}) {

    const [copiedId, setCopiedId] = React.useState<string | null>(null);

    const [busyId, setBusyId] = React.useState<string | null>(null);

    const [bridgeStatus, setBridgeStatus] = React.useState<{

        running: boolean;

        pid: number | null;

        installDir: string | null;

        exeExists: boolean;

    }>({ running: false, pid: null, installDir: null, exeExists: false });



    const [casparHzPref, setCasparHzPref] = React.useState(() => getCasparVideoModePreference());

    const [programOutputUrls, setProgramOutputUrls] = React.useState<{

        primary: string;

        primaryChannelId: string;

        obsLight?: string;

        mediaHttpExternal?: string;

        lanHttpClientsBlocked?: boolean;

        backups: Array<{ n: number; channelId: string; url: string }>;

    } | null>(null);



    React.useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.getOutputUrl) return;

        let cancelled = false;

        void (async () => {

            try {

                const r = await api.getOutputUrl();

                if (cancelled || !r || typeof r !== 'object') return;

                const primary = (r.external || r.local || '') as string;

                const primaryChannelId = typeof r.channelId === 'string' ? r.channelId : 'ch-1';

                const obsLight = typeof r.obsLight === 'string' ? r.obsLight : undefined;

                const mediaHttpExternal = typeof r.mediaHttpExternal === 'string' ? r.mediaHttpExternal : undefined;

                if (typeof r.mediaHttpPort === 'number' && r.mediaHttpPort > 0) {

                    setMediaHttpPort(r.mediaHttpPort);

                }

                const raw = Array.isArray(r.backups) ? r.backups : [];

                const backups = raw.map((b: { n: number; channelId?: string; external?: string; local?: string }) => ({

                    n: b.n,

                    channelId: typeof b.channelId === 'string' ? b.channelId : `ch-${b.n}`,

                    url: (b.external || b.local || '') as string,

                })).filter((x: { url: string }) => !!x.url);

                setProgramOutputUrls({

                    primary,

                    primaryChannelId,

                    obsLight,

                    mediaHttpExternal,

                    lanHttpClientsBlocked: !!r.lanHttpClientsBlocked,

                    backups,

                });

            } catch { /* */ }

        })();

        return () => { cancelled = true; };

    }, []);



    React.useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.bridgeStatus) return;

        let cancelled = false;

        const refresh = async () => {

            try {

                const r = await api.bridgeStatus();

                if (cancelled || !r?.ok || !r.data) return;

                setBridgeStatus({

                    running: !!r.data.running,

                    pid: r.data.pid ?? null,

                    installDir: r.data.installDir ?? null,

                    exeExists: !!r.data.exeExists,

                });

            } catch { /* */ }

        };

        refresh();

        const t = window.setInterval(refresh, 4000);

        return () => {

            cancelled = true;

            window.clearInterval(t);

        };

    }, []);



    const copyText = async (text: string, id: string) => {

        try {

            await navigator.clipboard.writeText(text);

            setCopiedId(id);

            setTimeout(() => setCopiedId(c => (c === id ? null : c)), 1200);

        } catch { /* */ }

    };



    const pushPlaylist = async (ch: PlayoutChannel, idx: number) => {

        const api = (window as any).electronAPI;

        if (!api?.casparPlayPlaylist) return;

        const casparChannel = (ch as any).casparChannel || (idx + 1);

        const playlist = ch.id === activeChannelId

            ? activePlaylist

            : (snapshotsRef.current[ch.id]?.playlist || []);

        if (!playlist.length) {

            alert('That channel has an empty playlist — nothing to push.');

            return;

        }

        setBusyId(ch.id);

        try {

            const r = await api.casparPlayPlaylist(casparChannel, playlist);

            if (!r?.ok) {

                const tail = Array.isArray(r?.recentAmcp) && r.recentAmcp.length

                    ? `\n\nRecent AMCP:\n${r.recentAmcp.join('\n')}`

                    : '';

                window.alert(`Caspar push failed:\n${r?.error || 'unknown'}${tail}`);

                console.error('caspar push failed:', r?.error, r?.recentAmcp);

            }

        } finally {

            setBusyId(b => (b === ch.id ? null : b));

        }

    };



    const stopChannel = async (ch: PlayoutChannel, idx: number) => {

        const api = (window as any).electronAPI;

        const casparChannel = (ch as any).casparChannel || (idx + 1);

        await api?.casparStopChannel?.(casparChannel);

    };



    return (

        <div style={{

            display: 'flex', flexDirection: 'column', gap: 3,

            padding: '4px 2px 6px', borderBottom: '1px solid var(--border)', fontSize: 9,

        }}>

            <div style={{

                display: 'flex', alignItems: 'center', gap: 4,

                padding: '2px 4px', fontSize: 9,

                color: casparStatus.running ? '#22c55e' : '#f59e0b',

            }}>

                <span>

                    ● CasparCG {casparStatus.running ? `running (pid ${casparStatus.pid})` : 'not running'}

                    {casparStatus.videoMode != null && (

                        <span

                            style={{ color: 'var(--text-muted)', marginLeft: 4 }}

                            title="Caspar channel video-mode and frame rate used for AMCP SEEK/LENGTH (output timeline). Source files can be any rate."

                        >

                            {casparStatus.videoMode}

                            {casparStatus.seekFps != null

                                ? ` @ ${Number(casparStatus.seekFps).toFixed(2)}fps`

                                : ''}

                        </span>

                    )}

                    {casparStatus.allowAbsoluteMediaPaths ? (

                        <span

                            style={{ color: 'var(--text-muted)', marginLeft: 4 }}

                            title="casparcg.config allows absolute/UNC paths (requires patched CasparCG server)."

                        >

                            abs✓

                        </span>

                    ) : null}

                </span>

                <label style={{ marginLeft: 6, display: 'inline-flex', alignItems: 'center', gap: 3 }}>

                    <span style={{ color: 'var(--text-muted)' }}>Hz</span>

                    <select

                        value={casparHzPref}

                        title="Caspar channel video-mode (restarts Caspar when changed)"

                        style={{ fontSize: 9, maxWidth: 72 }}

                        onChange={async (e) => {

                            const mode = e.target.value;

                            if (!setCasparVideoModePreference(mode)) return;

                            setCasparHzPref(mode as CasparVideoModeToken);

                            const api = (window as any).electronAPI;

                            if (!api?.casparConfigure) return;

                            const casparDir = (() => {

                                try {

                                    return localStorage.getItem('casparDir') || 'C:/casparcg';

                                } catch {

                                    return 'C:/casparcg';

                                }

                            })();

                            try {

                                const n = Math.floor(Number(casparPhysicalChannels));

                                const ch = Math.max(

                                    1,

                                    Math.min(32, Number.isFinite(n) && n >= 1 ? n : 1),

                                );

                                const casparMediaPath = getCasparMediaPathForCurrentSettings();

                                await api.casparConfigure({

                                    casparDir,

                                    channels: ch,

                                    videoMode: mode,

                                    ...(casparMediaPath ? { casparMediaPath } : {}),

                                });

                                const st = await api.casparStatus();

                                if (!st?.data?.running) await api.casparStart(casparDir);

                            } catch (err) {

                                console.error('Caspar video mode change failed:', err);

                                alert(`Caspar configure failed: ${(err as Error).message}`);

                            }

                        }}

                    >

                        {CASPAR_VIDEO_MODE_OPTIONS.map((o) => (

                            <option key={o.value} value={o.value}>{o.label}</option>

                        ))}

                    </select>

                </label>

            </div>

            {(window as any).electronAPI?.bridgeStatus && (

                <div style={{

                    display: 'flex', alignItems: 'center', flexWrap: 'wrap', gap: 4,

                    padding: '4px 4px 6px', fontSize: 9,

                    borderBottom: '1px solid var(--border)',

                    color: bridgeStatus.running ? '#38bdf8' : '#94a3b8',

                }}>

                    <span style={{ fontWeight: 600 }}>SVT Bridge</span>

                    <span>

                        {bridgeStatus.running

                            ? `running (pid ${bridgeStatus.pid})`

                            : bridgeStatus.exeExists

                                ? 'stopped'

                                : 'exe not found'}

                    </span>

                    <button

                        type="button"

                        style={{ fontSize: 9, padding: '2px 6px', cursor: 'pointer' }}

                        disabled={bridgeStatus.running}

                        title="Start Bridge.exe from the configured Bridge-win32-x64 folder"

                        onClick={async () => {

                            const api = (window as any).electronAPI;

                            const r = await api.bridgeStart?.();

                            if (!r?.ok) alert(r?.error || 'Bridge start failed');

                        }}

                    >

                        Start

                    </button>

                    <button

                        type="button"

                        style={{ fontSize: 9, padding: '2px 6px', cursor: 'pointer' }}

                        disabled={!bridgeStatus.running}

                        title="Stop the Bridge process launched by PLAY PLAY"

                        onClick={async () => {

                            await (window as any).electronAPI?.bridgeStop?.();

                        }}

                    >

                        Stop

                    </button>

                    <button

                        type="button"

                        style={{ fontSize: 9, padding: '2px 6px', cursor: 'pointer' }}

                        title="Select folder containing Bridge.exe (e.g. Downloads\\Bridge-win32-x64)"

                        onClick={async () => {

                            const api = (window as any).electronAPI;

                            const paths = await api.openFolderDialog?.();

                            const dir = Array.isArray(paths) ? paths[0] : null;

                            if (!dir) return;

                            const r = await api.bridgeSetInstallDir?.(dir);

                            if (!r?.ok) alert(r?.error || 'Invalid Bridge folder');

                        }}

                    >

                        Set folder…

                    </button>

                    <button

                        type="button"

                        style={{ fontSize: 9, padding: '2px 6px', cursor: 'pointer' }}

                        disabled={!bridgeStatus.installDir}

                        title="Reveal Bridge.exe in Explorer"

                        onClick={() => void (window as any).electronAPI?.bridgeRevealInstall?.()}

                    >

                        Reveal

                    </button>

                </div>

            )}

            {channels.map((ch, idx) => {

                const windowTitle = `PlayPlay CH${idx + 1}`;

                const isCopied = copiedId === ch.id;

                const isBusy = busyId === ch.id;

                return (

                    <div key={ch.id} style={{

                        display: 'flex', alignItems: 'center', gap: 3,

                        background: 'rgba(255,255,255,0.04)',

                        border: '1px solid var(--border)', borderRadius: 4, padding: '3px 4px',

                        fontFamily: 'monospace', fontSize: 9,

                    }}>

                        <span style={{ fontWeight: 700, color: 'var(--text-primary)', minWidth: 22 }}>CH{idx + 1}</span>

                        <label

                            style={{ display: 'inline-flex', alignItems: 'center', gap: 2, color: 'var(--text-muted)' }}

                            title="OBS WebSocket session for this channel’s playlist automation (use one OBS Portable + unique websocket port per session)"

                        >

                            <span style={{ fontSize: 8 }}>OBS</span>

                            <select

                                value={ch.outputConfig?.obsSessionId ?? ''}

                                onChange={(e) => {

                                    const v = e.target.value;

                                    onChannelObsSessionChange(ch.id, v || undefined);

                                }}

                                style={{ fontSize: 8, maxWidth: 120, padding: '1px 2px' }}

                            >

                                <option value="">auto</option>

                                {obsSessions.map((s) => (

                                    <option key={s.id} value={s.id}>

                                        {s.name}:{s.port}{s.isConnected ? '' : ' ○'}

                                    </option>

                                ))}

                            </select>

                        </label>

                        <span style={{ color: 'var(--text-muted)', whiteSpace: 'nowrap' }}>{windowTitle}</span>

                        <button

                            type="button"

                            onClick={() => copyText(windowTitle, ch.id)}

                            title={`Copy window title for OBS Window Capture: ${windowTitle}`}

                            style={{

                                marginLeft: 'auto',

                                padding: '1px 4px', fontSize: 9, background: 'transparent',

                                border: '1px solid var(--border)', borderRadius: 3,

                                color: 'var(--text-muted)', cursor: 'pointer',

                            }}

                        >

                            {isCopied ? '✓' : 'copy'}

                        </button>

                        <button

                            type="button"

                            onClick={() => pushPlaylist(ch, idx)}

                            disabled={isBusy || !casparStatus.running}

                            title="Push this channel's loaded hour-grid playlist (with segments) to CasparCG and start"

                            style={{

                                padding: '1px 4px', fontSize: 9, background: 'transparent',

                                border: '1px solid var(--border)', borderRadius: 3,

                                color: 'var(--text-muted)',

                                cursor: !isBusy && casparStatus.running ? 'pointer' : 'not-allowed',

                            }}

                        >

                            {isBusy ? '…' : 'push'}

                        </button>

                        <button

                            type="button"

                            onClick={() => stopChannel(ch, idx)}

                            disabled={!casparStatus.running}

                            title="Stop this CasparCG channel"

                            style={{

                                padding: '1px 4px', fontSize: 9, background: 'transparent',

                                border: '1px solid var(--border)', borderRadius: 3,

                                color: 'var(--text-muted)',

                                cursor: casparStatus.running ? 'pointer' : 'not-allowed',

                            }}

                        >

                            stop

                        </button>

                    </div>

                );

            })}

            {programOutputUrls?.primary ? (

                <div style={{

                    padding: '4px 4px 6px',

                    borderTop: '1px solid var(--border)',

                    fontSize: 9,

                    lineHeight: 1.35,

                    color: 'var(--text-muted)',

                }}>

                    {programOutputUrls.lanHttpClientsBlocked ? (

                        <div

                            style={{

                                marginBottom: 6,

                                padding: '4px 6px',

                                borderRadius: 4,

                                background: 'rgba(245,158,11,0.12)',

                                border: '1px solid rgba(245,158,11,0.35)',

                                color: '#fbbf24',

                                fontSize: 8,

                            }}

                            title="Remote PCs cannot reach /output or /api/state until the bind address includes the LAN interface."

                        >

                            Output HTTP is loopback-only — set <code style={{ fontSize: 8 }}>PLAYPLAY_OUTPUT_BIND=0.0.0.0</code> and open the port in Windows Firewall for LAN viewers. See <code style={{ fontSize: 8 }}>docs/ONLINEABLE_CASPAR_AND_OUTPUTS.md</code>.

                        </div>

                    ) : null}

                    <div style={{ fontWeight: 600, color: 'var(--text-primary)', marginBottom: 2 }}>

                        HTTP program (OBS Browser) — /output = CH1, /outputN = CHN

                    </div>

                    {programOutputUrls.obsLight ? (

                        <div style={{ marginBottom: 4, fontSize: 8, color: 'var(--text-muted)', lineHeight: 1.35 }}>

                            <span style={{ fontWeight: 600, color: 'var(--text-primary)' }}>Ultra-light</span>

                            {' '}

                            <code style={{ fontSize: 8 }}>/obs-light</code>

                            {' '}(no React; polls state). CH2:{' '}

                            <code style={{ fontSize: 8 }}>/obs-light/2</code>

                            {' '}· query{' '}

                            <code style={{ fontSize: 8 }}>?ch=ch-1&amp;poll=400&amp;reloadOnScene=1</code>

                            <button

                                type="button"

                                style={{ marginLeft: 6, padding: '1px 4px', fontSize: 8, cursor: 'pointer' }}

                                title="Copy /obs-light URL. reloadOnScene=1 / reloadOnActive=1 = full page reload on OBS events (needs Page permissions). obsstudio cannot hard-refresh this source from JS alone."

                                onClick={() => void copyText(programOutputUrls.obsLight!, 'obs-light')}

                            >

                                {copiedId === 'obs-light' ? '✓' : 'copy'}

                            </button>

                            {programOutputUrls.mediaHttpExternal ? (

                                <>

                                    {' · '}

                                    <span style={{ fontWeight: 600, color: 'var(--text-primary)' }}>Media HTTP</span>

                                    <button

                                        type="button"

                                        style={{ marginLeft: 4, padding: '1px 4px', fontSize: 8, cursor: 'pointer' }}

                                        title="Dedicated /m?path=… server + Electron read-ahead preload"

                                        onClick={() => void copyText(programOutputUrls.mediaHttpExternal!, 'media-http')}

                                    >

                                        {copiedId === 'media-http' ? '✓' : 'copy'}

                                    </button>

                                </>

                            ) : null}

                        </div>

                    ) : null}

                    <div style={{ display: 'flex', flexWrap: 'wrap', alignItems: 'center', gap: 3 }}>

                        <span style={{ fontFamily: 'monospace', wordBreak: 'break-all' }} title={`CH1 → ${programOutputUrls.primaryChannelId}`}>

                            CH1 {programOutputUrls.primary}

                        </span>

                        <button

                            type="button"

                            style={{ padding: '1px 4px', fontSize: 9, cursor: 'pointer' }}

                            onClick={() => void copyText(programOutputUrls.primary, 'out-primary')}

                        >

                            {copiedId === 'out-primary' ? '✓' : 'copy'}

                        </button>

                    </div>

                    <div style={{ marginTop: 4, display: 'flex', flexWrap: 'wrap', gap: 4 }}>

                        {programOutputUrls.backups.slice(0, 6).map((b) => (

                            <button

                                key={b.n}

                                type="button"

                                title={`CH${b.n} (${b.channelId}) — /api/state/${b.channelId}`}

                                style={{

                                    padding: '1px 5px',

                                    fontSize: 9,

                                    fontFamily: 'monospace',

                                    cursor: 'pointer',

                                    border: '1px solid var(--border)',

                                    borderRadius: 3,

                                    background: 'rgba(255,255,255,0.03)',

                                }}

                                onClick={() => void copyText(b.url, `out-${b.n}`)}

                            >

                                {copiedId === `out-${b.n}` ? '✓' : `CH${b.n} /output${b.n}`}

                            </button>

                        ))}

                        {programOutputUrls.backups.length > 6 ? (

                            <span title="/output2–/output16 map to ch-2 … ch-16">…+{programOutputUrls.backups.length - 6}</span>

                        ) : null}

                    </div>

                </div>

            ) : null}

            <div style={{ padding: '4px 4px 2px', color: 'var(--text-muted)', fontSize: 9, lineHeight: 1.4 }}>

                In OBS: Add Source → <b>Window Capture</b> → pick <b>PlayPlay CHn</b>.

            </div>

        </div>

    );

}



export function PlayoutController({ coreOnly = false }: PlayoutControllerProps) {

    const { showToast } = useToast();

    usePlayplayAgentToastBridge(showToast);

    const location = useLocation();

    const navigate = useNavigate();

    const { safeSetState } = useSafeAsync();



    // View state – sync campaigns view with /campaigns route

    const [viewMode, setViewMode] = useState<ViewMode>(() =>

        location.pathname === '/campaigns' ? 'campaigns' : 'playout'

    );

    useEffect(() => {

        if (location.pathname === '/campaigns') setViewMode('campaigns');

    }, [location.pathname]);

    const [trafficTab, setTrafficTab] = useState<'clients' | 'reports'>('clients');

    const [sidePanel, setSidePanel] = useState<SidePanel>('none');

    const [editingCG, setEditingCG] = useState<CGElement | null>(null);

    const [activeSiteId, setActiveSiteId] = useState<string | null>(null);

    const [schedulerTab, setSchedulerTab] = useState<'schedule' | 'automation'>('schedule');

    const [playoutRightTab, setPlayoutRightTab] = useState<'library' | 'runofshow' | 'cgtemplates' | 'inputs'>('library');

    const [editingSegmentMedia, setEditingSegmentMedia] = useState<MediaItem | null>(null);

    const [analyticsFocus, setAnalyticsFocus] = useState<Record<string, unknown> | null>(null);



    // Compact / low-DPI density mode — persisted

    const [compactMode, setCompactMode] = useState<boolean>(() => {

        try { return localStorage.getItem('ui_compact_mode') === '1'; } catch { return false; }

    });

    useEffect(() => {

        try { localStorage.setItem('ui_compact_mode', compactMode ? '1' : '0'); } catch { /* */ }

    }, [compactMode]);



    // Tracks whether the on-air playout is detached to a separate window.

    // When detached, the main window can be muted/used purely for control,

    // giving the playout engine its own process affinity.

    const [outputDetached, setOutputDetached] = useState<boolean>(false);

    const [showObsUrls, setShowObsUrls] = useState<boolean>(false);



    // CasparCG live status (poll every 3s)

    const [casparStatus, setCasparStatus] = useState<{

        running: boolean;

        pid: number | null;

        videoMode?: string;

        seekFps?: number;

        allowAbsoluteMediaPaths?: boolean;

    }>({ running: false, pid: null });

    const [useCasparAsPrimary, setUseCasparAsPrimary] = useState<boolean>(() =>

        mergeEngineApproaches(loadSettings().engineApproaches).casparTransport

    );

    const [casparStartupCatchupEnabled, setCasparStartupCatchupEnabled] = useState<boolean>(() =>

        mergeEngineApproaches(loadSettings().engineApproaches).casparStartupCatchup

    );

    const [casparTransportPlaying, setCasparTransportPlaying] = useState<boolean>(false);



    useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.casparStatus) return;

        let cancelled = false;

        const refresh = async () => {

            try {

                const r = await api.casparStatus();

                if (cancelled || !r?.ok) return;

                setCasparStatus({

                    running: r.data.running,

                    pid: r.data.pid,

                    videoMode: r.data.videoMode,

                    seekFps: r.data.seekFps,

                    allowAbsoluteMediaPaths: r.data.allowAbsoluteMediaPaths,

                });

            } catch { /* */ }

        };

        refresh();

        const t = window.setInterval(refresh, 3000);

        return () => { cancelled = true; window.clearInterval(t); };

    }, []);



    // On mount: configure CasparCG with however many channels the user has,

    // and ensure casparcg.exe is up.

    useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.casparConfigure) return;

        if (!mergeEngineApproaches(loadSettings().engineApproaches).casparAutoconfigureOnLaunch) return;

        // Match Caspar physical outputs to multichannel ceiling so PlayPlay CH1..CHn

        // screen consumers exist even when the sidebar has fewer rows (saved layout).

        const channelCount = Math.max(1, Math.min(32, Math.floor(multiChannelState.maxChannels) || 8));

        (async () => {

            const defaultDir = 'C:/casparcg';

            const persistedDir = (() => {

                try { return localStorage.getItem('casparDir') || defaultDir; }

                catch { return defaultDir; }

            })();

            const candidateDirs = Array.from(new Set([

                persistedDir,

                defaultDir,

                'C:/casparcg',

                'C:/CasparCG Server',

            ]));

            let casparDir = persistedDir;

            for (const dir of candidateDirs) {

                try {

                    const exists = await api.pathExists?.(`${dir}/casparcg.exe`);

                    if (exists) {

                        casparDir = dir;

                        try { localStorage.setItem('casparDir', dir); } catch { /* noop */ }

                        break;

                    }

                } catch {

                    // Keep probing candidates

                }

            }

            try {

                await api.casparConfigure({

                    casparDir,

                    channels: channelCount,

                    videoMode: getCasparVideoModePreference(),

                });

                const st = await api.casparStatus();

                if (!st?.data?.running) await api.casparStart(casparDir);

            } catch (e) {

                console.error('initial CasparCG setup failed:', e);

            }

        })();

        // eslint-disable-next-line react-hooks/exhaustive-deps

    }, []);

    useEffect(() => {

        try {

            localStorage.setItem('playout-use-caspar-engine', useCasparAsPrimary ? '1' : '0');

        } catch { /* noop */ }

        try {

            const { settingsManager } = require('../lib/settingsManager');

            const cur = settingsManager.getSettings();

            settingsManager.updateAutomationSettings({

                engineApproaches: mergeEngineApproaches({

                    ...mergeEngineApproaches(cur.engineApproaches),

                    casparTransport: useCasparAsPrimary,

                }),

            });

        } catch { /* noop */ }

    }, [useCasparAsPrimary]);

    

    // Multi-channel state

    const [multiChannelState, setMultiChannelState] = useState<MultiChannelState>(() => {

        const saved = localStorage.getItem('multiChannelState');

        if (saved) {

            try {

                const parsed = JSON.parse(saved);

                // Migrate older saves that capped at 4 channels

                if (parsed && typeof parsed === 'object' && (!parsed.maxChannels || parsed.maxChannels < 8)) {

                    parsed.maxChannels = 8;

                }

                // Reset to ch-1 on load — prevents stale ch-N from a previous session

                // sending commands to the wrong CasparCG channel silently.

                if (parsed && typeof parsed === 'object') {

                    parsed.activeChannelId = 'ch-1';

                }

                return parsed;

            } catch (e) {

                logError('Failed to parse saved multi-channel state', e);

            }

        }

        // Pre-seed 8 channels; first 4 are enabled (have casparChannel assignment)

        const seedChannels: PlayoutChannel[] = Array.from({ length: 8 }, (_, i) => ({

            id: `ch-${i + 1}`,

            name: `Channel ${i + 1}`,

            scheduleId: i === 0 ? 'default' : `ch-${i + 1}`,

            previewEnabled: i < 4,

            previewPosition: 'main' as const,

            isPlaying: false,

            casparChannel: i + 1,

        } as PlayoutChannel & { casparChannel: number }));

        return {

            channels: seedChannels,

            activeChannelId: 'ch-1',

            layoutMode: '1x1',

            maxChannels: 8,

        };

    });



    // Keep Caspar channel bindings deterministic and valid (1..N).

    // Prevents stale mappings (e.g. CH7 with only 2 configured channels) that cause AMCP 400 errors.

    useEffect(() => {

        setMultiChannelState((prev) => {

            const remapped = prev.channels.map((ch, idx) => ({ ...ch, casparChannel: idx + 1 } as any));

            const changed = prev.channels.some((ch, idx) => ((ch as any).casparChannel || idx + 1) !== (idx + 1));

            if (!changed) return prev;

            return {

                ...prev,

                channels: remapped,

            };

        });

    }, []);

    

    const { openBreakoutWindow } = useChannelBreakout();



    // Auto-push: at the top of each hour, push each channel's scheduled content to CasparCG

    const [autoPushEnabled, setAutoPushEnabled] = useState<boolean>(() =>

        mergeEngineApproaches(loadSettings().engineApproaches).casparHourAutoPush

    );

    useEffect(() => {

        try { localStorage.setItem('playplay_autopush', String(autoPushEnabled)); } catch {}

        try {

            const { settingsManager } = require('../lib/settingsManager');

            const cur = settingsManager.getSettings();

            settingsManager.updateAutomationSettings({

                engineApproaches: mergeEngineApproaches({

                    ...mergeEngineApproaches(cur.engineApproaches),

                    casparHourAutoPush: autoPushEnabled,

                }),

            });

        } catch { /* noop */ }

    }, [autoPushEnabled]);

    useHourAutoPush({ enabled: autoPushEnabled, channels: multiChannelState.channels });

    useStartupCatchup({

        enabled: casparStartupCatchupEnabled,

        channels: multiChannelState.channels,

        casparRunning: casparStatus.running,

    });



    useEffect(() => {

        const onSaved = () => {

            const m = mergeEngineApproaches(loadSettings().engineApproaches);

            setUseCasparAsPrimary(m.casparTransport);

            setAutoPushEnabled(m.casparHourAutoPush);

            setCasparStartupCatchupEnabled(m.casparStartupCatchup);

        };

        window.addEventListener('playplay-settings-saved', onSaved);

        return () => window.removeEventListener('playplay-settings-saved', onSaved);

    }, []);



    // Save multi-channel state

    useEffect(() => {

        localStorage.setItem('multiChannelState', JSON.stringify(multiChannelState));

    }, [multiChannelState]);

    

    // File drop handling

    const handleFileDrop = useCallback(async (e: React.DragEvent) => {

        e.preventDefault();

        const files = Array.from(e.dataTransfer.files);

        if (files.length === 0) return;



        const { fileTransferService } = await import('../lib/fileTransferService');

        

        for (const file of files) {

            const targetSite = activeSiteId || 'local';

            const filePath = (file as any).path || file.name;

            await fileTransferService.initiateTransfer(targetSite, filePath, file.name);

        }

    }, [activeSiteId]);



    // Playlist state - Use global window object to persist across remounts

    const GLOBAL_STATE_KEY = '__playout_state__';

    const getGlobalState = (key: string, defaultValue: any) => {

        if (!(window as any)[GLOBAL_STATE_KEY]) (window as any)[GLOBAL_STATE_KEY] = {};

        if (typeof (window as any)[GLOBAL_STATE_KEY][key] === 'undefined') {

            (window as any)[GLOBAL_STATE_KEY][key] = defaultValue;

        }

        return (window as any)[GLOBAL_STATE_KEY][key];

    };

    const setGlobalState = (key: string, value: any) => {

        if (!(window as any)[GLOBAL_STATE_KEY]) (window as any)[GLOBAL_STATE_KEY] = {};

        (window as any)[GLOBAL_STATE_KEY][key] = value;

    };



    const [playlist, _setPlaylist] = useState<PlaylistItem[]>(() => getGlobalState('playlist', []));

    const setPlaylist = useCallback((val: PlaylistItem[] | ((prev: PlaylistItem[]) => PlaylistItem[])) => {

        _setPlaylist(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            setGlobalState('playlist', next);

            return next;

        });

    }, []);



    const [activeItemId, _setActiveItemId] = useState<string | null>(() => getGlobalState('activeItemId', null));

    const setActiveItemId = useCallback((val: string | null | ((prev: string | null) => string | null)) => {

        _setActiveItemId(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            setGlobalState('activeItemId', next);

            return next;

        });

    }, []);



    const [isPlaying, _setIsPlaying] = useState<boolean>(() => getGlobalState('isPlaying', false));

    const setIsPlaying = useCallback((val: boolean | ((prev: boolean) => boolean)) => {

        _setIsPlaying(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            setGlobalState('isPlaying', next);

            return next;

        });

    }, []);



    const [scheduleStartTime, _setScheduleStartTime] = useState<number>(() => getGlobalState('scheduleStartTime', Date.now()));

    const setScheduleStartTime = useCallback((val: number | ((prev: number) => number)) => {

        _setScheduleStartTime(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            setGlobalState('scheduleStartTime', next);

            return next;

        });

    }, []);



    const [loadedSlotId, _setLoadedSlotId] = useState<string | null>(() => getGlobalState('loadedSlotId', null));

    const setLoadedSlotId = useCallback((val: string | null | ((prev: string | null) => string | null)) => {

        _setLoadedSlotId(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            setGlobalState('loadedSlotId', next);

            return next;

        });

    }, []);



    const [isArmed, _setIsArmed] = useState<boolean>(() => getGlobalState('isArmed', true));

    const isArmedRef = useRef(isArmed);

    const setIsArmed = useCallback((val: boolean | ((prev: boolean) => boolean)) => {

        _setIsArmed(prev => {

            const next = typeof val === 'function' ? val(prev) : val;

            isArmedRef.current = next;

            setGlobalState('isArmed', next);

            return next;

        });

    }, []);



    const [playStartTime, setPlayStartTime] = useState<number | null>(null);

    const [isFreePlayMode, setIsFreePlayMode] = useState<boolean>(false);

    const [freePlayQueuedStart, setFreePlayQueuedStart] = useState<string>('');

    const [currentHourSlotId, setCurrentHourSlotId] = useState<string | null>(null);

    const [initialSeekTime, setInitialSeekTime] = useState<number | null>(null); // For time-accurate sync

    const [currentPlaybackTime, setCurrentPlaybackTime] = useState<number>(0); // Current position in seconds

    const [psipEvents, setPsipEvents] = useState<PSIPEvent[]>([]);

    const [isStrictMode, setIsStrictMode] = useState(false); // Strict mode - 10s compliance checks



    // ──────────────────────────────────────────────────────────────────────

    // Per-channel state snapshots

    //

    // Until we mount one PlayerEngine per channel (a deeper refactor), we

    // give each channel its OWN copy of the per-channel state and swap on

    // activeChannelId change. This stops the "loop" bug where adding CH2

    // collided with CH1's playlist + activeItemId + isPlaying.

    //

    // Concurrent on-air playback of multiple channels is a follow-up.

    // ──────────────────────────────────────────────────────────────────────

    type ChannelSnapshot = {

        playlist: PlaylistItem[];

        activeItemId: string | null;

        isPlaying: boolean;

        scheduleStartTime: number;

        loadedSlotId: string | null;

        currentHourSlotId: string | null;

        isArmed: boolean;

        isStrictMode: boolean;

        playStartTime: number | null;

        initialSeekTime: number | null;

    };

    const channelSnapshotsRef = useRef<Record<string, ChannelSnapshot>>({});

    const prevActiveChannelIdRef = useRef<string>(multiChannelState.activeChannelId);

    const channelSwitchInProgressRef = useRef<boolean>(false);



    const captureCurrentSnapshot = (): ChannelSnapshot => ({

        playlist,

        activeItemId,

        isPlaying,

        scheduleStartTime,

        loadedSlotId,

        currentHourSlotId,

        isArmed,

        isStrictMode,

        playStartTime,

        initialSeekTime,

    });

    const emptyChannelSnapshot: ChannelSnapshot = {

        playlist: [],

        activeItemId: null,

        isPlaying: false,

        scheduleStartTime: Date.now(),

        loadedSlotId: null,

        currentHourSlotId: null,

        isArmed: false,

        isStrictMode: false,

        playStartTime: null,

        initialSeekTime: null,

    };

    const applySnapshot = (s: ChannelSnapshot) => {

        setPlaylist(s.playlist);

        setActiveItemId(s.activeItemId);

        setIsPlaying(s.isPlaying);

        setScheduleStartTime(s.scheduleStartTime);

        setLoadedSlotId(s.loadedSlotId);

        setCurrentHourSlotId(s.currentHourSlotId);

        setIsArmed(s.isArmed);

        setIsStrictMode(s.isStrictMode);

        setPlayStartTime(s.playStartTime);

        setInitialSeekTime(s.initialSeekTime);

    };



    // Save+restore on channel switch

    // Channel switch is a PURE UI operation: it swaps the local playlist/preview

    // state via snapshots. It MUST NOT send CasparCG commands or touch the AMCP

    // queue. All CasparCG control happens only through explicit user actions

    // (push / play / pause / stop / clear buttons) or time-based auto-push hooks.

    useEffect(() => {

        const prev = prevActiveChannelIdRef.current;

        const next = multiChannelState.activeChannelId;

        if (prev === next) return;



        // Anchor inactive-rail playback time for `/api/state/ch-*` before playlist swap mutates refs

        channelPlaybackSyncRef.current[prev] = {

            wallMs: Date.now(),

            mediaSec: currentPlaybackTimeRef.current,

        };



        channelSwitchInProgressRef.current = true;



        // Snapshot the OUTGOING channel's current state

        channelSnapshotsRef.current[prev] = captureCurrentSnapshot();



        // Restore the INCOMING channel's snapshot, or empty state for fresh channels

        const incoming = channelSnapshotsRef.current[next] ?? emptyChannelSnapshot;

        applySnapshot(incoming);



        // Clear the channel-switch guard after state has settled so the 2s polling loops

        // (auto-start, top-of-hour) don't react to the restored playlist.

        setTimeout(() => { channelSwitchInProgressRef.current = false; }, 500);



        // Tell storage helpers which channel is now active so loadWeeklySchedule

        // / saveWeeklySchedule route to the right per-channel grid.

        (window as any).__activeChannelId = next;

        // Notify schedule editors to refetch.

        window.dispatchEvent(new CustomEvent('active-channel-changed', { detail: { channelId: next } }));

        window.dispatchEvent(new CustomEvent('schedule-updated', { detail: { reason: 'channel-switch', channelId: next } }));



        prevActiveChannelIdRef.current = next;

        // eslint-disable-next-line react-hooks/exhaustive-deps

    }, [multiChannelState.activeChannelId]);



    // On mount: seed the active channel global so the first load picks the right grid

    useEffect(() => {

        (window as any).__activeChannelId = multiChannelState.activeChannelId;

        // eslint-disable-next-line react-hooks/exhaustive-deps

    }, []);



    // Drop snapshots when channels are removed so they don't leak

    useEffect(() => {

        const liveIds = new Set(multiChannelState.channels.map(c => c.id));

        for (const id of Object.keys(channelSnapshotsRef.current)) {

            if (!liveIds.has(id)) delete channelSnapshotsRef.current[id];

        }

        for (const id of Object.keys(channelPlaybackSyncRef.current)) {

            if (!liveIds.has(id)) delete channelPlaybackSyncRef.current[id];

        }

    }, [multiChannelState.channels]);

    const [upcomingEvent, setUpcomingEvent] = useState<{ item: PlaylistItem; secondsUntil: number } | null>(null);

    const [countdownFlash, setCountdownFlash] = useState(false); // Flash state for countdown

    const [isOutputWindowOpen, setIsOutputWindowOpen] = useState(false); // Track output window state

    const [outputWindowUrl, setOutputWindowUrl] = useState<string>(''); // Output window URL for OBS/browser

    const [showOutputUrlTooltip, setShowOutputUrlTooltip] = useState(false); // Show URL tooltip

    const [hasUnseenScheduleChanges, setHasUnseenScheduleChanges] = useState(false);

    const [asRunReport, setAsRunReport] = useState<MediaAsRunReport | null>(null);

    const [showFacebookStoryPoster, setShowFacebookStoryPoster] = useState(false); // Show Facebook Story Poster

    const [showSocialCampaignPanel, setShowSocialCampaignPanel] = useState(false); // Show FB/Website campaign poster

    // MPV broadcast engine removed - using built-in engine only

    const [mpvEngineStatus, setMpvEngineStatus] = useState<any>(null);

    const [mpvStarting, setMpvStarting] = useState(false);

    const playerWrapperRef = useRef<HTMLDivElement>(null);

    const currentPlaybackTimeRef = useRef<number>(0);

    /** Wall-clock extrapolation anchors for OBS `/outputN` URLs (inactive rails + paused freeze). */

    const channelPlaybackSyncRef = useRef<Record<string, ChannelPlaybackSync>>({});

    const lastPlaybackUiSyncRef = useRef<number>(0);

    const lastUpcomingIdRef = useRef<string | null>(null);

    const lastCurrentMediaIdRef = useRef<string | null>(null);

    const automationEngineRef = useRef<AutomationEngine | null>(null);

    const saveSnapInFlightRef = useRef(false);

    const [automationState, setAutomationState] = useState<AutomationState | null>(null);



    const handlePlaybackTimeUpdate = useCallback((time: number) => {

        currentPlaybackTimeRef.current = time;

        channelPlaybackSyncRef.current[multiChannelState.activeChannelId] = {

            wallMs: Date.now(),

            mediaSec: time,

        };

        // Scheduler grid is expensive; avoid pumping time-driven rerenders into it.

        if (viewMode === 'scheduler') return;

        const now = performance.now();

        const minSyncMs = viewMode === 'playout' ? 160 : 1200;

        if (now - lastPlaybackUiSyncRef.current < minSyncMs) return;

        lastPlaybackUiSyncRef.current = now;

        setCurrentPlaybackTime((prev) => (Math.abs(prev - time) >= 0.05 ? time : prev));

    }, [viewMode, multiChannelState.activeChannelId]);



    // Freeze aggregate playhead when paused so HTTP `/api/state` doesn't keep drifting.

    useEffect(() => {

        const id = multiChannelState.activeChannelId;

        if (!id) return;

        if (!isPlaying) {

            channelPlaybackSyncRef.current[id] = {

                wallMs: Date.now(),

                mediaSec: currentPlaybackTimeRef.current,

            };

        }

    }, [isPlaying, multiChannelState.activeChannelId]);



    useEffect(() => {

        const flagScheduleDirty = (event: Event) => {

            const custom = event as CustomEvent;

            if (custom?.detail?.source === 'self') return;

            // Channel switching is a pure UI preview swap; don't flag the schedule as dirty.

            if (custom?.detail?.reason === 'channel-switch') return;

            setHasUnseenScheduleChanges(true);

        };

        window.addEventListener('schedule-updated', flagScheduleDirty as EventListener);

        return () => {

            window.removeEventListener('schedule-updated', flagScheduleDirty as EventListener);

        };

    }, []);



    // Resizable columns

    const leftResizer = useResizer(380, 280, 600, 'playout-left-col-width');

    const rightResizer = useResizer(760, 620, 1400, 'playout-right-col-width');



    // Playout Engine Manager

    const playoutEngineRef = useRef<PlayoutEngineManager | null>(null);



    // Refresh site content filler periodically for evergreen slates (traffic/weather/alerts/etc.)

    useEffect(() => {

        refreshSiteFiller().catch(() => {});

        const interval = setInterval(() => refreshSiteFiller().catch(() => {}), 30 * 60 * 1000);

        return () => clearInterval(interval);

    }, []);



    // Helper: rehydrate PlaylistItems' media from current library/DB so old sessions don't carry stale blobs

    const rehydrateItemsFromLibrary = useCallback(async (items: PlaylistItem[]): Promise<PlaylistItem[]> => {

        if (!items || items.length === 0) return items;



        // Fetch canonical media sets

        const [dbMedia, localMedia, storedLibrary] = await Promise.all([

            dbFetchMedia().catch(() => []),

            Promise.resolve(loadLocalMedia()),

            Promise.resolve(loadLibraryMedia([])),

        ]);



        // Build multiple lookup indices for fallback matching

        const mediaById = new Map<string, MediaItem>();

        const mediaByTitle = new Map<string, MediaItem>();

        const mediaByFilename = new Map<string, MediaItem>();



        const localById = new Map<string, MediaItem>();

        localMedia.forEach(m => { if (m?.id) localById.set(m.id, m); });

        [...storedLibrary, ...localMedia, ...dbMedia].forEach(m => {

            if (m && m.id) {

                mediaById.set(m.id, m);

                if (m.title) mediaByTitle.set(m.title.toLowerCase().trim(), m);

                const filename = (m.filename || (m.filePath ? m.filePath.split(/[/\\]/).pop() : '') || '').toLowerCase();

                if (filename) mediaByFilename.set(filename, m);

            }

        });



        const isElectronEnv = isElectron;



        // Helper: build a valid src from a MediaItem using universal resolver

        const buildValidSrc = (m: MediaItem): string => {

            // Use the universal resolver for consistent URL handling

            return resolveMediaUrlSync(m);

        };



        // Create fallback media for missing sources (shows test pattern)

        const createMissingFallback = (original: MediaItem | undefined, title: string): MediaItem => ({

            id: `missing_${generateId()}`,

            title: `⚠ MISSING: ${title}`,

            duration: original?.duration || 10,

            type: 'gap',

            src: 'signal:bars', // Test pattern - always works

            thumbnail: '#ff4444',

        });



        return items.map(item => {

            // Try lookup by ID first

            let canonical = mediaById.get(item.mediaId);



            // Fallback: try matching by title

            if (!canonical && item.media?.title) {

                canonical = mediaByTitle.get(item.media.title.toLowerCase().trim());

            }



            // Fallback: try matching by filename from embedded media's filePath

            if (!canonical && item.media?.filePath) {

                const filename = item.media.filePath.split(/[/\\]/).pop()?.toLowerCase() || '';

                if (filename) canonical = mediaByFilename.get(filename);

            }



            // If we found canonical media, use it (merge segment data from local so edits persist)

            if (canonical) {

                const fromLocal = localById.get(item.mediaId || item.media?.id);

                const withSegments = fromLocal?.segmentCollections?.length || fromLocal?.segments?.length

                    ? { ...canonical, segmentCollections: fromLocal.segmentCollections ?? canonical.segmentCollections, segments: fromLocal.segments ?? canonical.segments, activeCollectionId: fromLocal.activeCollectionId ?? canonical.activeCollectionId }

                    : canonical;

                const src = buildValidSrc(withSegments);

                if (src) {

                    return {

                        ...item,

                        media: { ...withSegments, src },

                    };

                }

            }



            // Try to salvage the embedded media

            const media = item.media;

            if (media) {

                const src = buildValidSrc(media);

                if (src) {

                    return {

                        ...item,

                        media: { ...media, src },

                    };

                }



                // No valid source - use fallback so playback doesn't break

                persistentLogger.warn(`No valid source for: ${media.title} - using test pattern fallback`, undefined, 'Rehydrate');

                return {

                    ...item,

                    media: createMissingFallback(media, media.title || 'Unknown'),

                };

            }



            // Completely orphaned - create fallback

            persistentLogger.warn(`[Rehydrate] Orphaned playlist item: ${item.id} - using test pattern fallback`, undefined, 'Rehydrate');

            return {

                ...item,

                media: createMissingFallback(undefined, 'Unknown Media'),

            };

        });

    }, []);

    const [activeEngineId, setActiveEngineId] = useState<string | null>(null);

    const [enginePlaybackState, setEnginePlaybackState] = useState<EnginePlaybackState | null>(null);



    // CG state

    const cgController = useCGController([]);

    const { layers: cgLayers } = useCGState();



    // OBS state

    const [obsSessions, setObsSessions] = useState<OBSSession[]>(DEFAULT_OBS_SESSIONS);

    const obsClientsRef = useRef<Map<string, OBSWebSocketClient>>(new Map());

    // Item start the OBS start commands last ran for. Keeps them to once per item start.
    const obsStartKeyRef = useRef<string | null>(null);



    const resolveObsWsClient = useCallback<OBSClientResolver>((command) => {

        const map = obsClientsRef.current;

        const cmdSession =

            command?.sessionId

            ?? (command as { obsSessionId?: string } | null)?.obsSessionId;

        if (cmdSession) {

            const c = map.get(cmdSession);

            return c?.isConnected ? c : undefined;

        }

        const activeCh = multiChannelState.channels.find(c => c.id === multiChannelState.activeChannelId);

        const sid = activeCh?.outputConfig?.obsSessionId;

        if (sid) {

            const c = map.get(sid);

            if (c?.isConnected) return c;

        }

        return Array.from(map.values()).find(c => c.isConnected);

    }, [multiChannelState.channels, multiChannelState.activeChannelId]);



    const handleChannelObsSessionChange = useCallback((channelId: string, sessionId: string | undefined) => {

        setMultiChannelState(prev => ({

            ...prev,

            channels: prev.channels.map(c =>

                c.id === channelId

                    ? { ...c, outputConfig: { ...c.outputConfig, obsSessionId: sessionId } }

                    : c

            ),

        }));

    }, []);



    // CG on start/end: track previous item's cgOnStart so we can hide when switching

    const lastCgOnStartIdsRef = useRef<string[]>([]);



    // DDR state

    const [ddrChannels, setDdrChannels] = useState<Record<DDRChannel, DDRPlaylist>>({

        A: createEmptyDDRPlaylist('A'),

        B: createEmptyDDRPlaylist('B'),

        C: createEmptyDDRPlaylist('C'),

        D: createEmptyDDRPlaylist('D'),

    });

    const [ddrAddChannel, setDdrAddChannel] = useState<DDRChannel | null>(null);

    const [ddrPickerMedia, setDdrPickerMedia] = useState<MediaItem[]>([]);



    // Load library media when DDR "Add" picker is open

    useEffect(() => {

        if (!ddrAddChannel) return;

        let cancelled = false;

        (async () => {

            const [db, lib, local] = await Promise.all([

                dbFetchMedia().catch(() => []),

                Promise.resolve(loadLibraryMedia([])),

                Promise.resolve(loadLocalMedia()),

            ]);

            const byId = new Map<string, MediaItem>();

            [...local, ...lib, ...db].filter(Boolean).forEach((m) => {

                if (m.id && !byId.has(m.id)) byId.set(m.id, m);

            });

            if (!cancelled) setDdrPickerMedia(Array.from(byId.values()));

        })();

        return () => { cancelled = true; };

    }, [ddrAddChannel]);



    // Context menu

    const { menuState, showMenu, hideMenu } = useContextMenu();



    // Help panel

    const [helpOpen, setHelpOpen] = useState(false);

    const [logViewerOpen, setLogViewerOpen] = useState(false);



    // F1 keyboard shortcut for help, F2 for logs

    useEffect(() => {

        const handleKeyDown = (e: KeyboardEvent) => {

            if (e.key === 'F1') {

                e.preventDefault();

                setHelpOpen(true);

            } else if (e.key === 'F2') {

                e.preventDefault();

                setLogViewerOpen(true);

            }

        };

        window.addEventListener('keydown', handleKeyDown);

        return () => window.removeEventListener('keydown', handleKeyDown);

    }, []);



    // Log component mount/unmount

    useEffect(() => {

        persistentLogger.info('PlayoutController: Component mounted [v3-loop-fix]', {

            timestamp: Date.now(),

            playlistLength: playlist.length,

            activeItemId,

            isPlaying,

            isArmed,

        }, 'PlayoutController');

        

        return () => {

            persistentLogger.info('PlayoutController: Component unmounting', {

                timestamp: Date.now(),

                playlistLength: playlist.length,

            }, 'PlayoutController');

        };

    }, []); // Only run on mount/unmount



    // Log playlist changes (only when length or activeItemId actually changes)

    const playlistLengthRef = useRef(playlist.length);

    const activeItemIdRef = useRef(activeItemId);

    useEffect(() => {

        if (playlistLengthRef.current !== playlist.length || activeItemIdRef.current !== activeItemId) {

            persistentLogger.debug('Playlist changed', {

                length: playlist.length,

                previousLength: playlistLengthRef.current,

                activeItemId,

                previousActiveItemId: activeItemIdRef.current,

            }, 'PlayoutController');

            playlistLengthRef.current = playlist.length;

            activeItemIdRef.current = activeItemId;

        }

    }, [playlist.length, activeItemId]);



    // Initialize Playout Engine Manager - use GLOBAL ref to prevent re-initialization across remounts

    // Store in window to persist across component remounts

    const GLOBAL_ENGINE_MANAGER_KEY = '__playout_engine_manager__';

    const hasInitializedEngine = (window as any)[GLOBAL_ENGINE_MANAGER_KEY + '_initialized'] || false;

    

    useEffect(() => {

        if (hasInitializedEngine) {

            // Reuse existing manager from window

            const existingManager = (window as any)[GLOBAL_ENGINE_MANAGER_KEY];

            if (existingManager) {

                persistentLogger.warn('PlayoutEngineManager already initialized, reusing existing instance', {}, 'PlayoutController');

                playoutEngineRef.current = existingManager;

                return;

            }

        }

        

        // Mark as initialized globally

        (window as any)[GLOBAL_ENGINE_MANAGER_KEY + '_initialized'] = true;

        

        persistentLogger.info('Initializing PlayoutEngineManager', {}, 'PlayoutController');

        const engineOpts = mergeEngineApproaches(loadSettings().engineApproaches);

        const manager = new PlayoutEngineManager(

            (engine) => {

                persistentLogger.debug('PlayoutEngine status changed', { engine: engine.id, status: engine.status }, 'PlayoutController');

            },

            (state) => {

                setEnginePlaybackState(state);

            },

            {

                preferMpv: engineOpts.mpvBroadcastEngine,

                fallbackToElectron: engineOpts.mpvFallbackToElectron,

            }

        );

        

        // Store globally to persist across remounts

        (window as any)[GLOBAL_ENGINE_MANAGER_KEY] = manager;

        playoutEngineRef.current = manager;



        // Load saved engines

        const saved = localStorage.getItem('playout_engines');

        if (saved) {

            try {

                const configs: PlayoutEngineConfig[] = JSON.parse(saved);

                configs.forEach(config => {

                    manager.addEngine(config);

                });



                // Set internal as active engine (only engine type supported)

                manager.setActiveEngine('internal');

                setActiveEngineId('internal');

            } catch (e) {

                logError('Failed to load playout engines', e);

            }

        } else {

            manager.setActiveEngine('internal');

            setActiveEngineId('internal');

        }



        return () => {

            manager.getEngines().forEach(e => {

                manager.removeEngine(e.id);

            });

        };

    }, []);



    // Load saved CG elements

    useEffect(() => {

        const saved = loadCGElements();

        if (saved.length > 0) {

            cgController.setElements(saved);

        }

    }, []);



    // Save CG elements on change

    useEffect(() => {

        saveCGElements(cgController.elements);

    }, [cgController.elements]);



    // Get output window URL on mount

    useEffect(() => {

        const getOutputUrl = async () => {

            if (isElectron && (window as any).electronAPI?.getOutputUrl) {

                try {

                    const result = await (window as any).electronAPI.getOutputUrl();

                    // getOutputUrl returns { local, external, port, ip }

                    const url = typeof result === 'string' ? result : result?.external || result?.local || 'http://localhost:8080/output';

                    safeSetState(() => setOutputWindowUrl(url));

                    persistentLogger.debug('Output URL: ' + url, undefined, 'PlayoutController');

                } catch (err) {

                    logError('Failed to get output URL', err);

                    safeSetState(() => setOutputWindowUrl('http://localhost:8080/output'));

                }

            } else {

                safeSetState(() => setOutputWindowUrl(`${window.location.origin}/#/output`));

            }

        };

        getOutputUrl();

    }, [safeSetState]);



    // Load current hour from schedule when switching to playout view (only if playlist is empty)

    useEffect(() => {

        if (viewMode === 'playout' && playlist.length === 0 && !loadedSlotId) {

            const now = new Date();

            const dayOfWeek = now.getDay(); // 0 = Sunday, 1 = Monday, etc.

            const hour = now.getHours();

            const slotId = `${dayOfWeek}-${hour}`;



            persistentLogger.debug('Auto-loading current hour slot: ' + slotId, undefined, 'PlayoutController');

            void (async () => {

                const schedule = await loadWeeklySchedule();

                const currentSlot = schedule.slots[slotId];



                if (currentSlot && currentSlot.items.length > 0) {

                    persistentLogger.debug(`Auto-loading schedule slot into playout: ${slotId}, ${currentSlot.items.length} items`, undefined, 'PlayoutController');

                    // Rehydrate media from library/DB and generate new IDs

                    const hydrated = await rehydrateItemsFromLibrary(currentSlot.items);

                    const items = hydrated.map(item => ({

                        ...item,

                        id: generateId(),

                    }));

                    safeSetState(() => {

                        setPlaylist(items);

                        setLoadedSlotId(slotId);

                        if (items.length > 0) setActiveItemId(items[0].id);

                    });

                } else {

                    persistentLogger.debug(`No items in current schedule slot: ${slotId} - user can manually load a different hour`, undefined, 'PlayoutController');

                }

            })();

        }

    }, [viewMode, playlist.length, loadedSlotId]);



    // Calculate current hour slot ID based on actual current time

    useEffect(() => {

        if (isPlaying && playlist.length > 0) {

            const updateCurrentHourSlot = () => {

                const now = new Date();

                const dayOfWeek = now.getDay(); // 0 = Sunday

                const hour = now.getHours();

                const slotId = `${dayOfWeek}-${hour}`;

                persistentLogger.debug(`Updating current hour slot: ${slotId} Time: ${now.toLocaleTimeString()}`, undefined, 'PlayoutController');

                setCurrentHourSlotId(slotId);

            };



            // Update immediately

            updateCurrentHourSlot();



            // Update every minute to catch hour changes

            const interval = setInterval(updateCurrentHourSlot, 60000);

            return () => clearInterval(interval);

        } else {

            persistentLogger.debug('Not tracking hour slot (not playing or playlist empty)', undefined, 'PlayoutController');

            setCurrentHourSlotId(null);

        }

    }, [isPlaying, playlist]);



    // Listen for schedule updates and (optionally) update current playlist if current hour was modified.

    // Important: editing other hours must NOT affect playout ("the show must go on").

    useEffect(() => {

        if (!currentHourSlotId || !isPlaying) {

            persistentLogger.debug('Schedule update listener not active', { currentHourSlotId, isPlaying }, 'PlayoutController');

            return;

        }



        persistentLogger.debug('Setting up schedule update listener for slot: ' + currentHourSlotId, undefined, 'PlayoutController');



        const handleScheduleUpdate = (event: CustomEvent) => {

            // Only allow automatic schedule-following when explicitly armed AND in playout view.

            // Otherwise, schedule editing can cause unintended jumps.

            if (viewMode !== 'playout' || !isArmed) return;



            const updatedSlotId = event.detail?.slotId;

            persistentLogger.debug('Schedule update event received', { updatedSlotId, currentHourSlotId }, 'PlayoutController');



            // If the updated slot is the current hour, reload it

            // Also handle case where allSlots is true (full schedule reload)

            const isCurrentHourUpdate = updatedSlotId === currentHourSlotId;

            const isFullScheduleUpdate = event.detail?.allSlots === true;

            

            if (isCurrentHourUpdate || (isFullScheduleUpdate && currentHourSlotId)) {

                persistentLogger.debug('Current hour slot was updated, reloading playlist...', { updatedSlotId, isFullScheduleUpdate }, 'PlayoutController');

                void (async () => {

                    // Always load fresh schedule

                    const schedule = await loadWeeklySchedule();

                    const currentSlot = schedule.slots[currentHourSlotId || updatedSlotId];



                    if (currentSlot) {

                        persistentLogger.debug('Current slot found', { itemCount: currentSlot.items.length }, 'PlayoutController');



                        // Rehydrate media from library/DB and generate new IDs

                        const hydrated = await rehydrateItemsFromLibrary(currentSlot.items);

                        const updatedItems = hydrated.map(item => ({

                            ...item,

                            id: generateId(), // Generate new IDs to avoid conflicts

                        }));



                        // Find where we are in the current playlist

                        const currentIndex = playlist.findIndex(item => item.id === activeItemId);

                        persistentLogger.debug('Current playlist index: ' + currentIndex + ' Active item ID: ' + activeItemId, undefined, 'PlayoutController');



                        if (currentIndex >= 0 && currentIndex < playlist.length) {

                            // Keep items before current position, replace from current onwards

                            const newPlaylist = [

                                ...playlist.slice(0, currentIndex),

                                ...updatedItems

                            ];

                            persistentLogger.debug('Updating playlist', {

                                oldLength: playlist.length,

                                newLength: newPlaylist.length,

                                keptItems: currentIndex,

                                newItems: updatedItems.length

                            }, 'PlayoutController');

                            const nextActiveId = (updatedItems.length > 0 && currentIndex === playlist.length - 1)

                                ? updatedItems[0].id

                                : (updatedItems.length > 0 && newPlaylist.findIndex(item => item.id === activeItemId) < 0)

                                    ? updatedItems[0].id

                                    : activeItemId;

                            safeSetState(() => {

                                setPlaylist(newPlaylist);

                                if (updatedItems.length > 0) setActiveItemId(nextActiveId);

                            });

                        } else {

                            persistentLogger.debug('Current item not found, replacing entire playlist', undefined, 'PlayoutController');

                            safeSetState(() => {

                                setPlaylist(updatedItems);

                                if (updatedItems.length > 0) setActiveItemId(updatedItems[0].id);

                            });

                        }

                    } else {

                        persistentLogger.warn('Current slot not found in schedule', undefined, 'PlayoutController');

                    }

                })();

            }

        };



        // Listen for schedule update events

        window.addEventListener('schedule-updated', handleScheduleUpdate as EventListener);



        // Also check periodically for changes (in case event doesn't fire)

        const checkScheduleUpdate = async () => {

            if (viewMode !== 'playout' || !isArmed) return;

            const schedule = await loadWeeklySchedule();

            const currentSlot = schedule.slots[currentHourSlotId];



            if (currentSlot) {

                const storedLastCheck = localStorage.getItem(`playplay_slot_check_${currentHourSlotId}`);

                const slotLastModified = schedule.lastModified;



                if (!storedLastCheck || parseInt(storedLastCheck) < slotLastModified) {

                    persistentLogger.debug('Schedule change detected via polling', {

                        storedLastCheck,

                        slotLastModified

                    }, 'PlayoutController');

                    // Slot was updated

                    handleScheduleUpdate(new CustomEvent('schedule-updated', {

                        detail: { slotId: currentHourSlotId }

                    }));

                    localStorage.setItem(`playplay_slot_check_${currentHourSlotId}`, slotLastModified.toString());

                }

            }

        };



        // Check immediately and then every 5 seconds

        checkScheduleUpdate();

        const interval = setInterval(checkScheduleUpdate, 5000);



        return () => {

            window.removeEventListener('schedule-updated', handleScheduleUpdate as EventListener);

            clearInterval(interval);

        };

    }, [currentHourSlotId, isPlaying, playlist, activeItemId, viewMode, isArmed]);



    // MPV Broadcast Engine removed - using built-in engine only

    // These are stub handlers for UI compatibility

    const handleMPVToggle = async () => {

        persistentLogger.debug('MPV broadcast engine has been removed - using built-in engine', undefined, 'PlayoutController');

    };



    // Smart Slot Resolution

    const resolveSmartSlot = (config: SmartSlotConfig): MediaItem | null => {

        const settings = loadSettings();

        if (config.type === 'latest' || (settings.alwaysPlayLatestEpisode && config.seriesName)) {

            const library = loadLibraryMedia();

            const episodes = library.filter(

                m => m.programSeries === config.seriesName && m.episodeDate

            ).sort((a, b) => new Date(b.episodeDate!).getTime() - new Date(a.episodeDate!).getTime());

            return episodes.length > 0 ? episodes[0] : null;

        }

        return null;

    };



    // Effective duration helper for segmented / time-in/out items

    const getPlaylistItemDurationSeconds = (item: PlaylistItem): number => {

        const timeIn = item.timeIn || 0;

        const timeOut = item.timeOut ?? item.media.duration;

        const effective = timeOut - timeIn;

        return Math.max(0, effective);

    };



    // Time-based scheduling with gap insertion

    const calculateSchedule = useCallback((): PlaylistItem[] => {

        const result: PlaylistItem[] = [];

        // For single-hour slots, clamp any overage to the top-of-next-hour.

        // If hours are merged (loadedSlotId contains ','), allow continuous scheduling.

        const isMergedHours = !!loadedSlotId && loadedSlotId.includes(',');

        const isSingleHourMode = !!loadedSlotId && !isMergedHours;

        const hourStart = (() => {

            const d = new Date(scheduleStartTime);

            d.setMinutes(0, 0, 0);

            return d.getTime();

        })();

        const hourEnd = hourStart + 60 * 60 * 1000;



        let currentTime = scheduleStartTime;



        for (const item of playlist) {

            if (isSingleHourMode && currentTime >= hourEnd) break;



            if (item.isFixedTime && item.fixedTime) {

                const fixedStart = item.fixedTime;

                if (fixedStart > currentTime) {

                    let gapDuration = fixedStart - currentTime;

                    if (isSingleHourMode && currentTime + gapDuration > hourEnd) {

                        gapDuration = hourEnd - currentTime;

                    }

                    const gapMedia = createGapMedia(Math.max(0, gapDuration));

                    result.push({

                        // Use a stable deterministic ID for gaps

                        id: `gap_${currentTime}_${fixedStart}`,

                        mediaId: gapMedia.id,

                        media: gapMedia,

                        startTime: currentTime,

                        endTime: fixedStart,

                        isResolved: true,

                    });

                    currentTime = currentTime + gapDuration;

                    if (isSingleHourMode && currentTime >= hourEnd) break;

                }

                const startTime = fixedStart;

                const durMs = getPlaylistItemDurationSeconds(item) * 1000;

                let endTime = startTime + durMs;

                if (isSingleHourMode && endTime > hourEnd) {

                    // Truncate the item via timeOut so playback stops at TOH

                    const timeIn = item.timeIn || 0;

                    const effectiveMs = Math.max(0, hourEnd - startTime);

                    const newTimeOut = timeIn + (effectiveMs / 1000);

                    result.push({ ...item, startTime, endTime: hourEnd, timeOut: Math.max(timeIn + 0.1, newTimeOut) });

                    currentTime = hourEnd;

                    break;

                } else {

                    result.push({ ...item, startTime, endTime });

                    currentTime = endTime;

                }

            } else {

                const startTime = currentTime;

                const durMs = getPlaylistItemDurationSeconds(item) * 1000;

                let endTime = startTime + durMs;

                if (isSingleHourMode && endTime > hourEnd) {

                    const timeIn = item.timeIn || 0;

                    const effectiveMs = Math.max(0, hourEnd - startTime);

                    const newTimeOut = timeIn + (effectiveMs / 1000);

                    result.push({ ...item, startTime, endTime: hourEnd, timeOut: Math.max(timeIn + 0.1, newTimeOut) });

                    currentTime = hourEnd;

                    break;

                } else {

                    result.push({ ...item, startTime, endTime });

                    currentTime = endTime;

                }

            }

        }

        return result;

    }, [playlist, scheduleStartTime, loadedSlotId]);



    // Merge multiple hour slots into a single continuous playlist

    const mergeHourSlots = useCallback((slots: HourSlot[]): PlaylistItem[] => {

        const merged: PlaylistItem[] = [];

        if (slots.length === 0) return merged;



        // Sort slots by day/hour

        const ordered = [...slots].sort((a, b) =>

            a.dayOfWeek === b.dayOfWeek ? a.hour - b.hour : (a.dayOfWeek ?? 0) - (b.dayOfWeek ?? 0)

        );



        const first = ordered[0];

        const base = new Date();

        base.setHours(first.hour, 0, 0, 0);

        let currentTimeMs = base.getTime();



        for (const slot of ordered) {

            for (const item of slot.items) {

                const timeIn = item.timeIn || 0;

                const timeOut = item.timeOut ?? (item.media?.duration || 0);

                const durMs = Math.max(0, (timeOut - timeIn)) * 1000;

                if (durMs <= 0) continue;



                merged.push({

                    ...item,

                    // Use a stable ID combining original ID and start time to prevent infinite loops

                    id: item.id.startsWith('scheduled_') ? item.id : `scheduled_${item.id}_${currentTimeMs}`,

                    startTime: currentTimeMs,

                    endTime: currentTimeMs + durMs,

                });

                currentTimeMs += durMs;

            }

        }



        return merged;

    }, []);



    // Memoize calculatedPlaylist with stable dependencies

    // Remove activeItemId, isArmed, isPlaying as they don't affect the schedule calculation

    const lastCalculatedPlaylistRef = useRef<PlaylistItem[]>([]);

    const calculatedPlaylist = useMemo(() => {

        const result = calculateSchedule();

        

        // Deep check for changes to prevent unnecessary re-renders

        const hasChanged = result.length !== lastCalculatedPlaylistRef.current.length ||

            result.some((item, i) => {

                const prev = lastCalculatedPlaylistRef.current[i];

                return !prev || item.id !== prev.id || item.startTime !== prev.startTime || item.endTime !== prev.endTime;

            });



        if (hasChanged) {

            persistentLogger.debug('calculatedPlaylist recalculated (content changed)', { 

                length: result.length,

                playlistLength: playlist.length,

                loadedSlotId,

            }, 'PlayoutController');

            lastCalculatedPlaylistRef.current = result;

            return result;

        }

        

        return lastCalculatedPlaylistRef.current;

    }, [playlist, scheduleStartTime, loadedSlotId, calculateSchedule]);



    const calculatedPlaylistRef = useRef<PlaylistItem[]>([]);

    useEffect(() => {

        calculatedPlaylistRef.current = calculatedPlaylist;

    }, [calculatedPlaylist]);



    // Derived state - memoize to prevent unnecessary recalculations

    const activeIndex = useMemo(() => 

        calculatedPlaylist.findIndex(item => item.id === activeItemId),

        [calculatedPlaylist, activeItemId]

    );

    

    const currentMedia = useMemo(() => 

        activeIndex >= 0 ? calculatedPlaylist[activeIndex] : undefined,

        [calculatedPlaylist, activeIndex]

    );



    // Sync timeline to engine

    useEffect(() => {

        if (automationEngineRef.current && calculatedPlaylist.length > 0) {

            const events = calculatedPlaylist.map(item => ({

                id: item.id,

                mediaId: item.mediaId || item.media?.id || item.id,

                media: item.media,

                startTime: item.startTime!,

                endTime: item.endTime!,

                duration: (item.endTime! - item.startTime!),

                priority: item.isFixedTime ? 'fixed' : 'normal' as any,

            }));

            automationEngineRef.current.setTimeline(events);

        }

    }, [calculatedPlaylist]);



    // Initialize Automation Engine

    useEffect(() => {

        const engine = getEngine();

        automationEngineRef.current = engine;



        const handleCommand = (cmd: RendererCommand) => {

            if (!isArmedRef.current) return;

            const manager = playoutEngineRef.current;

            if (!manager) return;





            switch (cmd.type) {

                case 'load':

                    if (cmd.mediaId) {

                        const item = calculatedPlaylist.find(i => i.id === cmd.mediaId || i.mediaId === cmd.mediaId);

                        if (item) {

                            manager.playMediaAt(item, (cmd.seekTo || 0) / 1000);

                        }

                    }

                    break;

                case 'play':

                    manager.resume();

                    break;

                case 'pause':

                    manager.pause();

                    break;

                case 'stop':

                    manager.stop();

                    break;

                case 'seek':

                    if (cmd.seekTo !== undefined) {

                        manager.seek(cmd.seekTo / 1000);

                    }

                    break;

            }

        };



        const handleStateChange = (state: AutomationState) => {

            setAutomationState(state);

            if (state.activeEventId) {

                setActiveItemId(state.activeEventId);

            }

        };



        engine.start({

            onCommand: handleCommand,

            onStateChange: handleStateChange,

            onError: (err) => logError('Automation Engine Error', err),

        });



        return () => engine.stop();

    }, [calculatedPlaylist]);



    // Report renderer position to engine

    useEffect(() => {

        const interval = setInterval(() => {

            if (automationEngineRef.current && currentMedia) {

                automationEngineRef.current.reportRendererState(currentMedia.id, currentPlaybackTime * 1000);

            }

        }, 200);

        return () => clearInterval(interval);

    }, [currentMedia, currentPlaybackTime]);



    // Item ended handling now done via PlayerEngine onEnded callback

    // MPV broadcast engine has been removed

    

    const nextMedia = useMemo(() => 

        activeIndex >= 0 && activeIndex < calculatedPlaylist.length - 1

            ? calculatedPlaylist[activeIndex + 1]

            : undefined,

        [calculatedPlaylist, activeIndex]

    );



    // Sync per-channel currentItem/nextItem/isPlaying into multiChannelState.channels so that

    // PreviewGrid / ChannelTabs / MultiChannelManager show fresh "Next:" and LIVE/STOPPED for every channel.

    // Active channel: derived from top-level currentMedia/nextMedia/isPlaying.

    // Inactive channels: derived from their captured snapshot in channelSnapshotsRef.

    // Runs on every state change AND every 500ms to catch ref-only snapshot mutations

    // (e.g. when an inactive channel's media ends and its snapshot advances).

    useEffect(() => {

        const computeChannelState = (

            ch: PlayoutChannel,

            isActive: boolean

        ): { current: PlaylistItem | undefined; next: PlaylistItem | undefined; playing: boolean } => {

            if (isActive) {

                return { current: currentMedia, next: nextMedia, playing: isPlaying };

            }

            const snap = channelSnapshotsRef.current[ch.id];

            if (!snap || !snap.activeItemId || snap.playlist.length === 0) {

                return { current: undefined, next: undefined, playing: false };

            }

            const idx = snap.playlist.findIndex(p => p.id === snap.activeItemId);

            if (idx < 0) return { current: undefined, next: undefined, playing: false };

            return {

                current: snap.playlist[idx],

                next: idx < snap.playlist.length - 1 ? snap.playlist[idx + 1] : undefined,

                playing: snap.isPlaying,

            };

        };



        const syncOnce = () => {

            setMultiChannelState((prev) => {

                let mutated = false;

                const activeId = prev.activeChannelId;

                const updatedChannels = prev.channels.map((ch) => {

                    const { current, next, playing } = computeChannelState(ch, ch.id === activeId);

                    const curId = current?.id ?? null;

                    const nxtId = next?.id ?? null;

                    const prevCurId = ch.currentItem?.id ?? null;

                    const prevNxtId = ch.nextItem?.id ?? null;

                    const prevPlaying = ch.isPlaying;

                    if (curId === prevCurId && nxtId === prevNxtId && playing === prevPlaying) return ch;

                    mutated = true;

                    return { ...ch, currentItem: current, nextItem: next, isPlaying: playing };

                });

                return mutated ? { ...prev, channels: updatedChannels } : prev;

            });

        };



        syncOnce();

        const interval = window.setInterval(syncOnce, 500);

        return () => window.clearInterval(interval);

    }, [currentMedia, nextMedia, isPlaying, multiChannelState.activeChannelId]);



    const logAsPlayed = useCallback((completed: boolean) => {

        if (!playStartTime || !currentMedia) return;

        const actualStart = playStartTime;

        const actualEnd = Date.now();

        const duration = Math.max(1, Math.floor((actualEnd - actualStart) / 1000));

        const playlistIndex = playlist.findIndex(i => i.id === currentMedia.id);



        recordPlayEvent(currentMedia.mediaId, {

            timestamp: actualStart,

            duration,

            completed,

            hourSlot: loadedSlotId || currentHourSlotId || undefined,

            itemId: currentMedia.id,

            mediaTitle: currentMedia.media?.title,

            scheduledStart: currentMedia.startTime,

            actualStart,

            actualEnd,

            playlistIndex: playlistIndex >= 0 ? playlistIndex : undefined,

            source: 'playout',

        });



        // Durable file log — survives crashes, exportable

        void logPlayoutEvent({

            type: 'play_end',

            channelId: multiChannelState.activeChannelId,

            mediaId: currentMedia.mediaId,

            mediaTitle: currentMedia.media?.title,

            source: currentMedia.startTime ? 'scheduled' : 'fill',

            durationMs: actualEnd - actualStart,

            completed,

            scheduledStart: currentMedia.startTime,

            actualStart,

            actualEnd,

            hourSlot: loadedSlotId || currentHourSlotId || undefined,

            itemId: currentMedia.id,

            playlistIndex: playlistIndex >= 0 ? playlistIndex : undefined,

        });



        setPlayStartTime(null);

    }, [playStartTime, currentMedia, playlist, loadedSlotId, currentHourSlotId, multiChannelState.activeChannelId]);



    // Listen for merge-hours events from WeeklyScheduler

    useEffect(() => {

        const handler = async (e: any) => {

            const { slotIds } = e.detail as { slotIds: string[] };

            if (!slotIds || slotIds.length === 0) return;

            

            // Always load latest schedule

            const schedule = await loadWeeklySchedule();

            const slots = slotIds

                .map(id => schedule.slots[id])

                .filter((s): s is HourSlot => !!s);



            if (slots.length === 0) {

                persistentLogger.warn('[Merge Hours] No valid slots found for: ' + slotIds.join(','), undefined, 'PlayoutController');

                return;

            }



            // Merge the slots into a single playlist

            const merged = mergeHourSlots(slots);

            if (merged.length === 0) {

                persistentLogger.warn('[Merge Hours] No items in merged slots', undefined, 'PlayoutController');

                return;

            }



            persistentLogger.debug(`[Merge Hours] Merged ${slots.length} slots into ${merged.length} items`, undefined, 'PlayoutController');

            setPlaylist(merged);

            setLoadedSlotId(slotIds.join(','));

            snapScheduleStartToHour();

            setActiveItemId(merged[0].id);

            setInitialSeekTime(null);

        };



        window.addEventListener('merge-hours', handler as EventListener);

        return () => window.removeEventListener('merge-hours', handler as EventListener);

    }, [mergeHourSlots]);



    // Listen for side panel open events

    useEffect(() => {

        const handler = (e: any) => {

            const { panel } = e.detail as { panel: SidePanel };

            if (panel) {

                setSidePanel(panel);

                setViewMode('playout'); // Switch to playout view to see the side panel

            }

        };

        window.addEventListener('open-side-panel', handler as EventListener);

        return () => window.removeEventListener('open-side-panel', handler as EventListener);

    }, []);



    // Snap schedule start to top of current hour

    const snapScheduleStartToHour = () => {

        const now = new Date();

        const hourStart = new Date(now);

        hourStart.setMinutes(0, 0, 0);

        setScheduleStartTime(hourStart.getTime());

    };



    // Helper: given now, what item SHOULD be on air and at what offset

    const getExpectedPosition = useCallback((nowMs: number) => {

        if (calculatedPlaylist.length === 0) return null;



        const active = calculatedPlaylist.find(

            item => item.startTime && item.endTime && nowMs >= item.startTime && nowMs < item.endTime

        );

        if (!active) return null;



        const offsetSeconds = (nowMs - active.startTime!) / 1000;

        return { item: active, offsetSeconds };

    }, [calculatedPlaylist]);



    // Debug logging

    useEffect(() => {

        persistentLogger.debug('PlayoutController state', {

            playlistLength: playlist.length,

            calculatedPlaylistLength: calculatedPlaylist.length,

            activeItemId,

            activeIndex,

            currentMedia: currentMedia ? {

                id: currentMedia.id,

                title: currentMedia.media.title,

                src: currentMedia.media.src,

            } : null,

            isPlaying,

            isArmed,

            isStrictMode,

        }, 'PlayoutController');

    }, [playlist.length, activeItemId, isPlaying, isArmed, isStrictMode]);



    // Record play events

    useEffect(() => {

        if (isPlaying && currentMedia && playStartTime === null) {

            const startedAt = Date.now();

            setPlayStartTime(startedAt);

            // Log START to durable file log so a crash mid-clip is still recorded

            void logPlayoutEvent({

                type: 'play_start',

                t: startedAt,

                channelId: multiChannelState.activeChannelId,

                mediaId: currentMedia.mediaId,

                mediaTitle: currentMedia.media?.title,

                source: currentMedia.startTime ? 'scheduled' : 'fill',

                scheduledStart: currentMedia.startTime,

                actualStart: startedAt,

                hourSlot: loadedSlotId || currentHourSlotId || undefined,

                itemId: currentMedia.id,

                playlistIndex: playlist.findIndex(i => i.id === currentMedia.id),

            });

        }

        if (!isPlaying && playStartTime && currentMedia) {

            logAsPlayed(false);

        }

    }, [isPlaying, currentMedia, playStartTime, logAsPlayed, loadedSlotId, currentHourSlotId, playlist, multiChannelState.activeChannelId]);



    // Execute OBS commands once per item start. Status updates must not re-run them.

    useEffect(() => {

        if (!activeItemId || !currentMedia) return;

        const startKey = `${activeItemId}|${currentMedia.id}`;



        const executeCommands = async () => {

            if (obsStartKeyRef.current === startKey) return; // Already ran for this item start

            if (!currentMedia.obsCommands || currentMedia.obsCommands.length === 0) return;

            if (obsClientsRef.current.size === 0) return; // No OBS clients available

            obsStartKeyRef.current = startKey;



            try {

                await executeOBSCommandsForItem(currentMedia, 'start', resolveObsWsClient);

                persistentLogger.debug(`[OBS] Executed start commands for item: ${currentMedia.media?.title}`, undefined, 'PlayoutController');

                void appendEventSummary({

                    channel: multiChannelState.activeChannelId,

                    event: 'obs_start_cmds',

                    id: currentMedia.id,

                });

            } catch (error) {

                logError('[OBS] Failed to execute commands', error);

            }

        };



        // Small delay to ensure item is fully loaded

        const timeout = setTimeout(executeCommands, 100);

        return () => clearTimeout(timeout);

    }, [activeItemId, currentMedia, loadedSlotId, resolveObsWsClient, multiChannelState.activeChannelId]);



    // Execute special automation items fail-safe

    useEffect(() => {

        if (!currentMedia) return;

        const title = (currentMedia.media?.title || '').toUpperCase();

        const run = async () => {

            try {

                if (title.includes('[ATEM]')) {

                    const result = await executeAtemCommandForItem(currentMedia);

                    if (!result.success) showToast(result.message, 'warning');

                } else if (title.includes('[OBS]')) {

                    if (obsClientsRef.current.size === 0) {

                        showToast('OBS item reached but no OBS sessions are configured.', 'warning');

                    } else if (!resolveObsWsClient(null)) {

                        showToast('OBS item reached but no OBS WebSocket is connected.', 'warning');

                    } else {

                        await executeOBSCommandsForItem(currentMedia, 'start', resolveObsWsClient);

                    }

                } else if (title.includes('[MANUAL GAP]')) {

                    setIsPlaying(false);

                    const proceed = window.confirm('Manual gap reached. Continue to next item?');

                    if (proceed) handleMediaEnded();

                }

            } catch (error) {

                logError('[Automation] Special item execution failed', error);

                showToast('Automation item failed; playout continues fail-safe.', 'warning');

            }

        };

        void run();

    }, [currentMedia?.id, resolveObsWsClient, showToast]);



    // Announcement policy runtime triggers

    useEffect(() => {

        let timer: number | null = null;

        const tick = async () => {

            const settings = loadSettings() as any;

            if (!settings.ttsAnnouncementsEnabled) return;

            const now = new Date();

            if (settings.ttsAnnounceHourlyTime && now.getMinutes() === 0 && now.getSeconds() < 2) {

                const audio = await generateTTSAudio(`It is now ${now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}.`);

                await audio?.play().catch(() => null);

            }

            if (settings.ttsWarnNoContent3Min && calculatedPlaylist.length === 0 && now.getMinutes() >= 57) {

                const audio = await generateTTSAudio('Warning. No content is scheduled for the upcoming hour.');

                await audio?.play().catch(() => null);

            }

        };

        timer = window.setInterval(() => { void tick(); }, 1000);

        return () => {

            if (timer) window.clearInterval(timer);

        };

    }, [calculatedPlaylist.length]);



    // Trigger CG on start when item starts: hide previous item's cgOnStart, show current item's cgOnStart

    useEffect(() => {

        if (!currentMedia) return;

        const cgOnStart = currentMedia.cgOnStart || [];

        lastCgOnStartIdsRef.current.forEach(id => cgController.hideElement(id));

        cgOnStart.forEach(id => cgController.showElement(id));

        lastCgOnStartIdsRef.current = cgOnStart;

    }, [currentMedia?.id, currentMedia?.cgOnStart]);



    // Function to send current state to output window

    const sendStateToOutput = useCallback(() => {

        const livePlaybackTime = currentPlaybackTimeRef.current;

        const state = {

            currentMedia,

            nextMedia,

            isPlaying,

            cgElements: cgController.elements,

            cgLayers,

            currentTime: livePlaybackTime, // Use live ref, not throttled UI state

            seekTime: currentMedia?.timeIn || 0, // Include timeIn if any

            timestamp: Date.now(),

        };

        persistentLogger.debug('Sending state to output', {

            hasCurrentMedia: !!currentMedia,

            title: currentMedia?.media?.title,

            src: currentMedia?.media?.src,

            isPlaying,

            currentTime: livePlaybackTime,

        }, 'PlayoutController');

        if ((window as any).electronAPI?.syncPlayoutState) {

            (window as any).electronAPI.syncPlayoutState(state);

        }

    }, [currentMedia, nextMedia, isPlaying, cgController.elements, cgLayers]);



    // Listen for output window requesting state sync

    useEffect(() => {

        if (isElectron && (window as any).electronAPI?.onRequestStateSync) {

            const unsubscribe = (window as any).electronAPI.onRequestStateSync(() => {

                persistentLogger.debug('Output window requested state sync', undefined, 'PlayoutController');

                sendStateToOutput();

            });

            return () => unsubscribe();

        }

    }, [sendStateToOutput]);



    // Broadcast sync - use Electron IPC, BroadcastChannel, AND localStorage (for OBS browser source)

    useEffect(() => {

        persistentLogger.debug('Syncing playout state', {

            hasCurrentMedia: !!currentMedia,

            currentMediaTitle: currentMedia?.media?.title,

            currentMediaSrc: currentMedia?.media?.src,

            currentMediaId: currentMedia?.id,

            hasMedia: !!currentMedia?.media,

            mediaType: currentMedia?.media?.type,

            isPlaying,

            hasElectronAPI: !!(window as any).electronAPI,

        }, 'PlayoutController');



        // Always write to HTTP API for OBS/external browser sync

        const outputState = {

            currentMedia: preparePlaylistItemMediaHttp(currentMedia),

            nextMedia: preparePlaylistItemMediaHttp(nextMedia),

            isPlaying,

            cgElements: cgController.elements,

            cgLayers,

            currentTime: currentPlaybackTimeRef.current,

            seekTime: currentMedia?.timeIn || 0,

            timestamp: Date.now(), // For change detection

        };



        const port = getEnvironment().fileServerPort;

        const apiBase = `http://localhost:${port}`;

        const postState = (pathSuffix: string, body: object) => {

            const c = new AbortController();

            const t = setTimeout(() => c.abort(), 2000);

            fetch(`${apiBase}${pathSuffix}`, {

                method: 'POST',

                headers: { 'Content-Type': 'application/json' },

                body: JSON.stringify(body),

                signal: c.signal,

            }).catch(() => { /* server may be down — heartbeat will retry */ })

                .finally(() => clearTimeout(t));

        };



        postState('/api/state', outputState);

        for (const ch of multiChannelState.channels) {

            const body = ch.id === multiChannelState.activeChannelId

                ? outputState

                : buildBrowserOutputStateForChannel(

                    channelSnapshotsRef.current[ch.id],

                    channelPlaybackSyncRef.current[ch.id],

                    cgController.elements,

                    cgLayers,

                );

            postState(`/api/state/${encodeURIComponent(ch.id)}`, body);

        }



        if (isElectron) {

            if (!(window as any).electronAPI) {

                logError('electronAPI not available for state sync!');

                return;

            }



            // Send state to output window

            sendStateToOutput();



            // Listen for media-ended from outputs. Payload may contain a

            // channelId — when an OBS browser source on /output/<channelId>

            // ends a clip, advance THAT channel (snapshot or active state).

            const unsubscribe = (window as any).electronAPI.onOutputMediaEnded((payload: { channelId?: string } | undefined) => {

                const endedChannelId = payload?.channelId;

                if (!endedChannelId || endedChannelId === multiChannelState.activeChannelId) {

                    // Active channel — use the existing handler that operates

                    // on top-level state.

                    handleMediaEnded();

                    return;

                }

                // Inactive channel: advance its snapshot directly.

                const snap = channelSnapshotsRef.current[endedChannelId];

                if (!snap || !snap.activeItemId || snap.playlist.length === 0) return;

                const idx = snap.playlist.findIndex(p => p.id === snap.activeItemId);

                if (idx < 0) return;

                const nextItem = snap.playlist[idx + 1];

                channelSnapshotsRef.current[endedChannelId] = {

                    ...snap,

                    activeItemId: nextItem ? nextItem.id : null,

                    isPlaying: !!nextItem,

                    playStartTime: nextItem ? Date.now() : null,

                    initialSeekTime: null,

                };

                delete channelPlaybackSyncRef.current[endedChannelId];

            });

            return () => unsubscribe();

        } else {

            // BroadcastChannel for same-browser tabs

            const channel = new BroadcastChannel('playout_sync');

            channel.postMessage({ type: 'UPDATE_STATE', payload: outputState });

            channel.onmessage = (event) => {

                if (event.data.type === 'MEDIA_ENDED') {

                    handleMediaEnded();

                }

            };

            return () => channel.close();

        }

    }, [currentMedia, nextMedia, isPlaying, cgController.elements, cgLayers, sendStateToOutput, ddrChannels, multiChannelState.channels, multiChannelState.activeChannelId]);



    // Heartbeat — active rail `currentTime` advances every tick; **inactive** rails must also POST

    // so OBS `/output2` `/output3` keep lockstep timestamps (idle body still posts frozen playhead).

    useEffect(() => {

        const sendHeartbeat = () => {

            const livePlaybackTime = currentPlaybackTimeRef.current;

            const heartbeatState = {

                currentMedia: preparePlaylistItemMediaHttp(currentMedia),

                nextMedia: preparePlaylistItemMediaHttp(nextMedia),

                isPlaying: !!(isPlaying && currentMedia),

                cgElements: cgController.elements,

                cgLayers,

                currentTime: livePlaybackTime,

                seekTime: currentMedia?.timeIn || 0,

                timestamp: Date.now(),

            };



            if (isElectron && (window as any).electronAPI?.syncPlayoutState) {

                (window as any).electronAPI.syncPlayoutState(heartbeatState);

            }



            const port = getEnvironment().fileServerPort;

            const apiBase = `http://localhost:${port}`;

            const post = (pathSuffix: string, body: object) => {

                const controller = new AbortController();

                const timer = setTimeout(() => controller.abort(), 2000);

                fetch(`${apiBase}${pathSuffix}`, {

                    method: 'POST',

                    headers: { 'Content-Type': 'application/json' },

                    body: JSON.stringify(body),

                    signal: controller.signal,

                }).catch(() => { /* server may be down — fine */ })

                    .finally(() => clearTimeout(timer));

            };

            post('/api/state', heartbeatState);



            const activeId = multiChannelState.activeChannelId;

            for (const ch of multiChannelState.channels) {

                const body =

                    ch.id === activeId

                        ? heartbeatState

                        : buildBrowserOutputStateForChannel(

                            channelSnapshotsRef.current[ch.id],

                            channelPlaybackSyncRef.current[ch.id],

                            cgController.elements,

                            cgLayers,

                        );

                post(`/api/state/${encodeURIComponent(ch.id)}`, body);

            }

        };



        const interval = window.setInterval(sendHeartbeat, 500);

        sendHeartbeat();

        return () => window.clearInterval(interval);

    }, [isPlaying, currentMedia, nextMedia, cgLayers, cgController.elements, isElectron, multiChannelState.channels, multiChannelState.activeChannelId]);



    // --- HANDLERS ---



    const handleAddMedia = (media: MediaItem, options?: MediaLibraryItemClickOptions) => {

        const smartConfig = options && isSmartSlotConfig(options) ? options : undefined;

        let resolvedMedia = media;

        let isResolved = true;



        if (smartConfig) {

            const found = resolveSmartSlot(smartConfig);

            if (found) {

                resolvedMedia = found;

            } else {

                resolvedMedia = { ...media, title: `${media.title} (UNRESOLVED)` };

                isResolved = false;

            }

        }



        // If this media has segments, add one playlist item per segment (time-in/out)

        const activeSegs = resolvedMedia.segmentCollections?.find(c => c.isActive)?.segments

            ?? resolvedMedia.segmentCollections?.[0]?.segments

            ?? resolvedMedia.segments;

        const segs = activeSegs && activeSegs.length > 0 ? activeSegs : [];

        const itemsToAdd: PlaylistItem[] = segs.length > 0

            ? segs

                .filter(s => (s.endTime ?? 0) > (s.startTime ?? 0))

                .map((s, idx) => ({

                    id: generateId(),

                    mediaId: resolvedMedia.id,

                    parentMediaId: resolvedMedia.id,

                    segmentId: s.id,

                    segmentLabel: s.label || `Segment ${idx + 1}`,

                    media: {

                        ...resolvedMedia,

                        title: `${resolvedMedia.title} — ${s.label || `Segment ${idx + 1}`}`,

                        notes: [resolvedMedia.notes, `Segment from ${resolvedMedia.filename || resolvedMedia.title}`].filter(Boolean).join('\n'),

                    },

                    timeIn: s.startTime,

                    timeOut: s.endTime,

                    smartConfig,

                    isResolved,

                }))

            : [{

                id: generateId(),

                mediaId: resolvedMedia.id,

                media: resolvedMedia,

                smartConfig,

                isResolved,

            }];



        setPlaylist(prev => [...prev, ...itemsToAdd]);

        if (!activeItemId && playlist.length === 0 && itemsToAdd.length > 0) {

            setActiveItemId(itemsToAdd[0].id);

        }

    };



    const handleFocusAnalytics = useCallback((payload?: Record<string, unknown>) => {

        setAnalyticsFocus(payload || null);

        setViewMode('analytics');

    }, []);



    const updateMediaAvailability = useCallback(async (media: MediaItem, key: 'availableStart' | 'availableEnd') => {

        const current = media[key] || '';

        const next = window.prompt(

            key === 'availableStart'

                ? 'Set valid start (ISO or local date-time). Leave blank to clear.'

                : 'Set expire time (ISO or local date-time). Leave blank to clear.',

            current,

        );

        if (next === null) return;

        const value = next.trim();

        const parsed = value ? new Date(value) : null;

        if (value && Number.isNaN(parsed?.getTime())) {

            showToast('Invalid date/time. Use ISO or local date-time format.', 'error');

            return;

        }

        const updated: MediaItem = {

            ...media,

            [key]: value ? parsed!.toISOString() : undefined,

        };

        await dbSaveMedia(updated);

        showToast(`${media.title}: ${key === 'availableStart' ? 'start' : 'expire'} updated`, 'success');

    }, [showToast]);



    const runAdCadenceWizard = useCallback(async (media: MediaItem) => {

        const cadenceInput = window.prompt('Try to play every how many minutes?', '15');

        if (!cadenceInput) return;

        const cadenceMinutes = Number(cadenceInput);

        if (!Number.isFinite(cadenceMinutes) || cadenceMinutes <= 0) {

            showToast('Invalid cadence minutes.', 'error');

            return;

        }

        const startInput = window.prompt('Window start time (HH:MM, 24h)', '00:00');

        const endInput = window.prompt('Window end time (HH:MM, 24h)', '23:59');

        if (!startInput || !endInput) return;

        const toMinutes = (v: string) => {

            const [h, m] = v.split(':').map(Number);

            if (!Number.isFinite(h) || !Number.isFinite(m)) return NaN;

            return h * 60 + m;

        };

        const winStart = toMinutes(startInput);

        const winEnd = toMinutes(endInput);

        if (!Number.isFinite(winStart) || !Number.isFinite(winEnd) || winEnd <= winStart) {

            showToast('Invalid ad wizard window.', 'error');

            return;

        }



        const targets: number[] = [];

        for (let t = winStart; t <= winEnd; t += cadenceMinutes) targets.push(t);

        const conflicts: number[] = [];

        const additions: PlaylistItem[] = [];

        for (const targetMinute of targets) {

            const secOfHour = (targetMinute % 60) * 60;

            const hasConflict = playlist.some((p) => {

                const s = p.startTime != null ? (Math.floor((p.startTime - scheduleStartTime) / 1000) % 3600 + 3600) % 3600 : null;

                const e = s != null ? s + (p.media.duration || 0) : null;

                return s != null && e != null && secOfHour >= s && secOfHour < e;

            });

            if (hasConflict) {

                conflicts.push(targetMinute);

                continue;

            }

            additions.push({

                id: generateId(),

                mediaId: media.id,

                media,

                isFixedTime: true,

                fixedTime: secOfHour,

            });

        }



        let mode: 'skip' | 'force_all' | 'sequential' = 'skip';

        if (conflicts.length) {

            const modeRaw = window.prompt(

                `Conflicts: ${conflicts.length}. Choose mode: skip | force_all | sequential`,

                'skip',

            ) || 'skip';

            mode = modeRaw === 'force_all' || modeRaw === 'sequential' ? modeRaw : 'skip';

        }

        if (mode === 'force_all') {

            conflicts.forEach((targetMinute) => additions.push({

                id: generateId(),

                mediaId: media.id,

                media,

                isFixedTime: true,

                fixedTime: (targetMinute % 60) * 60,

            }));

        } else if (mode === 'sequential') {

            for (const targetMinute of conflicts) {

                const include = window.confirm(`Force placement at ${String(Math.floor(targetMinute / 60)).padStart(2, '0')}:${String(targetMinute % 60).padStart(2, '0')}?`);

                if (include) {

                    additions.push({

                        id: generateId(),

                        mediaId: media.id,

                        media,

                        isFixedTime: true,

                        fixedTime: (targetMinute % 60) * 60,

                    });

                }

            }

        }

        if (!additions.length) {

            showToast('Ad wizard produced no placements.', 'warning');

            return;

        }

        const updated = [...playlist, ...additions].sort((a, b) => (a.fixedTime ?? Number.MAX_SAFE_INTEGER) - (b.fixedTime ?? Number.MAX_SAFE_INTEGER));

        setPlaylist(updated);

        showToast(`Ad wizard applied ${additions.length} placements (${conflicts.length} conflicts).`, 'success');

    }, [playlist, scheduleStartTime, setPlaylist, showToast]);



    const generateAsRunReportForMedia = useCallback(async (media: MediaItem) => {

        const asPlayedEntries = loadAsPlayedLog()

            .filter((entry: AsPlayedEntry) => entry.mediaId === media.id)

            .map((entry): AsRunDisplayEntry => ({

                source: 'as-played',

                actualStart: entry.actualStart || entry.timestamp,

                actualEnd: entry.actualEnd || ((entry.actualStart || entry.timestamp) + (entry.duration || 0) * 1000),

                duration: entry.duration || 0,

                completed: Boolean(entry.completed),

                hourSlot: entry.hourSlot,

                itemId: entry.itemId,

            }));



        let controlServiceEntries: AsRunDisplayEntry[] = [];

        const controlGetAsRunLog = (window as any).electronAPI?.controlGetAsRunLog;

        if (typeof controlGetAsRunLog === 'function') {

            try {

                const rows = await controlGetAsRunLog({

                    startTime: Date.now() - (90 * 24 * 60 * 60 * 1000),

                });

                if (Array.isArray(rows)) {

                    controlServiceEntries = rows

                        .filter((row: any) => {

                            const metadata = row?.metadata || {};

                            return row?.assetId === media.id || metadata?.mediaId === media.id || metadata?.assetId === media.id;

                        })

                        .map((row: any): AsRunDisplayEntry => ({

                            source: 'control-service',

                            actualStart: Number(row?.actualStart || 0),

                            actualEnd: Number(row?.actualStart || 0),

                            duration: 0,

                            completed: row?.status !== 'FAILED' && row?.status !== 'CANCELLED',

                            status: row?.status,

                            reason: row?.reason,

                            itemId: row?.eventId,

                        }));

                }

            } catch (error) {

                persistentLogger.warn('[AsRunReport] controlGetAsRunLog failed', error, 'PlayoutController');

            }

        }



        const combined = [...asPlayedEntries, ...controlServiceEntries]

            .sort((a, b) => b.actualStart - a.actualStart);



        setAsRunReport({

            media,

            generatedAt: Date.now(),

            entries: combined,

        });



        showToast(

            combined.length > 0

                ? `As-run report ready: ${combined.length} entries for "${media.title}".`

                : `No as-run entries found for "${media.title}".`,

            combined.length > 0 ? 'success' : 'warning',

        );

    }, [showToast]);



    const exportAsRunReportCsv = useCallback(() => {

        if (!asRunReport) return;

        const header = ['Source', 'Actual Start', 'Actual End', 'Duration Seconds', 'Status', 'Slot/Item/Reason'];

        const rows = asRunReport.entries.map((entry) => [

            entry.source,

            entry.actualStart ? new Date(entry.actualStart).toISOString() : '',

            entry.actualEnd ? new Date(entry.actualEnd).toISOString() : '',

            String(entry.duration || 0),

            entry.status || (entry.completed ? 'COMPLETED' : 'PARTIAL'),

            entry.hourSlot || entry.itemId || entry.reason || '',

        ]);

        const csv = [header, ...rows]

            .map((row) => row.map((cell) => `"${String(cell).replace(/"/g, '""')}"`).join(','))

            .join('\n');

        const blob = new Blob([csv], { type: 'text/csv;charset=utf-8;' });

        const url = URL.createObjectURL(blob);

        const a = document.createElement('a');

        a.href = url;

        a.download = `asrun_${asRunReport.media.id}_${new Date(asRunReport.generatedAt).toISOString().replace(/[:.]/g, '-')}.csv`;

        a.click();

        URL.revokeObjectURL(url);

    }, [asRunReport]);



    const buildAsRunReportHtml = useCallback(() => {

        if (!asRunReport) return '';

        const rowsHtml = asRunReport.entries.map((entry) => `

            <tr>

                <td>${entry.source}</td>

                <td>${entry.actualStart ? new Date(entry.actualStart).toLocaleString() : '-'}</td>

                <td>${entry.actualEnd ? new Date(entry.actualEnd).toLocaleString() : '-'}</td>

                <td style="text-align:right">${entry.duration ? `${entry.duration}s` : '-'}</td>

                <td>${entry.status || (entry.completed ? 'COMPLETED' : 'PARTIAL')}</td>

                <td>${entry.hourSlot || entry.itemId || entry.reason || '-'}</td>

            </tr>

        `).join('');

        return `<!doctype html>

<html>

<head>

  <meta charset="utf-8" />

  <title>As-Run Report - ${asRunReport.media.title}</title>

  <style>

    body{font-family:Arial,sans-serif;padding:20px;color:#111}

    h1{font-size:20px;margin:0 0 8px 0}

    .meta{font-size:12px;color:#555;margin-bottom:16px}

    table{width:100%;border-collapse:collapse;font-size:12px}

    th,td{border:1px solid #ddd;padding:8px;text-align:left;vertical-align:top}

    th{background:#f3f4f6}

  </style>

</head>

<body>

  <h1>As-Run Report</h1>

  <div class="meta">

    <div><strong>Media:</strong> ${asRunReport.media.title}</div>

    <div><strong>Generated:</strong> ${new Date(asRunReport.generatedAt).toLocaleString()}</div>

    <div><strong>Entries:</strong> ${asRunReport.entries.length}</div>

  </div>

  <table>

    <thead>

      <tr>

        <th>Source</th><th>Actual Start</th><th>Actual End</th><th>Duration</th><th>Status</th><th>Slot / Item</th>

      </tr>

    </thead>

    <tbody>${rowsHtml || '<tr><td colspan="6">No as-run entries found.</td></tr>'}</tbody>

  </table>

</body>

</html>`;

    }, [asRunReport]);



    const exportAsRunReportHtml = useCallback(() => {

        const html = buildAsRunReportHtml();

        if (!html || !asRunReport) return;

        const blob = new Blob([html], { type: 'text/html;charset=utf-8;' });

        const url = URL.createObjectURL(blob);

        const a = document.createElement('a');

        a.href = url;

        a.download = `asrun_${asRunReport.media.id}_${new Date(asRunReport.generatedAt).toISOString().replace(/[:.]/g, '-')}.html`;

        a.click();

        URL.revokeObjectURL(url);

    }, [asRunReport, buildAsRunReportHtml]);



    const printAsRunReportPdf = useCallback(() => {

        const html = buildAsRunReportHtml();

        if (!html) return;

        const reportWindow = window.open('', '_blank', 'noopener,noreferrer,width=1000,height=700');

        if (!reportWindow) return;

        reportWindow.document.write(html);

        reportWindow.document.close();

        reportWindow.focus();

        window.setTimeout(() => {

            reportWindow.print();

        }, 250);

    }, [buildAsRunReportHtml]);



    const handleSharedContextMenuAction = useCallback(async (actionId: string, payload?: Record<string, unknown>) => {

        const media = payload?.media as MediaItem | undefined;

        const item = payload?.item as PlaylistItem | undefined;



        await executeContextMenuAction(actionId as any, payload, {

            onOpenPreview: async () => {

                if (!media) return;

                const resolvedUrl = await resolveMediaUrl(media).catch(() => media.src || '');

                if (resolvedUrl) {

                    window.open(resolvedUrl, '_blank', 'noopener');

                    return;

                }

                showToast('No preview source is available for this item.', 'warning');

            },

            onAddToPlaylist: async () => {

                if (media) {

                    handleAddMedia(media);

                }

            },

            onSendToDDR: async (nextPayload) => {

                if (media && nextPayload?.channel) {

                    await handleSendToDDR(nextPayload.channel as DDRChannel, media);

                }

            },

            onTriggerCG: async (nextPayload) => {

                const cgElementId = String(nextPayload?.cgElementId || '');

                if (cgElementId) {

                    cgController.toggleElement(cgElementId);

                }

            },

            onEditSegments: async () => {

                if (media) {

                    setEditingSegmentMedia(media);

                }

            },

            onAttachOBSCommand: async (nextPayload) => {

                const obsCommandId = String(nextPayload?.obsCommandId || '');

                if (!item || !obsCommandId) return;

                const currentCommands = item.obsCommands || [];

                if (currentCommands.includes(obsCommandId)) return;

                setPlaylist((prev) => prev.map((playlistItem) => (

                    playlistItem.id === item.id

                        ? { ...playlistItem, obsCommands: [...currentCommands, obsCommandId] }

                        : playlistItem

                )));

            },

            onDelete: async () => {

                if (item) {

                    handleRemoveItem(item.id);

                }

            },

            onViewAnalytics: async (nextPayload) => {

                handleFocusAnalytics(nextPayload);

            },

            onSetMediaStart: async () => {

                if (media) {

                    await updateMediaAvailability(media, 'availableStart');

                }

            },

            onSetMediaExpire: async () => {

                if (media) {

                    await updateMediaAvailability(media, 'availableEnd');

                }

            },

            onOpenAdCadenceWizard: async () => {

                if (media?.type === 'ad') {

                    await runAdCadenceWizard(media);

                } else {

                    showToast('Ad cadence wizard is available for ad items only.', 'warning');

                }

            },

            onGenerateAsRunReport: async () => {

                if (!media) return;

                await generateAsRunReportForMedia(media);

            },

            onMediaPriorityIncrease: async () => {

                if (!media) return;

                const next = bumpLibraryPriorityUp(media);

                await dbSaveMedia(next);

                window.dispatchEvent(new CustomEvent('media-library-update'));

                showToast('Library priority increased.', 'success');

            },

            onMediaPriorityDecrease: async () => {

                if (!media) return;

                const next = bumpLibraryPriorityDown(media);

                await dbSaveMedia(next);

                window.dispatchEvent(new CustomEvent('media-library-update'));

                showToast('Library priority reduced.', 'success');

            },

            onMediaPriorityFreezeToggle: async () => {

                if (!media) return;

                const next = toggleLibraryPriorityFreeze(media);

                await dbSaveMedia(next);

                window.dispatchEvent(new CustomEvent('media-library-update'));

                showToast(

                    next.priorityFrozen ? 'Priority frozen — decay paused for this item.' : 'Priority unfrozen — decay active.',

                    'info',

                );

            },

        });

    }, [cgController, handleAddMedia, handleFocusAnalytics, setPlaylist, showToast, updateMediaAvailability, runAdCadenceWizard, generateAsRunReportForMedia]);



    const handleMediaContextMenu = (media: MediaItem, x: number, y: number) => {

        // Create a mock mouse event for the menu

        const mockEvent = {

            clientX: x,

            clientY: y,

            preventDefault: () => {},

            stopPropagation: () => {},

        } as React.MouseEvent;

        showMenu(mockEvent, buildMediaMenuItems(

            {

                targetId: media.id,

                targetType: 'media',

                mediaId: media.id,

                media,

            },

            {

                ddrChannels: ['A', 'B', 'C', 'D'],

                cgElements: cgController.elements.map((element) => ({ id: element.id, name: element.name })),

            },

        ));

    };



    const handlePlaylistItemContextMenu = (e: React.MouseEvent, item: PlaylistItem) => {

        showMenu(e, buildPlaylistMenuItems(

            {

                targetId: item.id,

                targetType: 'playlist',

                playlistItemId: item.id,

                item,

                media: item.media,

            },

            {

                ddrChannels: ['A', 'B', 'C', 'D'],

                obsCommands: loadOBSCommands() as OBSCommand[],

            },

        ));

    };



    const handleSendToDDR = async (channel: DDRChannel, media: MediaItem) => {

        // Resolve playable URL so DDR video element can load (filePath -> blob/http URL in Electron)

        let resolvedMedia = media;

        const playableSrc = await resolveMediaUrl(media).catch(() => '');

        if (playableSrc) {

            resolvedMedia = { ...media, src: playableSrc };

        }

        const item: PlaylistItem = {

            id: generateId(),

            mediaId: resolvedMedia.id,

            media: resolvedMedia,

            isResolved: true,

        };

        setDdrChannels(prev => ({

            ...prev,

            [channel]: {

                ...prev[channel],

                items: [...prev[channel].items, item],

            },

        }));

    };



    const mapStarfileTypeToMediaType = (item: StarfileFileItem): MediaType => {

        if (item.type === 'image') return 'image';

        if (item.type === 'video') return 'video';

        return 'program';

    };



    const createMediaFromStarfileItem = useCallback((item: StarfileFileItem): MediaItem => ({

        id: item.analyticsKey || item.id,

        title: item.name,

        duration: 0,

        src: item.shareUrl || item.previewUrl || item.path,

        type: mapStarfileTypeToMediaType(item),

        filePath: item.path,

        filename: item.name,

        addedAt: item.modifiedDate || new Date().toISOString(),

        thumbnail: item.type === 'image' ? (item.previewUrl || item.shareUrl || item.path) : undefined,

        notes: `Imported from Starfile2: ${item.path}`,

    }), []);



    const handleImportStarfileItem = useCallback(async (item: StarfileFileItem) => {

        const mediaItem = createMediaFromStarfileItem(item);

        await dbSaveMedia(mediaItem);

        await dbLogAnalyticsEvent({

            family: 'filehub',

            action: 'import_to_library',

            entityType: 'file',

            entityId: item.path,

            entityLabel: item.name,

            payload: {

                analyticsKey: item.analyticsKey,

            },

        });

    }, [createMediaFromStarfileItem]);



    const handleSendStarfileItemToPlaylist = useCallback(async (item: StarfileFileItem) => {

        handleAddMedia(createMediaFromStarfileItem(item));

    }, [createMediaFromStarfileItem, handleAddMedia]);



    const pickWeighted = <T,>(items: T[], weight: (t: T) => number): T | null => {

        const weights = items.map(weight).map(w => Math.max(0, w));

        const total = weights.reduce((a, b) => a + b, 0);

        if (total <= 0) return null;

        let r = Math.random() * total;

        for (let i = 0; i < items.length; i++) {

            r -= weights[i];

            if (r <= 0) return items[i];

        }

        return items[items.length - 1] || null;

    };



    const handleQuickBreak = async () => {

        // Build ~6-8 minute ad break using weighted selection (priority + light recency penalty)

        const target = 360 + Math.floor(Math.random() * 121); // 360..480 sec

        const [dbMedia, localMedia, libMedia] = await Promise.all([

            dbFetchMedia().catch(() => []),

            Promise.resolve(loadLocalMedia()),

            Promise.resolve(loadLibraryMedia([])),

        ]);

        const pool = [...dbMedia, ...localMedia, ...libMedia]

            .filter(m => m && m.type === 'ad' && !m.archived && (m.duration || 0) > 0);



        if (pool.length === 0) {

            alert('No ads found in library.');

            return;

        }



        const chosen: MediaItem[] = [];

        let total = 0;

        let guard = 0;

        while (total < target && guard++ < 400) {

            const pick = pickWeighted(pool, (m) => {

                const pr = typeof m.priority === 'number' ? m.priority : 0;

                const base = Math.max(1, pr + 10); // -10..10 -> 0..20

                const plays = m.playCount || 0;

                const recencyPenalty = 1 / (1 + plays * 0.15);

                return base * recencyPenalty;

            });

            if (!pick) break;

            chosen.push(pick);

            total += pick.duration;

            if (total >= 480) break;

        }



        if (chosen.length === 0) return;



        const items: PlaylistItem[] = chosen.map(m => ({

            id: generateId(),

            mediaId: m.id,

            media: m,

            isResolved: true,

        }));



        setPlaylist(prev => {

            const insertAt = activeItemId ? Math.max(0, prev.findIndex(i => i.id === activeItemId) + 1) : prev.length;

            const next = [...prev];

            next.splice(insertAt, 0, ...items);

            return next;

        });

    };



    const handleRemoveItem = (id: string) => {

        setPlaylist(playlist.filter(item => item.id !== id));

        if (activeItemId === id) {

            setIsPlaying(false);

            setActiveItemId(null);

        }

    };



    const handleSetFixedTime = (id: string, time: number | null) => {

        setPlaylist(playlist.map(item =>

            item.id === id

                ? { ...item, isFixedTime: time !== null, fixedTime: time ?? undefined }

                : item

        ));

    };



    // PSIP update handler - automatically injects EIT into broadcast mux

    const handlePSIPUpdate = useCallback((events: PSIPEvent[]) => {

        setPsipEvents(events);

        

        // Auto-update BroadcastMuxManager PSIP if it's configured

        // This will be handled by the BroadcastMuxManager component listening to playlist changes

        persistentLogger.debug(`[PSIP] Generated ${events.length} EIT events from playlist`, undefined, 'PlayoutController');

        

        // Broadcast PSIP update event for other components

        window.dispatchEvent(new CustomEvent('psip-update', { 

            detail: { events, scheduleStartTime, channelName: 'PLAY PLAY' } 

        }));

    }, [scheduleStartTime]);



    const getActiveCasparChannel = useCallback(() => {

        const idx = multiChannelState.channels.findIndex((c) => c.id === multiChannelState.activeChannelId);

        const safeIdx = idx >= 0 ? idx : 0;

        const raw = Number((multiChannelState.channels[safeIdx] as any)?.casparChannel || (safeIdx + 1));

        // Cap by multichannel ceiling (matches Caspar `<screen>` count), not visible row count —

        // with fewer sidebar rows than physical channels, playlist transport must still reach CHn.

        const mc = Math.floor(Number(multiChannelState.maxChannels));

        const ceiling = Math.max(

            1,

            Number.isFinite(mc) && mc >= 1 ? mc : multiChannelState.channels.length || 32,

            multiChannelState.channels.length,

        );

        return Math.min(ceiling, Math.max(1, Number.isFinite(raw) ? Math.floor(raw) : (safeIdx + 1)));

    }, [multiChannelState.activeChannelId, multiChannelState.channels, multiChannelState.maxChannels]);



    const playActiveChannelOnCaspar = useCallback(async (items: PlaylistItem[]) => {

        const api = (window as any).electronAPI;

        if (!api?.casparPlayPlaylist) throw new Error('CasparCG API unavailable');

        if (!items.length) return null;

        const casparChannel = getActiveCasparChannel();

        const result = await api.casparPlayPlaylist(casparChannel, items);

        if (!result?.ok) {

            let msg = result?.error || 'caspar play failed';

            if (Array.isArray(result?.recentAmcp) && result.recentAmcp.length) {

                msg += `\n\nRecent AMCP:\n${result.recentAmcp.slice(-10).join('\n')}`;

            }

            throw new Error(msg);

        }

        setCasparTransportPlaying(true);

        return result;

    }, [getActiveCasparChannel]);



    const buildCasparRunlistFromPosition = useCallback((items: PlaylistItem[], startIndex: number, seekIntoMediaSeconds: number) => {

        return buildCasparRunlistFromBossSeek(items, startIndex, seekIntoMediaSeconds);

    }, []);



    const handlePlayPause = async () => {

        const api = (window as any).electronAPI;

        const casparChannel = getActiveCasparChannel();

        const casparMode = useCasparAsPrimary && !!casparStatus.running;

        if (casparMode && api?.casparPauseChannel && api?.casparResumeChannel) {

            try {

                if (casparTransportPlaying) {

                    const r = await api.casparPauseChannel(casparChannel);

                    if (!r?.ok) throw new Error(r?.error || 'Caspar pause failed');

                    setCasparTransportPlaying(false);

                } else {

                    const r = await api.casparResumeChannel(casparChannel);

                    if (!r?.ok) throw new Error(r?.error || 'Caspar resume failed');

                    setCasparTransportPlaying(true);

                }

                return;

            } catch (error) {

                logError('[Caspar] Play/pause failed', error);

            }

        }

        if (enginePlaybackState?.isPlaying) {

            playoutEngineRef.current?.pause();

        } else {

            playoutEngineRef.current?.resume();

        }

    };



    const handleStop = async () => {

        const api = (window as any).electronAPI;

        const casparMode = useCasparAsPrimary && !!casparStatus.running;

        if (casparMode && api?.casparStopChannel) {

            try {

                const r = await api.casparStopChannel(getActiveCasparChannel());

                if (!r?.ok) throw new Error(r?.error || 'Caspar stop failed');

                setCasparTransportPlaying(false);

                return;

            } catch (error) {

                logError('[Caspar] Stop failed', error);

            }

        }

        if (playoutEngineRef.current) {

            playoutEngineRef.current.stop();

        }

    };

    const effectiveIsPlaying = useCasparAsPrimary && casparStatus.running

        ? casparTransportPlaying

        : (enginePlaybackState?.isPlaying ?? isPlaying);



    const handleNext = async () => {

        const nextIndex = activeIndex + 1;

        if (nextIndex < calculatedPlaylist.length) {

            setActiveItemId(calculatedPlaylist[nextIndex].id);

        }

    };



    const handleMediaEnded = () => {

        if (playStartTime && currentMedia) {

            logAsPlayed(true);

            // CG on end: hide item's cgOnStart, show item's cgOnEnd

            (currentMedia.cgOnStart || []).forEach(id => cgController.hideElement(id));

            (currentMedia.cgOnEnd || []).forEach(id => cgController.showElement(id));

            // Also track plays in SQL media DB (if available)

            if ((window as any).electronAPI?.mediaIncrementPlay) {

                const duration = Math.max(1, Math.floor((Date.now() - playStartTime) / 1000));

                (window as any).electronAPI.mediaIncrementPlay(currentMedia.mediaId, duration);

            }

        }



        if (nextMedia) {

            setActiveItemId(nextMedia.id);

        } else {

            setIsPlaying(false);

        }



        // Advance the ACTIVE channel's snapshot so that output windows,

        // heartbeats, and channel-switch restore stay coherent.

        const activeId = multiChannelState.activeChannelId;

        const snap = channelSnapshotsRef.current[activeId];

        if (snap) {

            const idx = snap.playlist.findIndex(p => p.id === snap.activeItemId);

            const nextItem = idx >= 0 ? snap.playlist[idx + 1] : undefined;

            channelSnapshotsRef.current[activeId] = {

                ...snap,

                activeItemId: nextItem ? nextItem.id : null,

                isPlaying: !!nextItem,

                playStartTime: nextItem ? Date.now() : null,

                initialSeekTime: null,

            };

            delete channelPlaybackSyncRef.current[activeId];

        }

    };



    const handleFullscreen = () => {

        if (playerWrapperRef.current) {

            if (!document.fullscreenElement) {

                playerWrapperRef.current.requestFullscreen().catch((e) => logError('Fullscreen failed', e));

            } else {

                document.exitFullscreen();

            }

        }

    };



    // OBS dock / external playout commands (HTTP POST → main → IPC → here)

    const playoutCommandHandlersRef = useRef({ handleNext, handlePlayPause, handleStop });

    playoutCommandHandlersRef.current = { handleNext, handlePlayPause, handleStop };

    useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.onPlayoutCommand) return;

        const unsub = api.onPlayoutCommand((data: { action?: string }) => {

            const action = (data?.action || '').toLowerCase();

            const { handleNext: next, handlePlayPause: playPause, handleStop: stop } = playoutCommandHandlersRef.current;

            if (action === 'next') next();

            else if (action === 'play') playPause();

            else if (action === 'pause') playPause();

            else if (action === 'stop') stop();

        });

        return () => { unsub?.(); };

    }, []);



    const toggleOutputWindow = safeHandler(async () => {

        if (isElectron) {

            if (isOutputWindowOpen) {

                await (window as any).electronAPI.closeOutputWindow();

                setIsOutputWindowOpen(false);

            } else {

                await (window as any).electronAPI.openOutputWindow();

                setIsOutputWindowOpen(true);

            }

        } else {

            // Browser mode - open in new window

            window.open('/#/output', 'PlayoutOutput', 'width=1280,height=720,menubar=no,toolbar=no');

        }

    }, 'PlayoutController');



    const copyOutputUrl = () => {

        if (outputWindowUrl) {

            navigator.clipboard.writeText(outputWindowUrl).then(() => {

                setShowOutputUrlTooltip(true);

                setTimeout(() => setShowOutputUrlTooltip(false), 2000);

            });

        }

    };



    const handleSnapToTopOfHour = () => {

        const now = new Date();

        const hourStart = new Date(now);

        hourStart.setMinutes(0, 0, 0);

        setScheduleStartTime(hourStart.getTime());

    };



    // Calculate which item should be playing and seek position for time-accurate playback.

    // Uses wall-clock start/end from calculateSchedule(); fills gaps and early-before-air edge cases.

    const calculateTimeAccuratePosition = useCallback((items: PlaylistItem[], hourStartTime: number) => {

        const pos = computeBossClockPlayoutPosition(items, hourStartTime, Date.now());

        if (items.length && pos.itemIndex >= 0) {

            const row = items[pos.itemIndex];

            persistentLogger.debug('Time-accurate (boss clock)', {

                itemIndex: pos.itemIndex,

                title: row?.media?.title,

                seekTime: pos.seekTime,

                isWithinHour: pos.isWithinHour,

            }, 'PlayoutController');

        }

        return pos;

    }, []);



    const handleLoadFromSchedule = async (slotId?: string, playLive: boolean = false) => {

        // Always load the latest saved schedule - force reload from storage

        // Clear any cached schedule data to ensure fresh load

        const schedule = await loadWeeklySchedule();

        let targetSlotId = slotId;



        if (!targetSlotId) {

            // Load current hour

            const now = new Date();

            const dayOfWeek = now.getDay(); // 0 = Sunday, 1 = Monday, etc.

            const hour = now.getHours();

            targetSlotId = `${dayOfWeek}-${hour}`;

        }



        persistentLogger.debug(`Attempting to load schedule slot: ${targetSlotId} playLive: ${playLive}`, undefined, 'PlayoutController');

        const slot = schedule.slots[targetSlotId];



        if (!slot) {

            logError('Slot not found in schedule: ' + targetSlotId);

            logError(`Schedule slot ${targetSlotId} not found. Save items to that hour in Weekly Schedule.`, { slotId: targetSlotId });

            return;

        }



        if (slot.items.length === 0) {

            persistentLogger.debug('Slot exists but has no items: ' + targetSlotId, undefined, 'PlayoutController');

            logError(`Schedule slot ${targetSlotId} has no items. Add items in Weekly Schedule first.`, { slotId: targetSlotId });

            return;

        }



        persistentLogger.debug(`Loading schedule slot into playout: ${targetSlotId} ${slot.items.length} items`, undefined, 'PlayoutController');



        // Calculate hour start time (top of the current hour)

        const now = new Date();

        const hourStart = new Date(now);

        hourStart.setMinutes(0, 0, 0);

        const hourStartTime = hourStart.getTime();



        // Rehydrate media from library/DB and generate new IDs

        const hydrated = await rehydrateItemsFromLibrary(slot.items);

        const items = hydrated.map(item => {

            persistentLogger.debug('Loading item: ' + item.media?.title + ' src: ' + item.media?.src + ' duration: ' + item.media?.duration, undefined, 'PlayoutController');

            return {

                ...item,

                id: generateId(),

            };

        });



        // CRITICAL: Set schedule start time FIRST before calculating positions

        safeSetState(() => {

            setScheduleStartTime(hourStartTime);

            setPlaylist(items);

            setLoadedSlotId(targetSlotId);

        });



        // Small delay to ensure state is updated before calculating position

        await new Promise(resolve => setTimeout(resolve, 50));



        if (playLive && items.length > 0) {

            // TIME-ACCURATE PLAYBACK: Calculate which item should be playing NOW

            // This ensures instant snap-to-current-time on cold start and top-of-hour

            const position = calculateTimeAccuratePosition(items, hourStartTime);



            if (position.itemId && position.isWithinHour) {

                persistentLogger.debug('[LoadSchedule] Starting time-accurate playback (snap to current time)', {

                    itemIndex: position.itemIndex,

                    itemTitle: items[position.itemIndex]?.media?.title,

                    seekTime: position.seekTime,

                    isWithinHour: position.isWithinHour,

                    hourStartTime: new Date(hourStartTime).toISOString(),

                    now: new Date().toISOString(),

                    elapsedSeconds: (Date.now() - hourStartTime) / 1000,

                }, 'PlayoutController');



                // INSTANT START: Set active item and seek time, then start playing

                safeSetState(() => {

                    setActiveItemId(position.itemId);

                    setInitialSeekTime(position.seekTime);

                    setIsPlaying(true);

                });



                // If external engine is active, play at calculated position

                const manager = playoutEngineRef.current;

                if (manager) {

                    const liveItem = items[position.itemIndex];

                    if (liveItem) {

                        try {

                            if (useCasparAsPrimary && casparStatus.running) {

                                const runlist = buildCasparRunlistFromPosition(items, position.itemIndex, position.seekTime);

                                await playActiveChannelOnCaspar(runlist);

                            } else {

                                await manager.playMediaAt(liveItem, position.seekTime);

                            }

                        } catch (error) {

                            logError('[LoadSchedule] Failed to play on external engine', error);

                        }

                    }

                }

            } else {

                // If we're past the hour or calculation failed, start from beginning

                persistentLogger.warn('[LoadSchedule] Time-accurate calculation failed or outside hour, starting from beginning', position, 'PlayoutController');

                if (items.length > 0) {

                    safeSetState(() => {

                        setActiveItemId(items[0].id);

                        setInitialSeekTime(0);

                        setIsPlaying(true);

                    });

                }

            }

        } else if (items.length > 0) {

            // Just load, don't play - set first item as active

            persistentLogger.debug('Setting first item as active: ' + items[0].id + ' media src: ' + items[0].media?.src, undefined, 'PlayoutController');

            safeSetState(() => {

                setActiveItemId(items[0].id);

                setInitialSeekTime(null);

            });

        }

    };



    // Play Live - sync to current hour position

    const handlePlayLive = async () => {

        const manager = playoutEngineRef.current;

        const useEngine = !!manager;



        if (playlist.length === 0) {

            // Load current hour and play live (rehydration happens inside handleLoadFromSchedule)

            // Reset initialization to allow proper setup

            setIsInitialized(false);

            await handleLoadFromSchedule(undefined, true);

            // Mark as initialized after load completes

            setTimeout(() => setIsInitialized(true), 500);

        } else {

            // Already have items, calculate position and play

            const now = new Date();

            const hourStart = new Date(now);

            hourStart.setMinutes(0, 0, 0);

            const hourStartTime = hourStart.getTime();



            // CRITICAL: Set schedule start time FIRST

            setScheduleStartTime(hourStartTime);



            // Wait for state update

            await new Promise(resolve => setTimeout(resolve, 50));



            const position = calculateTimeAccuratePosition(calculatedPlaylist, hourStartTime);



            if (position.itemId && position.isWithinHour) {

                persistentLogger.debug('Syncing to live position', {

                    ...position,

                    hourStartTime: new Date(hourStartTime).toISOString(),

                    now: new Date().toISOString(),

                }, 'PlayoutController');

                const liveItem = calculatedPlaylist.find(i => i.id === position.itemId);



                setActiveItemId(position.itemId);

                setInitialSeekTime(position.seekTime);

                setIsPlaying(true);



                // Play at calculated position

                if (useEngine && manager && liveItem) {

                    try {

                        if (useCasparAsPrimary && casparStatus.running) {

                            const startIdx = Math.max(0, calculatedPlaylist.findIndex((i) => i.id === liveItem.id));

                            const runlist = buildCasparRunlistFromPosition(calculatedPlaylist, startIdx, position.seekTime);

                            await playActiveChannelOnCaspar(runlist);

                        } else {

                            await manager.playMediaAt(liveItem, position.seekTime);

                        }

                    } catch (error) {

                        logError('Failed to play live', error);

                    }

                }

            } else {

                persistentLogger.warn('Live sync failed - position not found or outside hour', position, 'PlayoutController');

            }

        }

    };



    /** Align playback with wall-clock schedule (armed-style target). Resolves intra-hour gaps and drift. */

    const handleSnapToSchedule = useCallback(async () => {

        const mergedHour = !!loadedSlotId?.includes(',');

        const hourNow = new Date();

        const hourStart = new Date(hourNow);

        hourStart.setMinutes(0, 0, 0);

        const hourStartTime = hourStart.getTime();



        if (calculatedPlaylistRef.current.length === 0) {

            await handleLoadFromSchedule(undefined, isArmed);

            return;

        }



        if (!mergedHour) {

            setScheduleStartTime(hourStartTime);

        }



        await new Promise((resolve) => setTimeout(resolve, 60));



        const items = calculatedPlaylistRef.current;

        const position = calculateTimeAccuratePosition(items, hourStartTime);



        if (!position.itemId) {

            persistentLogger.warn('[SnapSchedule] No position for current time', undefined, 'PlayoutController');

            return;

        }



        const scheduledItem = items.find((i) => i.id === position.itemId);

        if (!scheduledItem) return;



        persistentLogger.debug('[SnapSchedule] Jump to schedule', {

            title: scheduledItem.media?.title,

            seekTime: position.seekTime,

            mergedHour,

        }, 'PlayoutController');



        setActiveItemId(position.itemId);

        setInitialSeekTime(position.seekTime);

        if (isArmed) {

            setIsPlaying(true);

        }



        const manager = playoutEngineRef.current;

        if (manager) {

            try {

                if (useCasparAsPrimary && casparStatus.running) {

                    const startIdx = Math.max(0, items.findIndex((i) => i.id === scheduledItem.id));

                    const runlist = buildCasparRunlistFromPosition(items, startIdx, position.seekTime);

                    await playActiveChannelOnCaspar(runlist);

                } else {

                    await manager.playMediaAt(scheduledItem, position.seekTime);

                }

            } catch (error) {

                logError('[SnapSchedule] External engine failed', error);

            }

        }

    }, [

        loadedSlotId,

        isArmed,

        handleLoadFromSchedule,

        calculateTimeAccuratePosition,

        useCasparAsPrimary,

        casparStatus.running,

        buildCasparRunlistFromPosition,

        playActiveChannelOnCaspar,

    ]);



    const refreshAndSnapAfterSave = useCallback(async () => {

        if (saveSnapInFlightRef.current) return;

        saveSnapInFlightRef.current = true;

        try {

            await handleLoadFromSchedule(undefined, isArmedRef.current);

            await handleSnapToSchedule();

            setHasUnseenScheduleChanges(false);

        } catch (error) {

            persistentLogger.warn('[SaveSnap] Refresh/snap after save failed', error, 'PlayoutController');

        } finally {

            saveSnapInFlightRef.current = false;

        }

    }, [handleLoadFromSchedule, handleSnapToSchedule]);



    useEffect(() => {

        const handleSavedState = (event: Event) => {

            window.setTimeout(() => {

                void refreshAndSnapAfterSave();

            }, 80);

        };

        window.addEventListener('schedule-saved', handleSavedState as EventListener);

        window.addEventListener('playlist-saved', handleSavedState as EventListener);

        return () => {

            window.removeEventListener('schedule-saved', handleSavedState as EventListener);

            window.removeEventListener('playlist-saved', handleSavedState as EventListener);

        };

    }, [refreshAndSnapAfterSave]);



    // CG Handlers

    const handleCGEdit = (element: CGElement) => setEditingCG(element);

    const handleCGSave = (element: CGElement) => {

        cgController.updateElement(element.id, element);

        setEditingCG(null);

    };

    const handleCGAdd = (element: CGElement) => cgController.addElement(element);



    // DDR Handlers

    const handleDDRChannelChange = (channel: DDRChannel, playlist: DDRPlaylist) => {

        setDdrChannels(prev => ({ ...prev, [channel]: playlist }));

    };



    // Side panel toggle

    const toggleSidePanel = (panel: SidePanel) => {

        setSidePanel(current => current === panel ? 'none' : panel);

    };



    const openCGTemplatesWindow = safeHandler(async () => {

        const api = (window as any).electronAPI;

        if (api?.openCGLauncher) {

            await api.openCGLauncher();

            return;

        }

        // In packaged Electron, window.location is file://, so open the local HTTP server route instead.

        if (api?.getAllUrls) {

            try {

                const u = await api.getAllUrls();

                const url =

                    u?.urls?.cgTemplates ||

                    (u?.port ? `http://localhost:${u.port}/cg-templates` : null);

                if (url) {

                    window.open(url, 'CGTemplates', 'width=1400,height=900');

                    return;

                }

            } catch {

                // fall back below

            }

        }

        // Dev/browser fallback

        const base = window.location.origin;

        window.open(`${base}/#/cg-templates`, 'CGTemplates', 'width=1400,height=900');

    }, 'PlayoutController');



    // Calculate totals

    const totalDuration = calculatedPlaylist.reduce((acc, item) => acc + getPlaylistItemDurationSeconds(item), 0);

    const endTime = calculatedPlaylist.length > 0 ? calculatedPlaylist[calculatedPlaylist.length - 1].endTime : null;



    // Strict snap: make "now" line up with the current position in the current item

    // (Only adjusts scheduleStartTime; does not change playback.)

    const handleSnapToNowStrict = useCallback(() => {

        if (!currentMedia || !currentMedia.startTime) return;

        const timeIn = currentMedia.timeIn || 0;

        const posWithinItem = Math.max(0, currentPlaybackTime - timeIn); // seconds into the scheduled segment

        const desiredItemStart = Date.now() - posWithinItem * 1000;

        const delta = desiredItemStart - currentMedia.startTime;

        setScheduleStartTime(prev => prev + delta);

    }, [currentMedia, currentPlaybackTime]);



    // Track initialization state - but be more aggressive about initialization

    const [isInitialized, setIsInitialized] = useState(false);

    const initializationTimeoutRef = useRef<NodeJS.Timeout | null>(null);

    const hasColdStartedRef = useRef(false); // Track if we've done cold start



    // COLD START: On app mount, immediately load current hour and start playing if scheduled

    useEffect(() => {

        // Only run once on mount

        if (hasColdStartedRef.current) return;



        const coldStart = async () => {

            persistentLogger.debug('[ColdStart] App starting - checking for scheduled content...', undefined, 'PlayoutController');

            hasColdStartedRef.current = true;



            try {

                const schedule = await loadWeeklySchedule();

                const now = new Date();

                const currentDay = now.getDay();

                const currentHour = now.getHours();

                const slotId = `${currentDay}-${currentHour}`;

                const slot = schedule.slots[slotId];



                if (slot && slot.items.length > 0) {

                    persistentLogger.debug('[ColdStart] Found scheduled content for current hour, loading and auto-starting...', undefined, 'PlayoutController');

                    // Load and auto-play immediately - no delays, no barriers

                    // TIME RUNS THE GAME - instant snap to current time

                    await handleLoadFromSchedule(slotId, true);

                    // Mark as initialized immediately after load

                    setTimeout(() => setIsInitialized(true), 200);

                } else {

                    persistentLogger.debug('[ColdStart] No scheduled content for current hour', undefined, 'PlayoutController');

                    setIsInitialized(true);

                }

            } catch (error) {

                logError('[ColdStart] Error during cold start', error);

                setIsInitialized(true); // Still mark as initialized to allow manual operation

            }

        };



        // Start immediately, no delay

        coldStart();

    }, []); // Only run once on mount



    // Mark as initialized after playlist is ready (faster for hot starts)

    useEffect(() => {

        // Clear any existing timeout

        if (initializationTimeoutRef.current) {

            clearTimeout(initializationTimeoutRef.current);

        }



        // Wait for playlist and schedule to be ready - but faster (500ms instead of 1500ms)

        if (calculatedPlaylist.length > 0 && scheduleStartTime > 0) {

            initializationTimeoutRef.current = setTimeout(() => {

                setIsInitialized(true);

                persistentLogger.debug('[Init] System initialized and ready for auto-start', undefined, 'PlayoutController');

            }, 500); // Faster initialization

        } else if (calculatedPlaylist.length === 0) {

            // If playlist is empty, still mark as initialized after a short delay

            initializationTimeoutRef.current = setTimeout(() => {

                setIsInitialized(true);

            }, 500);

        }



        return () => {

            if (initializationTimeoutRef.current) {

                clearTimeout(initializationTimeoutRef.current);

            }

        };

    }, [calculatedPlaylist.length, scheduleStartTime]);



    // AGGRESSIVE AUTO-START: Continuously monitor for scheduled content at current moment

    // TIME RUNS THE GAME - If something is scheduled NOW, automatically start playing it

    // User must intervene to STOP, not to START

    //

    // CRITICAL: This effect MUST NOT re-run when channel-switching mutates preview state.

    // It uses a single state ref so deps only include isArmed / isInitialized — user

    // actions and mount — not playlist / activeItemId / isPlaying / scheduleStartTime.

    const autoStartStateRef = useRef({

        calculatedPlaylist,

        isPlaying,

        activeItemId,

        scheduleStartTime,

        handleLoadFromSchedule,

        useCasparAsPrimary,

        casparRunning: casparStatus.running,

        buildCasparRunlistFromPosition,

        playActiveChannelOnCaspar,

    });

    autoStartStateRef.current = {

        calculatedPlaylist,

        isPlaying,

        activeItemId,

        scheduleStartTime,

        handleLoadFromSchedule,

        useCasparAsPrimary,

        casparRunning: casparStatus.running,

        buildCasparRunlistFromPosition,

        playActiveChannelOnCaspar,

    };



    useEffect(() => {

        if (!isArmed) {

            // When disarmed, don't auto-start but keep initialization state

            return;

        }



        const s = autoStartStateRef.current;



        // Allow auto-start even during initialization if we have a playlist

        // This ensures instant snap-to-current-time on cold start

        if (!isInitialized && s.calculatedPlaylist.length === 0) {

                persistentLogger.debug('[AutoStart] Waiting for initialization...', undefined, 'PlayoutController');

            return;

        }



        let checkInterval: NodeJS.Timeout;

        let isChecking = false;

        let hasAutoStarted = false; // Track if we've already auto-started to prevent loops



        const checkAndAutoStart = async () => {

            if (isChecking) return;

            if (channelSwitchInProgressRef.current) return;

            isChecking = true;



            const s = autoStartStateRef.current;



            try {

                const now = Date.now();



                // Check if we have a calculated playlist with items

                if (s.calculatedPlaylist.length === 0) {

                    // No playlist loaded - try to load current hour (but only once)

                    if (!hasAutoStarted) {

                        const schedule = await loadWeeklySchedule();

                        const currentDay = new Date().getDay();

                        const currentHour = new Date().getHours();

                        const slotId = `${currentDay}-${currentHour}`;

                        const slot = schedule.slots[slotId];



                        if (slot && slot.items.length > 0) {

                            persistentLogger.debug('[AutoStart] Detected scheduled content for current hour, loading...', undefined, 'PlayoutController');

                            hasAutoStarted = true;

                            await s.handleLoadFromSchedule(slotId, true); // Auto-start

                            isChecking = false;

                            return;

                        }

                    }

                    isChecking = false;

                    return;

                }



                // Check if there's content scheduled for RIGHT NOW

                const hourStart = new Date();

                hourStart.setMinutes(0, 0, 0);

                const hourStartTime = hourStart.getTime();



                // Find what should be playing NOW using improved time-accurate calculation

                const position = calculateTimeAccuratePosition(s.calculatedPlaylist, hourStartTime);



                // If we found an item that should be playing, but we're not playing

                // AGGRESSIVE: Auto-start if we have a valid position, even if schedule time isn't perfectly set

                // We'll correct the schedule time as we go

                if (position.itemId && position.isWithinHour && !s.isPlaying) {

                    const scheduledItem = s.calculatedPlaylist.find(i => i.id === position.itemId);

                    if (scheduledItem) {

                        // Use actual scheduled times if available, otherwise calculate from hour start

                        const itemStartTime = scheduledItem.startTime || (hourStartTime + (position.itemIndex > 0 ?

                            s.calculatedPlaylist.slice(0, position.itemIndex).reduce((sum, item) => {

                                const timeIn = item.timeIn || 0;

                                const timeOut = item.timeOut ?? item.media.duration;

                                return sum + (timeOut - timeIn);

                            }, 0) * 1000 : 0));

                        const itemEndTime = scheduledItem.endTime || (itemStartTime + (scheduledItem.media.duration * 1000));

                        const nowTime = now;



                        // AGGRESSIVE: Check if we're within the scheduled time window (with 10 second tolerance)

                        // This ensures we catch scheduled content even if timing is slightly off

                        if (nowTime >= itemStartTime - 10000 && nowTime <= itemEndTime + 10000) {

                            persistentLogger.debug('[AutoStart] Detected scheduled content at current moment, auto-starting', {

                                item: scheduledItem.media.title,

                                scheduledStart: new Date(itemStartTime).toISOString(),

                                scheduledEnd: new Date(itemEndTime).toISOString(),

                                now: new Date(nowTime).toISOString(),

                                seekTime: position.seekTime,

                                isWithinHour: position.isWithinHour,

                                scheduleStartTime: new Date(s.scheduleStartTime).toISOString(),

                            }, 'PlayoutController');



                            // Ensure schedule start time is correct BEFORE auto-starting

                            if (Math.abs(s.scheduleStartTime - hourStartTime) > 2000) {

                                persistentLogger.debug('[AutoStart] Correcting schedule start time before auto-start', undefined, 'PlayoutController');

                                setScheduleStartTime(hourStartTime);

                                // Wait a tick for state to update

                                await new Promise(resolve => setTimeout(resolve, 50));

                            }



                            // Auto-start playback immediately

                            setActiveItemId(position.itemId);

                            setInitialSeekTime(position.seekTime);

                            setIsPlaying(true);



                            // If external engine is active, play there too

                            const manager = playoutEngineRef.current;

                            if (manager) {

                                try {

                                    if (s.useCasparAsPrimary && s.casparRunning) {

                                        const startIdx = Math.max(0, s.calculatedPlaylist.findIndex((i) => i.id === scheduledItem.id));

                                        const runlist = s.buildCasparRunlistFromPosition(s.calculatedPlaylist, startIdx, position.seekTime);

                                        await s.playActiveChannelOnCaspar(runlist);

                                    } else {

                                        await manager.playMediaAt(scheduledItem, position.seekTime);

                                    }

                                } catch (error) {

                                    logError('[AutoStart] Error starting external engine', error);

                                }

                            }

                        }

                    }

                }



                // If we're playing but not at the right position, aggressively snap to correct position

                if (s.isPlaying && position.itemId && position.isWithinHour && s.activeItemId !== position.itemId) {

                    const scheduledItem = s.calculatedPlaylist.find(i => i.id === position.itemId);

                    if (scheduledItem) {

                        const itemStartTime = scheduledItem.startTime || 0;

                        const itemEndTime = scheduledItem.endTime || (itemStartTime + scheduledItem.media.duration * 1000);

                        const nowTime = now;



                        // Aggressively snap if we're within the scheduled window (1 second tolerance)

                        if (nowTime >= itemStartTime - 1000 && nowTime <= itemEndTime + 1000) {

                            persistentLogger.debug('[AutoStart] Aggressively snapping to correct scheduled item', {

                                from: s.activeItemId,

                                to: scheduledItem.media.title,

                                scheduledStart: new Date(itemStartTime).toISOString(),

                                now: new Date(nowTime).toISOString(),

                                seekTime: position.seekTime,

                            }, 'PlayoutController');



                            setActiveItemId(position.itemId);

                            setInitialSeekTime(position.seekTime);



                            // Update external engine if active

                            const manager = playoutEngineRef.current;

                            if (manager) {

                                try {

                                    if (s.useCasparAsPrimary && s.casparRunning) {

                                        const startIdx = Math.max(0, s.calculatedPlaylist.findIndex((i) => i.id === scheduledItem.id));

                                        const runlist = s.buildCasparRunlistFromPosition(s.calculatedPlaylist, startIdx, position.seekTime);

                                        await s.playActiveChannelOnCaspar(runlist);

                                    } else {

                                        await manager.playMediaAt(scheduledItem, position.seekTime);

                                    }

                                } catch (error) {

                                    logError('[AutoStart] Error snapping external engine', error);

                                }

                            }

                        }

                    }

                }

            } catch (error) {

                logError('[AutoStart] Error in auto-start check', error);

            } finally {

                isChecking = false;

            }

        };



        // Check every 2 seconds - aggressive monitoring

        checkInterval = setInterval(checkAndAutoStart, 2000);



        // Also check immediately (but skip if a channel switch is in progress)

        if (!channelSwitchInProgressRef.current) {

            checkAndAutoStart();

        }



        return () => clearInterval(checkInterval);

    }, [isArmed, isInitialized]);



    // Top-of-hour auto-update: when a new hour starts, load that hour's playlist

    // (unless we're on merged hours or disarmed/float mode)

    // NEW BEHAVIOR: Always stop current playback and start new hour automatically

    // The new hour always wins - no manual button pushing required

    //

    // CRITICAL: This effect MUST NOT re-run when channel-switching mutates preview state.

    // It uses a single state ref so deps only include isArmed — user action and mount.

    const topOfHourStateRef = useRef({

        loadedSlotId,

        isPlaying,

        useCasparAsPrimary,

        casparRunning: casparStatus.running,

        getActiveCasparChannel,

        handleLoadFromSchedule,

    });

    topOfHourStateRef.current = {

        loadedSlotId,

        isPlaying,

        useCasparAsPrimary,

        casparRunning: casparStatus.running,

        getActiveCasparChannel,

        handleLoadFromSchedule,

    };



    useEffect(() => {

        if (!isArmed) return; // Don't auto-update in disarmed/float mode



        let lastCheckedSlotId: string | null = null;

        let isTransitioning = false; // Prevent multiple simultaneous transitions



        const checkTopOfHour = async () => {

            if (channelSwitchInProgressRef.current) return;

            // Prevent concurrent transitions

            if (isTransitioning) {

                persistentLogger.debug('[TopOfHour] Transition already in progress, skipping check', undefined, 'PlayoutController');

                return;

            }



            const s = topOfHourStateRef.current;

            const now = new Date();

            const currentHour = now.getHours();

            const currentDay = now.getDay();

            const expectedSlotId = `${currentDay}-${currentHour}`;



            // Only update if:

            // 1. We're in armed mode

            // 2. This is a different slot than what's loaded

            // 3. We're not on merged hours

            const isMergedHours = s.loadedSlotId?.includes(',');



            // Skip if already checked this slot

            if (lastCheckedSlotId === expectedSlotId && s.loadedSlotId === expectedSlotId) {

                return;

            }



            if (!isMergedHours && s.loadedSlotId && s.loadedSlotId !== expectedSlotId) {

                isTransitioning = true;

                lastCheckedSlotId = expectedSlotId;



                persistentLogger.debug('[TopOfHour] Hour changed, stopping current playback and loading new schedule: ' + expectedSlotId, undefined, 'PlayoutController');

                persistentLogger.debug('[TopOfHour] Previous slot: ' + s.loadedSlotId + ' New slot: ' + expectedSlotId, undefined, 'PlayoutController');



                // CRITICAL: Stop current playback first to avoid conflicts

                // This ensures the new hour always wins, even if media is already playing

                if (s.isPlaying) {

                    persistentLogger.debug('[TopOfHour] Stopping current playback before loading new hour', undefined, 'PlayoutController');

                    setIsPlaying(false);



                    // Stop external engines if active

                    const manager = playoutEngineRef.current;

                    if (manager) {

                        try {

                            if (s.useCasparAsPrimary && s.casparRunning && (window as any).electronAPI?.casparStopChannel) {

                                const r = await (window as any).electronAPI.casparStopChannel(s.getActiveCasparChannel());

                                if (!r?.ok) throw new Error(r?.error || 'Caspar stop failed');

                                setCasparTransportPlaying(false);

                            } else {

                                await manager.stop();

                            }

                        } catch (error) {

                            logError('[TopOfHour] Error stopping engine', error);

                        }

                    }



                    // Small delay to ensure stop completes and prevents "jamming"

                    await new Promise(resolve => setTimeout(resolve, 200));

                }



                // Now load and start the new hour's playlist

                // Always pass true for playLive to automatically start playback

                try {

                    await s.handleLoadFromSchedule(expectedSlotId, true);

                    persistentLogger.debug('[TopOfHour] Successfully loaded and started new hour: ' + expectedSlotId, undefined, 'PlayoutController');

                } catch (error) {

                    logError('[TopOfHour] Error loading new hour', error);

                } finally {

                    isTransitioning = false;

                }

            } else {

                lastCheckedSlotId = expectedSlotId;

            }

        };



        // Check every 5 seconds (more frequent to catch hour changes quickly)

        // This ensures we catch the transition within 5 seconds of the hour change

        const interval = setInterval(checkTopOfHour, 5000);



        // Also check immediately (but skip if a channel switch is in progress)

        if (!channelSwitchInProgressRef.current) {

            checkTopOfHour();

        }



        return () => clearInterval(interval);

    }, [isArmed]);



    // Native menu bar (Electron): bridge IPC actions into operator handlers

    const menuBridgeRef = useRef({

        setViewMode,

        setHelpOpen,

        setLogViewerOpen,

        navigate,

        toggleOutputWindow,

        openCGTemplatesWindow,

        handleSnapToSchedule,

        handleSnapToTopOfHour,

        handleSnapToNowStrict,

        handleLoadFromSchedule,

        handlePlayLive,

        handlePlayPause,

        handleStop,

        handleNext,

        setIsArmed,

        setIsOutputWindowOpen,

        isOutputWindowOpen,

        isArmed,

    });

    menuBridgeRef.current = {

        setViewMode,

        setHelpOpen,

        setLogViewerOpen,

        navigate,

        toggleOutputWindow,

        openCGTemplatesWindow,

        handleSnapToSchedule,

        handleSnapToTopOfHour,

        handleSnapToNowStrict,

        handleLoadFromSchedule,

        handlePlayLive,

        handlePlayPause,

        handleStop,

        handleNext,

        setIsArmed,

        setIsOutputWindowOpen,

        isOutputWindowOpen,

        isArmed,

    };



    useEffect(() => {

        const api = (window as any).electronAPI;

        if (!api?.onAppMenuAction) return undefined;

        const unsub = api.onAppMenuAction((action: string) => {

            const b = menuBridgeRef.current;

            switch (action) {

                case 'view-playout':

                    b.setViewMode('playout');

                    break;

                case 'view-scheduler':

                    b.setViewMode('scheduler');

                    break;

                case 'view-settings':

                    b.setViewMode('settings');

                    break;

                case 'view-analytics':

                    b.setViewMode('analytics');

                    break;

                case 'view-traffic':

                    b.setViewMode('traffic');

                    break;

                case 'toggle-arm':

                    b.setIsArmed(!b.isArmed);

                    break;

                case 'snap-schedule':

                    void b.handleSnapToSchedule();

                    break;

                case 'snap-toh':

                    b.handleSnapToTopOfHour();

                    break;

                case 'snap-playhead-clock':

                    b.handleSnapToNowStrict();

                    break;

                case 'load-current-hour':

                    void b.handleLoadFromSchedule();

                    break;

                case 'play-live':

                    void b.handlePlayLive();

                    break;

                case 'play-pause':

                    b.handlePlayPause();

                    break;

                case 'stop':

                    void b.handleStop();

                    break;

                case 'next-event':

                    void b.handleNext();

                    break;

                case 'toggle-output-window':

                    void b.toggleOutputWindow();

                    break;

                case 'open-cg-templates':

                    void b.openCGTemplatesWindow();

                    break;

                case 'show-help':

                    b.setHelpOpen(true);

                    break;

                case 'show-logs':

                    b.setLogViewerOpen(true);

                    break;

                default:

                    break;

            }

        });

        return () => unsub();

    }, []);



    return (

        <div

            className={`${styles.appContainer} ${compactMode ? styles.compact : ''}`}

            onDrop={handleFileDrop}

            onDragOver={(e) => e.preventDefault()}

        >

            {/* Navigation Sidebar */}

            <nav className={styles.sidebar}>

                {/* Channel switcher — replaces logo. Wired to multiChannelState. */}

                <div className={styles.channelSwitcher}>

                    {!compactMode && <div className={styles.logo}>▶ PLAY</div>}

                    {multiChannelState.channels.map((ch, idx) => {

                        const isActive = ch.id === multiChannelState.activeChannelId;

                        return (

                            <div

                                key={ch.id}

                                className={`${styles.channelPill} ${isActive ? styles.active : ''}`}

                                onClick={() => setMultiChannelState(prev => ({ ...prev, activeChannelId: ch.id }))}

                                title={ch.name || `Channel ${idx + 1}`}

                                role="button"

                            >

                                <span>CH{idx + 1}</span>

                                {ch.isPlaying && <span className={styles.liveDot} />}

                                {multiChannelState.channels.length > 1 && (

                                    <button

                                        type="button"

                                        className={styles.channelPillClose}

                                        onClick={(e) => {

                                            e.stopPropagation();

                                            const api = (window as any).electronAPI;

                                            const casparChannel = (ch as any).casparChannel || (idx + 1);

                                            if (api?.casparClearChannel) {

                                                api.casparClearChannel(casparChannel).catch(() => {});

                                            }

                                            delete channelSnapshotsRef.current[ch.id];

                                            setMultiChannelState(prev => {

                                                const newChannels = prev.channels.filter(c => c.id !== ch.id);

                                                const newActiveId = prev.activeChannelId === ch.id

                                                    ? newChannels[0]?.id

                                                    : prev.activeChannelId;

                                                return {

                                                    ...prev,

                                                    channels: newChannels,

                                                    activeChannelId: newActiveId,

                                                };

                                            });

                                        }}

                                        title="Remove channel"

                                    >×</button>

                                )}

                            </div>

                        );

                    })}

                    <button

                        className={styles.channelPillAdd}

                        onClick={async () => {

                            const nextNum = multiChannelState.channels.length + 1;

                            const newCh: PlayoutChannel = {

                                id: `ch-${Date.now()}`,

                                name: `Channel ${nextNum}`,

                                scheduleId: 'default',

                                previewEnabled: true,

                                previewPosition: 'main',

                                isPlaying: false,

                            };

                            // Caspar's channel index is 1-based and matches our pill order

                            (newCh as any).casparChannel = nextNum;



                            // Seed empty UI snapshot

                            channelSnapshotsRef.current[newCh.id] = emptyChannelSnapshot;



                            // Tell CasparCG to grow its config to N channels and (re)start.

                            // The default casparDir lives in localStorage and is settable

                            // in the OBS URLs panel; ships with the user-known location.

                            const api = (window as any).electronAPI;

                            const casparDir = (() => {

                                try { return localStorage.getItem('casparDir') || 'C:/casparcg'; }

                                catch { return 'C:/casparcg'; }

                            })();

                            if (api?.casparConfigure) {

                                try {

                                    const casparMediaPath = getCasparMediaPathForCurrentSettings();

                                    await api.casparConfigure({

                                        casparDir,

                                        channels: Math.max(1, Math.min(32, multiChannelState.maxChannels || nextNum)),

                                        videoMode: getCasparVideoModePreference(),

                                        ...(casparMediaPath ? { casparMediaPath } : {}),

                                    });

                                    // Make sure casparcg.exe is up

                                    const st = await api.casparStatus();

                                    if (!st?.data?.running) {

                                        await api.casparStart(casparDir);

                                    }

                                } catch (e) {

                                    console.error('[+] CasparCG configure/start failed:', e);

                                }

                            }



                            setMultiChannelState(prev => {

                                if (prev.channels.length >= prev.maxChannels) return prev;

                                const channels = [...prev.channels, newCh];

                                return {

                                    ...prev,

                                    channels,

                                    activeChannelId: newCh.id,

                                    layoutMode: channels.length === 2 ? '2x1' : '2x2',

                                };

                            });

                        }}

                        disabled={multiChannelState.channels.length >= multiChannelState.maxChannels}

                        title={multiChannelState.channels.length >= multiChannelState.maxChannels

                            ? `Channel limit reached (${multiChannelState.maxChannels})`

                            : 'Add channel'}

                    >

                        <Plus size={14} />

                    </button>

                    {outputDetached && (

                        <div className={styles.affinityBadge} title="On-air playout is running in a detached window — main UI is preview only">

                            ● DETACHED

                        </div>

                    )}

                    {/* OBS Browser Source URLs — one per channel */}

                    <button

                        type="button"

                        className={styles.obsUrlsToggle}

                        onClick={() => setShowObsUrls(s => !s)}

                        title="Browser Source URLs for OBS"

                    >

                        OBS URLs {showObsUrls ? '▾' : '▸'}

                    </button>

                </div>

                {showObsUrls && (

                    <ObsUrlPanel

                        channels={multiChannelState.channels}

                        casparPhysicalChannels={multiChannelState.maxChannels}

                        snapshotsRef={channelSnapshotsRef}

                        activeChannelId={multiChannelState.activeChannelId}

                        activePlaylist={playlist}

                        casparStatus={casparStatus}

                        obsSessions={obsSessions}

                        onChannelObsSessionChange={handleChannelObsSessionChange}

                    />

                )}

                <div className={styles.navItems}>

                    <button className={`${styles.navItem} ${viewMode === 'playout' ? styles.active : ''}`}

                        onClick={() => setViewMode('playout')} title="Playout">

                        <Play size={18} />

                    </button>

                    <button className={`${styles.navItem} ${viewMode === 'scheduler' ? styles.active : ''}`}

                        onClick={() => setViewMode('scheduler')} title="Weekly Schedule">

                        <Calendar size={18} />

                    </button>

                    <button className={`${styles.navItem} ${viewMode === 'timeline' ? styles.active : ''}`}

                        onClick={() => setViewMode('timeline')} title="NLE Timeline">

                        <Film size={18} />

                    </button>

                    {!coreOnly && (

                        <>

                            <button className={`${styles.navItem} ${viewMode === 'analytics' ? styles.active : ''}`}

                                onClick={() => setViewMode('analytics')} title="Analytics">

                                <BarChart2 size={18} />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'traffic' ? styles.active : ''}`}

                                onClick={() => setViewMode('traffic')} title="Traffic & Reports">

                                <Briefcase size={18} />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'settings' ? styles.active : ''}`}

                                onClick={() => setViewMode('settings')} title="Settings">

                                <Settings size={18} />

                            </button>

                            <div className={styles.separator} style={{ margin: '10px 0', borderTop: '1px solid var(--border)' }} />

                            <button className={`${styles.navItem} ${viewMode === 'master' ? styles.active : ''}`}

                                onClick={() => setViewMode('master')} title="Master Control">

                                <Server size={18} color="var(--accent)" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'multigrid' ? styles.active : ''}`}

                                onClick={() => setViewMode('multigrid')} title="Multi-Grid View">

                                <Grid size={18} />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'mux' ? styles.active : ''}`}

                                onClick={() => setViewMode('mux')} title="Broadcast Mux">

                                <Satellite size={18} />

                            </button>

                            <div className={styles.separator} style={{ margin: '10px 0', borderTop: '1px solid var(--border)' }} />

                            <button className={`${styles.navItem} ${viewMode === 'multichannel' ? styles.active : ''}`}

                                onClick={() => setViewMode('multichannel')} title="Multi-Channel Mode">

                                <Monitor size={18} color="#10b981" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'campaigns' ? styles.active : ''}`}

                                onClick={() => { setViewMode('campaigns'); navigate('/campaigns'); }} title="Campaign Management">

                                <Briefcase size={18} color="#f59e0b" />

                            </button>

                            <div className={styles.separator} style={{ margin: '10px 0', borderTop: '1px solid var(--border)' }} />

                            <button className={`${styles.navItem} ${viewMode === 'social' ? styles.active : ''}`}

                                onClick={() => setViewMode('social')} title="Social Media Studio">

                                <MessageSquare size={18} color="#1da1f2" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'talent' ? styles.active : ''}`}

                                onClick={() => setViewMode('talent')} title="Talent Monitor">

                                <Video size={18} color="#e4405f" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'billing' ? styles.active : ''}`}

                                onClick={() => setViewMode('billing')} title="Traffic & Billing">

                                <DollarSign size={18} color="#22c55e" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'filesync' ? styles.active : ''}`}

                                onClick={() => setViewMode('filesync')} title="P2P File Sync">

                                <Cloud size={18} color="#8b5cf6" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'starfile' ? styles.active : ''}`}

                                onClick={() => setViewMode('starfile')} title="Starfile Workspace">

                                <FolderOpen size={18} color="#86efac" />

                            </button>

                            <button className={`${styles.navItem} ${viewMode === 'utilities' ? styles.active : ''}`}

                                onClick={() => setViewMode('utilities')} title="Utilities">

                                <Wrench size={18} color="#f97316" />

                            </button>

                        </>

                    )}

                    {coreOnly && (

                        <>

                            <button className={styles.navItem}

                                onClick={() => navigate('/reports')} title="As-Played & Reports">

                                <FileText size={18} />

                            </button>

                            <button className={`${styles.navItem} ${location.pathname === '/remote' ? styles.active : ''}`}

                                onClick={() => navigate('/remote')} title="Remote Sessions">

                                <Wifi size={18} />

                            </button>

                        </>

                    )}

                </div>

                {!coreOnly && (

                <div className={styles.navBottom}>

                    <button className={`${styles.navItem} ${sidePanel === 'router' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('router')} title="Video Router">

                        <Maximize size={18} />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'obs' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('obs')} title="OBS Control">

                        <Radio size={18} />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'ddr' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('ddr')} title="DDR Players">

                        <Film size={18} />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'cg' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('cg')} title="CG Controller">

                        <Layers size={18} />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'caspercg' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('caspercg')} title="CasperCG Server">

                        <Server size={18} />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'cgdesigner' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('cgdesigner')} title="CG Designer (WYSIWYG)">

                        <Palette size={18} color="#ec4899" />

                    </button>

                    <button className={`${styles.navItem} ${sidePanel === 'obsadvanced' ? styles.active : ''}`}

                        onClick={() => toggleSidePanel('obsadvanced')} title="OBS Studio Advanced">

                        <Target size={18} color="#ff0000" />

                    </button>

                    <button className={styles.navItem}

                        onClick={openCGTemplatesWindow} title="CG Templates">

                        <Sparkles size={18} />

                    </button>

                    <button className={styles.navItem}

                        onClick={() => setHelpOpen(true)} title="Help (F1)">

                        <HelpCircle size={18} />

                    </button>

                    <button className={styles.navItem}

                        onClick={() => setLogViewerOpen(true)} title="Logs (F2)">

                        <FileText size={18} />

                    </button>

                    <button

                        className={`${styles.navItem} ${outputDetached ? styles.active : ''}`}

                        onClick={async () => {

                            try {

                                await (window as any).electronAPI?.openOutputWindow?.();

                                setOutputDetached(true);

                            } catch (e) { console.error(e); }

                        }}

                        title="Detach playout to its own window — gives the engine its own process and frees the UI"

                    >

                        <ExternalLink size={18} />

                    </button>

                    <button

                        className={`${styles.navItem} ${compactMode ? styles.active : ''}`}

                        onClick={() => setCompactMode(c => !c)}

                        title={compactMode ? 'Normal density' : 'Compact density (low-DPI)'}

                    >

                        <Minimize2 size={18} />

                    </button>

                </div>

                )}

                {coreOnly && (

                <div className={styles.navBottom}>

                    <button

                        className={`${styles.navItem} ${outputDetached ? styles.active : ''}`}

                        onClick={async () => {

                            try {

                                await (window as any).electronAPI?.openOutputWindow?.();

                                setOutputDetached(true);

                            } catch (e) { console.error(e); }

                        }}

                        title="Detach playout to its own window"

                    >

                        <ExternalLink size={18} />

                    </button>

                    <button

                        className={`${styles.navItem} ${compactMode ? styles.active : ''}`}

                        onClick={() => setCompactMode(c => !c)}

                        title={compactMode ? 'Normal density' : 'Compact density (low-DPI)'}

                    >

                        <Minimize2 size={18} />

                    </button>

                </div>

                )}

            </nav>



            {/* Main Content Area */}

            <div className={styles.mainArea}>

                <div className={styles.operatorMenuBar}>

                    <div className={styles.menuGroup}>

                        <button type="button" className={`${styles.menuButton} ${viewMode === 'playout' ? styles.menuButtonActive : ''}`} onClick={() => setViewMode('playout')}>

                            <Play size={14} />

                            <span>On Air</span>

                        </button>

                        <button type="button" className={`${styles.menuButton} ${viewMode === 'scheduler' ? styles.menuButtonActive : ''}`} onClick={() => setViewMode('scheduler')}>

                            <Calendar size={14} />

                            <span>Schedule</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={() => navigate('/reports')}>

                            <FileText size={14} />

                            <span>Reports</span>

                        </button>

                        <button type="button" className={`${styles.menuButton} ${viewMode === 'timeline' ? styles.menuButtonActive : ''}`} onClick={() => setViewMode('timeline')}>

                            <Layers size={14} />

                            <span>Timeline</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={() => navigate('/remote')}>

                            <Wifi size={14} />

                            <span>Remote</span>

                        </button>

                    </div>

                    <div className={styles.menuGroup}>

                        <button type="button" className={styles.menuButton} onClick={toggleOutputWindow}>

                            <ExternalLink size={14} />

                            <span>{isOutputWindowOpen ? 'Close Output' : 'Output'}</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={openCGTemplatesWindow}>

                            <Sparkles size={14} />

                            <span>CG Templates</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={() => setViewMode('utilities')}>

                            <Wrench size={14} />

                            <span>Tools</span>

                        </button>

                    </div>

                    <div className={styles.menuGroup}>

                        <button type="button" className={styles.menuButton} onClick={() => setViewMode('settings')}>

                            <Settings size={14} />

                            <span>Config</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={() => setLogViewerOpen(true)}>

                            <FileText size={14} />

                            <span>Logs</span>

                        </button>

                        <button type="button" className={styles.menuButton} onClick={() => setHelpOpen(true)}>

                            <HelpCircle size={14} />

                            <span>Help</span>

                        </button>

                    </div>

                </div>

                <div className={styles.workspaceArea}>

                {/* (Removed: <ChannelTabs> bottom strip — duplicated the sidebar

                     pills and the Preview Jack. Channel removal moved to the

                     pill itself / context menu.) */}

                {false && (

                    <ChannelTabs

                        channels={multiChannelState.channels}

                        activeChannelId={multiChannelState.activeChannelId}

                        onSelectChannel={(channelId) => {

                            setMultiChannelState(prev => ({ ...prev, activeChannelId: channelId }));

                        }}

                        onRemoveChannel={(channelId) => {

                            if (multiChannelState.channels.length <= 1) return;

                            const api = (window as any).electronAPI;

                            // Stop + clear the corresponding CasparCG channel

                            const idx = multiChannelState.channels.findIndex(c => c.id === channelId);

                            const casparChannel = (multiChannelState.channels[idx] as any)?.casparChannel || (idx + 1);

                            if (api?.casparClearChannel && casparChannel) {

                                api.casparClearChannel(casparChannel).catch((e: unknown) =>

                                    console.error('caspar clear failed:', e));

                            }

                            // (CasparCG won't shed the channel from its config until next configure;

                            //  we'll regenerate the config on the next [+] anyway. For an exact

                            //  trim, the user can hit "Reconfigure" — TODO.)



                            // Drop its UI snapshot

                            delete channelSnapshotsRef.current[channelId];

                            setMultiChannelState(prev => {

                                const newChannels = prev.channels.filter(c => c.id !== channelId);

                                const newActiveId = prev.activeChannelId === channelId

                                    ? newChannels[0]?.id

                                    : prev.activeChannelId;

                                return {

                                    ...prev,

                                    channels: newChannels,

                                    activeChannelId: newActiveId,

                                    layoutMode: newChannels.length === 1 ? '1x1' : newChannels.length === 2 ? '2x1' : '2x2',

                                };

                            });

                        }}

                        onBreakoutChannel={(channelId) => {

                            const channel = multiChannelState.channels.find(c => c.id === channelId);

                            if (channel) {

                                openBreakoutWindow(channelId, channel.name);

                            }

                        }}

                        maxChannels={multiChannelState.maxChannels}

                    />

                )}

                

                {/* Always-on playout/output dock (never unmounts) */}

                <section className={styles.leftDock} style={{ width: Math.max(rightResizer.width, 760) }}>

                    <StudioClock />

                    

                    {/* Preview Jack — single monitor with channel patching strip.

                        Replaces the old metadata grid: one engine + a router-style

                        selector to choose which channel feeds the preview. */}

                    {multiChannelState.channels.length > 1 && (

                        <div className={styles.previewJack}>

                            <span className={styles.previewJackLabel}>PREVIEW</span>

                            <div className={styles.previewJackButtons}>

                                {multiChannelState.channels.map((ch, idx) => {

                                    const isOn = ch.id === multiChannelState.activeChannelId;

                                    return (

                                        <button

                                            key={ch.id}

                                            type="button"

                                            className={`${styles.jackButton} ${isOn ? styles.jackButtonOn : ''}`}

                                            onClick={() => setMultiChannelState(prev => ({ ...prev, activeChannelId: ch.id }))}

                                            title={`Patch preview to ${ch.name || `Channel ${idx + 1}`}`}

                                        >

                                            <span>CH{idx + 1}</span>

                                            {ch.isPlaying && <span className={styles.jackLiveDot} />}

                                        </button>

                                    );

                                })}

                            </div>

                            <span className={styles.previewJackCurrent}>

                                ► {multiChannelState.channels.find(c => c.id === multiChannelState.activeChannelId)?.name || '—'}

                            </span>

                        </div>

                    )}

                    <div className={styles.playerWrapper} ref={playerWrapperRef}>

                        <PlayerEngine

                            currentMedia={currentMedia}

                            nextMedia={nextMedia}

                            isPlaying={enginePlaybackState?.isPlaying ?? isPlaying}

                            onEnded={handleMediaEnded}

                            onTimeUpdate={handlePlaybackTimeUpdate}

                            initialSeekTime={initialSeekTime}

                            onSeekComplete={() => setInitialSeekTime(null)}

                            defaultMuted={true}

                            enableEnhancements={!coreOnly}

                            audioControlsSlot="gutter"

                        />

                        {!coreOnly && <CGOverlay elements={cgController.elements} cgLayers={cgLayers} />}

                    </div>

                    <div className={styles.controlsGrid}>

                        <div className={styles.transportRow}>

                            <button

                                className={`${styles.controlBtn} ${styles.armBtn} ${isArmed ? styles.armed : ''}`}

                                onClick={() => setIsArmed(!isArmed)}

                                title={isArmed ? "Disarm" : "Arm schedule follow"}

                            >

                                <Anchor size={16} />

                                <span className={styles.btnLabel}>{isArmed ? 'ARMED' : 'ARM'}</span>

                            </button>

                            <button

                                type="button"

                                className={`${styles.controlBtn} ${styles.snapScheduleBtn}`}

                                onClick={() => void handleSnapToSchedule()}

                                title="Snap playout to the wall-clock correct item and in-point (menu: Playout → Snap to scheduled time, Ctrl+Shift+T). When armed, starts transport."

                            >

                                <Target size={16} />

                                <span className={styles.btnLabel}>SCHED</span>

                            </button>

                            {!coreOnly && (

                                <button

                                    className={`${styles.controlBtn} ${styles.strictBtn} ${isStrictMode ? styles.strictActive : ''}`}

                                    onClick={() => setIsStrictMode(!isStrictMode)}

                                    title={isStrictMode ? "Disable strict" : "Strict 10s"}

                                >

                                    <ShieldAlert size={16} />

                                    <span className={styles.btnLabel}>{isStrictMode ? 'STRICT' : 'RELAX'}</span>

                                </button>

                            )}

                            <button className={styles.controlBtn} onClick={handleStop} title="Stop">

                                <Square size={16} />

                            </button>

                            <button className={`${styles.controlBtn} ${styles.playBtn} ${effectiveIsPlaying ? styles.playing : ''}`} onClick={handlePlayPause} title={effectiveIsPlaying ? 'Pause' : 'Play'}>

                                {effectiveIsPlaying ? <Pause size={18} /> : <Play size={18} />}

                            </button>

                            <button className={styles.controlBtn} onClick={handleNext} title="Next Event">

                                <SkipForward size={16} />

                            </button>

                            {!coreOnly && (

                                <button className={`${styles.controlBtn} ${styles.liveBtn}`} onClick={handlePlayLive} title="Play Live (current hour)">

                                    <Radio size={16} />

                                    <span className={styles.btnLabel}>LIVE</span>

                                </button>

                            )}

                            {/* Push the current playlist to the active channel's CasparCG channel and start it. */}

                            <button

                                className={`${styles.controlBtn} ${useCasparAsPrimary ? styles.liveBtn : ''}`}

                                onClick={() => setUseCasparAsPrimary((prev) => !prev)}

                                title="Toggle CasparCG as primary transport engine for play/snap/autostart"

                            >

                                <Tv size={16} />

                                <span className={styles.btnLabel}>

                                    {useCasparAsPrimary

                                        ? `CASPAR ON · CH${getActiveCasparChannel()}`

                                        : 'CASPAR OFF'}

                                </span>

                            </button>

                            {isElectron && (

                                <button

                                    className={styles.controlBtn}

                                    onClick={() => (window as any).electronAPI?.openMultiviewer?.()}

                                    title="Open detached multi-viewer with VU meters and OTA volume controls"

                                >

                                    <Monitor size={16} />

                                    <span className={styles.btnLabel}>MULTIVIEW</span>

                                </button>

                            )}

                            {/* LOAD HOUR — pulls the current wall-clock hour from the schedule into the playlist.

                                Always visible here so the operator doesn't have to hunt through panels. */}

                            <button

                                className={styles.controlBtn}

                                onClick={async () => {

                                    try {

                                        await handleLoadFromSchedule();

                                        setHasUnseenScheduleChanges(false);

                                        const n = (calculatedPlaylistRef.current || []).length;

                                        showToast(n > 0 ? `Loaded ${n} item(s) from current hour` : 'No items in current hour', n > 0 ? 'success' : 'warning');

                                    } catch (e) {

                                        showToast(`Load failed: ${(e as Error).message}`, 'error');

                                    }

                                }}

                                title="Load the current wall-clock hour's slot from the schedule into the playlist"

                            >

                                <Download size={16} />

                                <span className={styles.btnLabel}>LOAD HOUR</span>

                            </button>

                            <button

                                className={`${styles.controlBtn} ${styles.pushBtn}`}

                                onClick={async () => {

                                    const idx = multiChannelState.channels.findIndex(c => c.id === multiChannelState.activeChannelId);



                                    // Prefer the smart-resolved calculatedPlaylist (segments + start/end times)

                                    // over the raw playlist state. Fall back to playlist if it's empty.

                                    let itemsToPush: PlaylistItem[] = (calculatedPlaylist?.length ? calculatedPlaylist : playlist) || [];



                                    // If nothing is loaded yet, auto-load the current hour from the schedule

                                    // (mirrors handlePlayLive's behaviour). This is the #1 cause of "push

                                    // sends only CLEAR" — the user hasn't clicked Load Hour yet.

                                    if (itemsToPush.length === 0) {

                                        showToast('No hour loaded — loading current hour…', 'info');

                                        try {

                                            await handleLoadFromSchedule(undefined, false);

                                        } catch (e) {

                                            console.error('[PUSH] auto-load failed', e);

                                        }

                                        // Wait a tick for state to settle, then re-read via ref

                                        await new Promise(r => setTimeout(r, 250));

                                        itemsToPush = (calculatedPlaylistRef.current?.length

                                            ? calculatedPlaylistRef.current

                                            : (playlist || [])) as PlaylistItem[];

                                    }



                                    if (itemsToPush.length === 0) {

                                        showToast('No hour scheduled right now — load one in Schedule view first', 'warning');

                                        return;

                                    }



                                    try {

                                        const wallHour = new Date();

                                        wallHour.setMinutes(0, 0, 0);

                                        const hourAnchorMs = scheduleStartTime ?? wallHour.getTime();

                                        const casparRunlist = buildCasparRunlistForBossClock(itemsToPush, hourAnchorMs);

                                        const r = await playActiveChannelOnCaspar(casparRunlist);

                                        const queued = (r as any)?.data?.queued ?? itemsToPush.length;

                                        const skipped = (r as any)?.data?.skipped ?? 0;

                                        showToast(

                                            `Pushed ${queued}/${itemsToPush.length} to CH${idx + 1}` +

                                            (skipped ? ` · ${skipped} skipped (unplayable in Caspar)` : ''),

                                            'success'

                                        );

                                    } catch (e) {

                                        showToast(`Push error: ${(e as Error).message}`, 'error');

                                    }

                                }}

                                title="Push this hour's playlist (with segments) to the active channel's CasparCG output. Auto-loads the current hour if none is loaded."

                            >

                                <ExternalLink size={16} />

                                <span className={styles.btnLabel}>

                                    PUSH › CH{(multiChannelState.channels.findIndex(c => c.id === multiChannelState.activeChannelId) + 1) || 1}

                                </span>

                            </button>

                        </div>

                        <div className={styles.interfaceRail} aria-label="Interface tools">

                            {!coreOnly && (

                                <>

                                    <button className={`${styles.controlBtn} ${styles.breakBtn}`} onClick={handleQuickBreak} title="Insert ad break">

                                        <Briefcase size={14} />

                                    </button>

                                    <button className={styles.controlBtn} onClick={() => setShowFacebookStoryPoster(true)} title="Facebook Story">

                                        <Share2 size={14} />

                                    </button>

                                    <button className={styles.controlBtn} onClick={() => setViewMode('social')} title="Social">

                                        <MessageCircle size={14} />

                                    </button>

                                </>

                            )}

                            <button className={styles.controlBtn} onClick={handleFullscreen} title="Fullscreen"><Maximize size={14} /></button>

                            <button

                                className={`${styles.controlBtn} ${isOutputWindowOpen ? styles.outputActive : ''}`}

                                onClick={toggleOutputWindow}

                                title={isOutputWindowOpen ? "Close Output" : "Open Output Window"}

                            >

                                <ExternalLink size={14} />

                            </button>

                            <button className={styles.controlBtn} onClick={copyOutputUrl} title="Copy output URL"><Link2 size={14} /></button>

                            {showOutputUrlTooltip && <div className={styles.copyTooltip}>Copied!</div>}

                        </div>

                    </div>

                    <div className={styles.leftRundownSection}>

                        <PlaylistEditor

                            playlist={calculatedPlaylist}

                            setPlaylist={setPlaylist}

                            onRemove={handleRemoveItem}

                            onSetFixedTime={handleSetFixedTime}

                            activeItemId={activeItemId}

                            onItemContextMenu={handlePlaylistItemContextMenu}

                            scheduleStartTime={scheduleStartTime}

                            channelName="PLAY PLAY"

                            onPSIPUpdate={handlePSIPUpdate}

                            currentPlaybackTime={currentPlaybackTime}

                            isPlaying={enginePlaybackState?.isPlaying ?? isPlaying}

                        />

                    </div>

                </section>



                <div className={styles.rightArea}>

                    <ScheduleTicker scheduleStartTime={scheduleStartTime} />

                    {!coreOnly && (

                        <MultiSiteTicker

                            scheduleStartTime={scheduleStartTime}

                            onToggleSiteInterface={(siteId) => setActiveSiteId(prev => prev === siteId ? null : siteId)}

                            activeSiteId={activeSiteId}

                        />

                    )}



                    <div className={styles.contentArea}>

                        {viewMode === 'playout' && (

                            <div className={styles.twoColumnPane}>

                                {!coreOnly && (

                                    <div className={styles.playoutTabBar}>

                                        <button

                                            type="button"

                                            className={`${styles.playoutTab} ${playoutRightTab === 'library' ? styles.playoutTabActive : ''}`}

                                            onClick={() => setPlayoutRightTab('library')}

                                        >

                                            Library

                                        </button>

                                        <button

                                            type="button"

                                            className={`${styles.playoutTab} ${playoutRightTab === 'inputs' ? styles.playoutTabActive : ''}`}

                                            onClick={() => setPlayoutRightTab('inputs')}

                                        >

                                            Virtual Inputs

                                        </button>

                                        <button

                                            type="button"

                                            className={`${styles.playoutTab} ${playoutRightTab === 'cgtemplates' ? styles.playoutTabActive : ''}`}

                                            onClick={() => setPlayoutRightTab('cgtemplates')}

                                        >

                                            CG Templates

                                        </button>

                                    </div>

                                )}

                                <div className={styles.playoutTabContent}>

                                    {!coreOnly && playoutRightTab === 'library' && (

                                        <aside className={styles.libraryColumn}>

                                            <ErrorBoundary componentName="MediaLibrary">

                                                <MediaLibrary

                                                    onItemClick={handleAddMedia}

                                                    onItemContextMenu={handleMediaContextMenu}

                                                    preferredViewMode="grid"

                                                    isPlaying={isPlaying}

                                                />

                                            </ErrorBoundary>

                                        </aside>

                                    )}

                                    {!coreOnly && playoutRightTab === 'cgtemplates' && (

                                        <aside className={styles.libraryColumn}>

                                            <CGTemplateShelf />

                                        </aside>

                                    )}

                                    {!coreOnly && playoutRightTab === 'inputs' && (

                                        <aside className={styles.libraryColumn}>

                                            <InputManager />

                                        </aside>

                                    )}

                                    {coreOnly && (

                                        <div className={styles.runOfShowSection}>

                                            <div className={coreOnly ? styles.scheduleHeaderLite : styles.scheduleHeader}>

                                                {/* Always show the Load Current Hour bar — operators need it

                                                    in both core and full modes. */}

                                                <div className={styles.currentSlotBar}>

                                                    <span className={styles.currentSlotLabel}>

                                                        {loadedSlotId

                                                            ? (() => {

                                                                  const [d, h] = loadedSlotId.split('-').map(Number);

                                                                  const days = 'Sun Mon Tue Wed Thu Fri Sat'.split(' ');

                                                                  return `${days[d] ?? '?'} ${h}:00`;

                                                              })()

                                                            : 'No slot loaded'}

                                                    </span>

                                                    <button

                                                        className={styles.loadBtnPrimary}

                                                        onClick={async () => {

                                                            await handleLoadFromSchedule();

                                                            setHasUnseenScheduleChanges(false);

                                                        }}

                                                        title="Load current hour from schedule"

                                                    >

                                                        <Download size={14} /> Load current hour

                                                    </button>

                                                </div>

                                                <div className={styles.scheduleInfo}>

                                                    <span className={styles.scheduleLabel}>START</span>

                                                    <span className={styles.scheduleTime} suppressHydrationWarning>{new Date(scheduleStartTime).toLocaleTimeString()}</span>

                                                </div>

                                                <div className={styles.scheduleInfo}>

                                                    <span className={styles.scheduleLabel}>DURATION</span>

                                                    <span className={styles.scheduleTime}>{Math.floor(totalDuration / 60)}m {Math.round(totalDuration % 60)}s</span>

                                                </div>

                                                {!coreOnly && endTime && (

                                                    <div className={styles.scheduleInfo}>

                                                        <span className={styles.scheduleLabel}>END</span>

                                                        <span className={styles.scheduleTime} suppressHydrationWarning>{new Date(endTime).toLocaleTimeString()}</span>

                                                    </div>

                                                )}

                                                {!coreOnly && (

                                                    <>

                                                        <button type="button" className={styles.snapBtn} onClick={() => void handleSnapToSchedule()} title="Snap to wall-clock schedule position (same target as armed follow)">

                                                            <Target size={14} /> Snap schedule

                                                        </button>

                                                        <button type="button" className={styles.snapBtn} onClick={() => setIsFreePlayMode((v) => !v)} title="Toggle free-play mode">

                                                            <Play size={14} /> {isFreePlayMode ? 'Free Play ON' : 'Free Play OFF'}

                                                        </button>

                                                        <input

                                                            type="time"

                                                            value={freePlayQueuedStart}

                                                            onChange={(e) => setFreePlayQueuedStart(e.target.value)}

                                                            style={{ background: 'var(--surface)', color: 'var(--text-primary)', border: '1px solid var(--border)', borderRadius: 6, padding: '0.35rem' }}

                                                            title="Optional queued start time for free play"

                                                        />

                                                        <button

                                                            type="button"

                                                            className={styles.snapBtn}

                                                            onClick={async () => {

                                                                const s = loadSettings() as any;

                                                                if (s.ttsAnnouncementsEnabled && s.ttsAnnounceQueuedStarts) {

                                                                    const audio = await generateTTSAudio('Queued free play start is armed.');

                                                                    await audio?.play().catch(() => null);

                                                                }

                                                                showToast(`Free play ${freePlayQueuedStart ? `queued at ${freePlayQueuedStart}` : 'queued now'}.`, 'success');

                                                            }}

                                                            title="Queue free-play start"

                                                        >

                                                            <Clock size={14} /> Queue Start

                                                        </button>

                                                        <button type="button" className={styles.snapBtn} onClick={handleSnapToTopOfHour} title="Anchor timeline start to top of current hour">

                                                            <Anchor size={14} /> Anchor TOH

                                                        </button>

                                                        <button type="button" className={styles.snapBtn} onClick={handleSnapToNowStrict} title="Shift schedule start so clock matches current playhead (drift trim)" disabled={!currentMedia}>

                                                            <Clock size={14} /> Align clock to playhead

                                                        </button>

                                                        <button className={styles.loadBtn} onClick={async () => {

                                                            await handleLoadFromSchedule();

                                                            setHasUnseenScheduleChanges(false);

                                                        }} title="Load current hour">

                                                            <Download size={14} /> Load Schedule

                                                        </button>

                                                        <button

                                                            className={styles.loadBtn}

                                                            onClick={async () => {

                                                                await handleLoadFromSchedule();

                                                                setHasUnseenScheduleChanges(false);

                                                            }}

                                                            title="Reload schedule changes not yet applied"

                                                        >

                                                            <RefreshCw size={14} /> {hasUnseenScheduleChanges ? 'Reload updates' : 'Reload'}

                                                        </button>

                                                    </>

                                                )}

                                                {coreOnly && (

                                                    <button type="button" className={styles.snapBtn} onClick={handleSnapToTopOfHour} title="Anchor timeline to top of current hour">

                                                        <Anchor size={14} /> TOH

                                                    </button>

                                                )}

                                            </div>

                                            <PlaylistEditor

                                                playlist={calculatedPlaylist}

                                                setPlaylist={setPlaylist}

                                                onRemove={handleRemoveItem}

                                                onSetFixedTime={handleSetFixedTime}

                                                activeItemId={activeItemId}

                                                onItemContextMenu={handlePlaylistItemContextMenu}

                                                scheduleStartTime={scheduleStartTime}

                                                channelName="PLAY PLAY"

                                                onPSIPUpdate={handlePSIPUpdate}

                                                currentPlaybackTime={currentPlaybackTime}

                                                isPlaying={enginePlaybackState?.isPlaying ?? isPlaying}

                                            />

                                        </div>

                                    )}

                                </div>

                            </div>

                        )}



                        {viewMode === 'scheduler' && (

                            <div

                                className={styles.layout}

                                style={{

                                    gridTemplateColumns: `${leftResizer.width}px auto 1fr`,

                                }}

                            >

                                <aside className={styles.libraryColumn}>

                                    <ErrorBoundary componentName="MediaLibrary">

                                        <MediaLibrary

                                            onItemClick={handleAddMedia}

                                            onItemContextMenu={handleMediaContextMenu}

                                            preferredViewMode="list"

                                            isPlaying={isPlaying}

                                        />

                                    </ErrorBoundary>

                                </aside>

                                <div

                                    className={`${styles.resizer} ${leftResizer.isDragging ? styles.resizerActive : ''}`}

                                    onMouseDown={leftResizer.handleMouseDown}

                                    onDoubleClick={leftResizer.reset}

                                    title="Drag to resize, double-click to reset"

                                />

                                <section className={styles.mainColumn} style={{ flex: 1, overflow: 'hidden' }}>

                                    {/* Channel tab bar — switches which channel's schedule is displayed */}

                                    <div style={{

                                        display: 'flex', alignItems: 'center', gap: 2,

                                        padding: '4px 8px', borderBottom: '1px solid var(--border)',

                                        background: 'var(--surface)', overflowX: 'auto', flexShrink: 0,

                                    }}>

                                        {multiChannelState.channels.map((ch, idx) => {

                                            const isActive = ch.id === multiChannelState.activeChannelId;

                                            const hasOutput = !!(ch as any).casparChannel;

                                            return (

                                                <button

                                                    key={ch.id}

                                                    type="button"

                                                    onClick={() => {

                                                        // Pure UI channel switch — the central useEffect handles

                                                        // snapshot capture/restore and event dispatching.

                                                        setMultiChannelState(prev => ({ ...prev, activeChannelId: ch.id }));

                                                    }}

                                                    style={{

                                                        padding: '4px 12px', borderRadius: 5, border: 'none',

                                                        cursor: 'pointer', fontSize: '0.72rem', fontWeight: isActive ? 700 : 500,

                                                        background: isActive ? 'rgba(99,102,241,0.2)' : 'transparent',

                                                        color: isActive ? '#a5b4fc' : hasOutput ? 'var(--text-secondary)' : 'rgba(255,255,255,0.25)',

                                                        borderBottom: isActive ? '2px solid #818cf8' : '2px solid transparent',

                                                        whiteSpace: 'nowrap', display: 'flex', alignItems: 'center', gap: 4,

                                                    }}

                                                >

                                                    CH{idx + 1}

                                                    {ch.isPlaying && <span style={{ width: 5, height: 5, borderRadius: '50%', background: '#22c55e', flexShrink: 0 }} />}

                                                    {!hasOutput && <span style={{ fontSize: '0.58rem', opacity: 0.5 }}>OFF</span>}

                                                </button>

                                            );

                                        })}

                                        <div style={{ flex: 1 }} />

                                        {/* Auto-push toggle */}

                                        <button

                                            type="button"

                                            title={autoPushEnabled ? 'Auto-push ON — hours push automatically at :00' : 'Auto-push OFF — hours must be pushed manually'}

                                            onClick={() => setAutoPushEnabled(p => !p)}

                                            style={{

                                                padding: '3px 10px', borderRadius: 999, border: 'none', cursor: 'pointer',

                                                fontSize: '0.68rem', fontWeight: 700,

                                                background: autoPushEnabled ? 'rgba(34,197,94,0.15)' : 'rgba(255,255,255,0.05)',

                                                color: autoPushEnabled ? '#22c55e' : 'rgba(255,255,255,0.3)',

                                                display: 'flex', alignItems: 'center', gap: 5, whiteSpace: 'nowrap',

                                            }}

                                        >

                                            <span style={{ width: 6, height: 6, borderRadius: '50%', background: 'currentColor' }} />

                                            AUTO PUSH

                                        </button>

                                    </div>



                                    {!coreOnly && (

                                    <div className={styles.schedulerTabBar}>

                                        <button

                                            className={`${styles.schedulerTab} ${schedulerTab === 'schedule' ? styles.schedulerTabActive : ''}`}

                                            onClick={() => setSchedulerTab('schedule')}

                                        >

                                            Schedule Grid

                                        </button>

                                        <button

                                            className={`${styles.schedulerTab} ${schedulerTab === 'automation' ? styles.schedulerTabActive : ''}`}

                                            onClick={() => setSchedulerTab('automation')}

                                        >

                                            Social / Automation

                                        </button>

                                    </div>

                                    )}

                                    <div className={styles.schedulerPane}>

                                        {(coreOnly || schedulerTab === 'schedule') ? (

                                            <ErrorBoundary componentName="UnifiedScheduler">

                                                <UnifiedScheduler

                                                    obsSessions={obsSessions}

                                                    mode="schedule"

                                                    viewType="single"

                                                    onAnalyticsFocus={handleFocusAnalytics}

                                                />

                                            </ErrorBoundary>

                                        ) : (

                                            <AutomationBoard obsSessions={obsSessions} onSessionsChange={setObsSessions} />

                                        )}

                                    </div>

                                </section>

                            </div>

                        )}

                        {viewMode === 'traffic' && !coreOnly && (

                            <div className={styles.fullView} style={{ padding: 0, overflow: 'hidden' }}>

                                <div style={{ display: 'flex', flexDirection: 'column', width: '100%', height: '100%', overflow: 'hidden' }}>

                                    <div style={{ display: 'flex', borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}>

                                        <button

                                            style={{

                                                padding: '1.1rem 2rem',

                                                fontWeight: 700,

                                                fontSize: '1rem',

                                                lineHeight: 1.1,

                                                color: trafficTab === 'clients' ? 'var(--text-primary)' : 'var(--text-secondary)',

                                                cursor: 'pointer',

                                                background: 'none',

                                                border: 'none',

                                                borderBottom: trafficTab === 'clients' ? '3px solid var(--accent)' : '3px solid transparent',

                                            }}

                                            onClick={() => setTrafficTab('clients')}

                                        >

                                            Client Manager

                                        </button>

                                        <button

                                            style={{

                                                padding: '1.1rem 2rem',

                                                fontWeight: 700,

                                                fontSize: '1rem',

                                                lineHeight: 1.1,

                                                color: trafficTab === 'reports' ? 'var(--text-primary)' : 'var(--text-secondary)',

                                                cursor: 'pointer',

                                                background: 'none',

                                                border: 'none',

                                                borderBottom: trafficTab === 'reports' ? '3px solid var(--accent)' : '3px solid transparent',

                                            }}

                                            onClick={() => setTrafficTab('reports')}

                                        >

                                            Reports

                                        </button>

                                    </div>

                                    <div style={{ flex: 1, overflow: 'hidden' }}>

                                        {trafficTab === 'clients' ? <ClientManager /> : <TrafficReport />}

                                    </div>

                                </div>

                            </div>

                        )}

                        {viewMode === 'analytics' && !coreOnly && (

                            <div className={styles.fullView}><AnalyticsDashboard focus={analyticsFocus} /></div>

                        )}

                        {viewMode === 'settings' && (

                            <div className={styles.centeredView}><SettingsPanel /></div>

                        )}

                        {viewMode === 'timeline' && !coreOnly && (

                            <div className={styles.fullView} style={{ display: 'flex', flexDirection: 'column' }}>

                                <div style={{ flex: 1, borderBottom: '1px solid var(--border)' }}>

                                    <PrecisionTimeline

                                        tracks={[

                                            { id: 't1', name: 'Video 1', type: 'video', height: 80, locked: false, muted: false, visible: true, clips: [] },

                                            { id: 't2', name: 'CG Gfx Overlay', type: 'graphics', height: 60, locked: false, muted: false, visible: true, clips: [] },

                                            { id: 't3', name: 'Aux Audio', type: 'audio', height: 60, locked: false, muted: false, visible: true, clips: [] },

                                            { id: 't4', name: 'Switching Events', type: 'graphics', height: 40, locked: false, muted: false, visible: true, clips: [] }

                                        ]}

                                        currentFrame={0}

                                        totalFrames={36000} // 20 mins at 30fps

                                        frameRate={30}

                                        isPlaying={false}

                                    />

                                </div>

                                <div style={{ flex: 1, display: 'flex', background: 'var(--surface-bg)' }}>

                                    <div style={{ flex: 1, display: 'flex' }}>

                                        <ErrorBoundary componentName="MediaLibrary">

                                            <MediaLibrary onItemClick={() => {}} preferredViewMode="grid" />

                                        </ErrorBoundary>

                                    </div>

                                    <div style={{ flex: 1, display: 'flex', borderLeft: '1px solid var(--border)' }}>

                                        <InputManager />

                                    </div>

                                </div>

                            </div>

                        )}

                        {viewMode === 'master' && !coreOnly && (

                            <div className={styles.fullView}><MasterDashboard /></div>

                        )}

                        {viewMode === 'multigrid' && !coreOnly && (

                            <div className={styles.fullView}>

                                <UnifiedScheduler 

                                    obsSessions={obsSessions}

                                    mode="overview" 

                                    viewType="multi"

                                    onAnalyticsFocus={handleFocusAnalytics}

                                    onHourClick={(siteId, day, hour) => {

                                        // Open playlist editor for that hour

                                        persistentLogger.debug('Open hour editor: ' + siteId + ' ' + day + ' ' + hour, undefined, 'PlayoutController');

                                    }}

                                    onHourContextMenu={(e, siteId, day, hour) => {

                                        // Show context menu for hour operations

                                        persistentLogger.debug('Hour context menu: ' + siteId + ' ' + day + ' ' + hour, undefined, 'PlayoutController');

                                    }}

                                />

                            </div>

                        )}

                        {viewMode === 'library' && !coreOnly && (

                            <div className={styles.fullView}>

                                <ErrorBoundary componentName="MediaLibrary">

                                    <MediaLibrary

                                        onItemClick={handleAddMedia}

                                        onItemContextMenu={handleMediaContextMenu}

                                        isPlaying={isPlaying}

                                    />

                                </ErrorBoundary>

                            </div>

                        )}

                        {viewMode === 'tvguide' && !coreOnly && (

                            <div className={styles.fullView}>

                                <TVGuideTickers />

                            </div>

                        )}

                        {viewMode === 'broadcast' && !coreOnly && (

                            <div className={styles.fullView}>

                                <BroadcastControlMapper />

                            </div>

                        )}

                        {viewMode === 'mux' && !coreOnly && (

                            <div className={styles.fullView}>

                                <BroadcastMuxManager />

                            </div>

                        )}

                        {viewMode === 'multichannel' && (

                            <div className={styles.fullView}>

                                <MultiChannelManager />

                            </div>

                        )}

                        {viewMode === 'campaigns' && !coreOnly && (

                            <div className={styles.fullView} style={{ padding: 0, minHeight: 0, overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>

                                <CampaignManagementPage />

                            </div>

                        )}

                        {viewMode === 'social' && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <SocialMediaStudio

                                    onIntegrateWithTicker={(post) => {

                                        persistentLogger.debug('Integrate with ticker: ' + (post?.content ?? post?.id ?? String(post)), undefined, 'PlayoutController');

                                        // Integration point for ticker

                                    }}

                                />

                            </div>

                        )}

                        {viewMode === 'talent' && !coreOnly && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <TalentMonitor

                                    showName={currentMedia?.media?.title || 'Live Broadcast'}

                                    isLive={isPlaying}

                                    currentSegment={currentMedia ? {

                                        id: currentMedia.id,

                                        name: currentMedia.media.title,

                                        type: 'media',

                                        duration: currentMedia.media.duration,

                                        startTime: new Date(currentMedia.startTime || scheduleStartTime)

                                    } : undefined}

                                    upcomingSegments={calculatedPlaylist.slice(calculatedPlaylist.findIndex(p => p.id === activeItemId) + 1, calculatedPlaylist.findIndex(p => p.id === activeItemId) + 6).map(item => ({

                                        id: item.id,

                                        name: item.media.title,

                                        type: 'media' as const,

                                        duration: item.media.duration,

                                        startTime: new Date(item.startTime || Date.now())

                                    }))}

                                />

                            </div>

                        )}

                        {viewMode === 'billing' && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <TrafficBillingDashboard />

                            </div>

                        )}

                        {viewMode === 'filesync' && !coreOnly && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <P2PFileSync />

                            </div>

                        )}

                        {viewMode === 'starfile' && !coreOnly && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <StarfileWorkspace

                                    onImportToLibrary={handleImportStarfileItem}

                                    onSendToPlaylist={handleSendStarfileItemToPlaylist}

                                    onShowAnalytics={handleFocusAnalytics}

                                />

                            </div>

                        )}

                        {viewMode === 'utilities' && (

                            <div className={styles.fullView} style={{ padding: 0 }}>

                                <UtilityMenu />

                            </div>

                        )}

                        {viewMode === 'timeline' && (

                            <div className={styles.fullView} style={{ padding: 0, display: 'flex', flexDirection: 'column' }}>

                                <PrecisionTimeline

                                    tracks={(() => {

                                        const fps = 25;

                                        const videoTrack: TimelineTrack = {

                                            id: 'main_video',

                                            name: 'Video',

                                            type: 'video',

                                            height: 80,

                                            locked: false,

                                            muted: false,

                                            visible: true,

                                            clips: calculatedPlaylist.slice(0, 60).map((item, idx) => {

                                                const startSec = calculatedPlaylist.slice(0, idx).reduce((s, p) => s + (p.media.duration || 0), 0);

                                                return {

                                                    id: item.id,

                                                    trackId: 'main_video',

                                                    type: (item.media.type === 'live' ? 'live' : item.media.type === 'ad' ? 'ad' : 'video') as any,

                                                    title: item.media.title,

                                                    startFrame: Math.round(startSec * fps),

                                                    durationFrames: Math.max(1, Math.round((item.media.duration || 30) * fps)),

                                                    thumbnail: item.media.thumbnail,

                                                    color: item.media.type === 'ad' ? '#f97316' : item.media.type === 'live' ? '#ef4444' : undefined,

                                                };

                                            }),

                                        };

                                        const audioTrack: TimelineTrack = {

                                            id: 'main_audio',

                                            name: 'Audio',

                                            type: 'audio',

                                            height: 60,

                                            locked: false,

                                            muted: false,

                                            visible: true,

                                            clips: videoTrack.clips.map(c => ({

                                                ...c,

                                                id: c.id + '_a',

                                                trackId: 'main_audio',

                                                type: 'audio' as const,

                                            })),

                                        };

                                        return [videoTrack, audioTrack];

                                    })()}

                                    currentFrame={Math.round((activeItemId ? calculatedPlaylist.slice(0, calculatedPlaylist.findIndex(p => p.id === activeItemId)).reduce((s, p) => s + (p.media.duration || 0), 0) : 0) * 25)}

                                    totalFrames={Math.round(calculatedPlaylist.reduce((s, p) => s + (p.media.duration || 0), 0) * 25) || 1}

                                    frameRate={25}

                                    isPlaying={isPlaying}

                                    onPlay={() => { if (!isPlaying) handlePlayPause(); }}

                                    onPause={() => { if (isPlaying) handlePlayPause(); }}

                                    onStop={() => handleStop()}

                                />

                            </div>

                        )}

                    </div>

                </div>

                </div>

            </div>



            {/* Side Panels */}

            {sidePanel !== 'none' && (

                <aside className={`${styles.sidePanel} ${sidePanel === 'router' || sidePanel === 'cgdesigner' || sidePanel === 'obsadvanced' ? styles.wide : ''}`}>

                    {sidePanel === 'obs' && (

                        <div className={styles.sidePanelContent}>

                            <OBSController

                                sessions={obsSessions}

                                onSessionsChange={setObsSessions}

                                onClientsChange={(clients) => {

                                    obsClientsRef.current = clients;

                                }}

                            />

                        </div>

                    )}

                    {sidePanel === 'router' && (

                        <div className={styles.sidePanelContent}>

                            <VideoRouter

                                programMedia={currentMedia}

                                programPlaying={isPlaying}

                                onProgramEnded={handleMediaEnded}

                                cgElements={cgController.elements}

                                cgLayers={cgLayers}

                                initialSeekTime={initialSeekTime}

                            />

                        </div>

                    )}

                    {sidePanel === 'ddr' && (

                        <div className={styles.sidePanelContent}>

                            {ddrAddChannel ? (

                                <div className={styles.ddrPicker}>

                                    <div className={styles.ddrPickerHeader}>

                                        <span>Add to DDR {ddrAddChannel}</span>

                                        <button type="button" className={styles.ddrPickerCancel} onClick={() => setDdrAddChannel(null)}>Cancel</button>

                                    </div>

                                    <div className={styles.ddrPickerList}>

                                        {ddrPickerMedia.length === 0 && <div className={styles.ddrPickerEmpty}>Loading...</div>}

                                        {ddrPickerMedia.map((m) => (

                                            <button

                                                key={m.id}

                                                type="button"

                                                className={styles.ddrPickerItem}

                                                onClick={() => {

                                                    handleSendToDDR(ddrAddChannel, m);

                                                    setDdrAddChannel(null);

                                                }}

                                            >

                                                <Film size={14} />

                                                <span>{m.title}</span>

                                                <span className={styles.ddrPickerDuration}>{Math.floor((m.duration || 0) / 60)}:{String((m.duration || 0) % 60).padStart(2, '0')}</span>

                                            </button>

                                        ))}

                                    </div>

                                </div>

                            ) : (

                                <DDRPanel

                                    channels={ddrChannels}

                                    onChannelChange={handleDDRChannelChange}

                                    onSendToChannel={(channel: DDRChannel, items: PlaylistItem[]) => {

                                        items.forEach((it) => handleSendToDDR(channel, it.media));

                                    }}

                                    onAddMediaRequest={(channel: DDRChannel) => setDdrAddChannel(channel)}

                                />

                            )}

                        </div>

                    )}

                    {sidePanel === 'cg' && (

                        <div className={styles.sidePanelContent}>

                            <CGLivePanel

                                legacyElements={cgController.elements}

                                onOpenAdvancedTools={() => setSidePanel('cgdesigner')}

                                onFocusAnalytics={handleFocusAnalytics}

                            />

                        </div>

                    )}

                    {sidePanel === 'cgdesigner' && (

                        <div className={styles.sidePanelContent}>

                            <CGDesigner />

                        </div>

                    )}

                    {sidePanel === 'obsadvanced' && (

                        <div className={styles.sidePanelContent}>

                            <OBSStudioController />

                        </div>

                    )}

                </aside>

            )}



            {/* Context Menu */}

            {menuState.visible && (

                <ContextMenu

                    x={menuState.x}

                    y={menuState.y}

                    items={menuState.items}

                    onClose={hideMenu}

                    onAction={handleSharedContextMenuAction}

                />

            )}



            {/* CG Builder Modal */}

            {editingCG && (

                <div className={styles.modal}>

                    <div className={styles.modalContent}>

                        <CGBuilder element={editingCG} onSave={handleCGSave} onCancel={() => setEditingCG(null)} />

                    </div>

                </div>

            )}



            {/* Help Panel */}

            <HelpPanel isOpen={helpOpen} onClose={() => setHelpOpen(false)} />

            {logViewerOpen && <LogViewer onClose={() => setLogViewerOpen(false)} />}



            {/* Error Tally Box */}

            <ErrorTallyBox />



            {/* Facebook Story Poster Modal */}

            {showFacebookStoryPoster && (

                <div style={{ position: 'fixed', inset: 0, zIndex: 10000, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'rgba(0, 0, 0, 0.7)', backdropFilter: 'blur(4px)' }}>

                    <FacebookStoryPoster

                        currentProgram={currentMedia?.media?.title}

                        onClose={() => setShowFacebookStoryPoster(false)}

                    />

                </div>

            )}



            {/* Social Campaign Panel Modal */}

            {showSocialCampaignPanel && (

                <SocialCampaignPanel onClose={() => setShowSocialCampaignPanel(false)} />

            )}



            {/* Segment Editor Modal */}

            {editingSegmentMedia && (

                <div style={{ position: 'fixed', inset: 0, zIndex: 10000, background: 'rgba(0, 0, 0, 0.85)', display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '2rem' }}>

                    <div style={{ background: 'var(--surface)', borderRadius: '12px', overflow: 'hidden', display: 'flex', flexDirection: 'column', width: '90vw', height: '90vh', boxShadow: '0 10px 30px rgba(0,0,0,0.5)' }}>

                        <div style={{ padding: '1rem', display: 'flex', justifyContent: 'space-between', alignItems: 'center', background: 'var(--surface-light)', borderBottom: '1px solid var(--border)' }}>

                            <h2 style={{ margin: 0, fontSize: '1.2rem', color: 'var(--text-primary)' }}>Segment Editor: {editingSegmentMedia.title}</h2>

                            <button 

                                onClick={() => {

                                    if (isPlaying) {

                                        if (window.confirm('Are you sure you want to close the segment editor? Playout is active.')) {

                                            setEditingSegmentMedia(null);

                                        }

                                    } else {

                                        setEditingSegmentMedia(null);

                                    }

                                }}

                                style={{ background: 'transparent', border: 'none', color: 'var(--text-muted)', cursor: 'pointer', fontSize: '1.5rem', padding: '0 0.5rem' }}

                            >×</button>

                        </div>

                        <div style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>

                            <ErrorBoundary componentName="SegmentEditor">

                                <SegmentEditor

                                    item={editingSegmentMedia}

                                    onSave={async (collections, activeCollectionId) => {

                                        const updatedMedia: MediaItem = {

                                            ...editingSegmentMedia,

                                            segmentCollections: collections,

                                            activeCollectionId: activeCollectionId,

                                            segments: collections.find(c => c.isActive)?.segments || collections[0]?.segments || []

                                        };

                                        

                                        const localMedia = loadLocalMedia();

                                        const mediaIndex = localMedia.findIndex(m => m.id === updatedMedia.id);

                                        if (mediaIndex >= 0) {

                                            localMedia[mediaIndex] = updatedMedia;

                                            await import('../lib/storage').then(m => m.saveLocalMedia(localMedia));

                                        }

                                        

                                        // Update database immediately

                                        if (typeof (window as any).electronAPI?.mediaSave === 'function') {

                                            try {

                                                await (window as any).electronAPI.mediaSave(updatedMedia);

                                            } catch (e) {

                                                console.warn('[SegmentEditor] mediaSave to DB failed:', e);

                                            }

                                        }



                                        setEditingSegmentMedia(null);

                                    }}

                                    onClose={() => {

                                        if (isPlaying) {

                                            if (window.confirm('Are you sure you want to close the segment editor? Playout is active.')) {

                                                setEditingSegmentMedia(null);

                                            }

                                        } else {

                                            setEditingSegmentMedia(null);

                                        }

                                    }}

                                />

                            </ErrorBoundary>

                        </div>

                    </div>

                </div>

            )}



            {asRunReport && (

                <div style={{ position: 'fixed', inset: 0, zIndex: 10001, background: 'rgba(0,0,0,0.72)', display: 'flex', alignItems: 'center', justifyContent: 'center', padding: '2rem' }}>

                    <div style={{ width: 'min(960px, 96vw)', maxHeight: '88vh', overflow: 'hidden', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 12, display: 'flex', flexDirection: 'column' }}>

                        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '0.75rem 1rem', borderBottom: '1px solid var(--border)' }}>

                            <div>

                                <strong>As-Run Report</strong>

                                <div style={{ fontSize: 12, opacity: 0.8 }}>{asRunReport.media.title}</div>

                            </div>

                            <div style={{ display: 'flex', gap: 6 }}>

                                <button className={styles.controlBtn} onClick={exportAsRunReportCsv} title="Export CSV spreadsheet">

                                    Export CSV

                                </button>

                                <button className={styles.controlBtn} onClick={exportAsRunReportHtml} title="Export HTML report">

                                    Export HTML

                                </button>

                                <button className={styles.controlBtn} onClick={printAsRunReportPdf} title="Print report (Save as PDF)">

                                    Print/PDF

                                </button>

                                <button className={styles.controlBtn} onClick={() => setAsRunReport(null)} title="Close as-run report">Close</button>

                            </div>

                        </div>

                        <div style={{ padding: '0.6rem 1rem', borderBottom: '1px solid var(--border)', fontSize: 12, color: 'var(--text-secondary)' }}>

                            Generated {new Date(asRunReport.generatedAt).toLocaleString()} | Entries: {asRunReport.entries.length}

                        </div>

                        <div style={{ overflow: 'auto', maxHeight: '65vh' }}>

                            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>

                                <thead style={{ position: 'sticky', top: 0, background: 'var(--surface-highlight)' }}>

                                    <tr>

                                        <th style={{ textAlign: 'left', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Source</th>

                                        <th style={{ textAlign: 'left', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Actual Start</th>

                                        <th style={{ textAlign: 'left', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Actual End</th>

                                        <th style={{ textAlign: 'right', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Duration</th>

                                        <th style={{ textAlign: 'left', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Status</th>

                                        <th style={{ textAlign: 'left', padding: '0.5rem', borderBottom: '1px solid var(--border)' }}>Slot / Item</th>

                                    </tr>

                                </thead>

                                <tbody>

                                    {asRunReport.entries.map((entry, index) => (

                                        <tr key={`${entry.source}-${entry.actualStart}-${entry.itemId || index}`} style={{ borderBottom: '1px solid rgba(148,163,184,0.18)' }}>

                                            <td style={{ padding: '0.45rem 0.5rem' }}>{entry.source}</td>

                                            <td style={{ padding: '0.45rem 0.5rem' }}>{entry.actualStart ? new Date(entry.actualStart).toLocaleString() : '-'}</td>

                                            <td style={{ padding: '0.45rem 0.5rem' }}>{entry.actualEnd ? new Date(entry.actualEnd).toLocaleString() : '-'}</td>

                                            <td style={{ padding: '0.45rem 0.5rem', textAlign: 'right' }}>{entry.duration ? `${entry.duration}s` : '-'}</td>

                                            <td style={{ padding: '0.45rem 0.5rem' }}>{entry.status || (entry.completed ? 'COMPLETED' : 'PARTIAL')}</td>

                                            <td style={{ padding: '0.45rem 0.5rem' }}>{entry.hourSlot || entry.itemId || entry.reason || '-'}</td>

                                        </tr>

                                    ))}

                                    {asRunReport.entries.length === 0 && (

                                        <tr>

                                            <td colSpan={6} style={{ padding: '1rem', textAlign: 'center', color: 'var(--text-secondary)' }}>

                                                No as-run entries found for this media item.

                                            </td>

                                        </tr>

                                    )}

                                </tbody>

                            </table>

                        </div>

                    </div>

                </div>

            )}



            <FloatingAIWidget

                onOpenLibrary={() => setViewMode('library')}

                onOpenAnalytics={() => setViewMode('analytics')}

            />

        </div>

    );

}

