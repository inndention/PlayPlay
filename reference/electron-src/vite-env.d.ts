/// <reference types="vite/client" />

interface ImportMetaEnv {
    /** Optional: Settings placeholder when default model field is empty (dev UX only; main uses OPENROUTER_MODEL). */
    readonly VITE_OPENROUTER_MODEL?: string;
    /** Optional: mirror `OPENROUTER_BASE_URL` for renderer-only OpenRouter clients (e.g. UnifiedScheduler) when IPC is unavailable. */
    readonly VITE_OPENROUTER_BASE_URL?: string;
}

interface ImportMeta {
    readonly env: ImportMetaEnv;
}

interface CasparConfigResult {
    found: boolean;
    path?: string;
    port?: number;
    mediaPath?: string;
    videoMode?: string;
    lockPhrase?: string;
    raw?: string;
    error?: string;
}

interface ElectronAPI {
    openOutputWindow: () => Promise<boolean>;
    closeOutputWindow: () => Promise<boolean>;
    toggleOutputFullscreen: () => Promise<boolean>;
    getAppPath: () => Promise<string>;
    getOutputUrl: () => Promise<
        | string
        | {
              local: string;
              external: string;
              port: number;
              /** Host used in external / obsLight / mediaHttpExternal URLs */
              ip: string;
              /** Present when external host is overridden via env */
              lanIpv4?: string;
              /** TCP bind address for output + media HTTP (see PLAYPLAY_OUTPUT_BIND) */
              outputBind?: string;
              /** When true, HTTP server is bound to loopback only — remote LAN clients cannot use external URLs until PLAYPLAY_OUTPUT_BIND is widened. */
              lanHttpClientsBlocked?: boolean;
              externalHostFromEnv?: boolean;
              channelId?: string;
              obsLight?: string;
              obsLightLocal?: string;
              mediaHttpPort?: number;
              mediaHttpLocal?: string;
              mediaHttpExternal?: string;
              backups?: Array<{ n: number; channelId?: string; local: string; external: string }>;
          }
    >;
    openFileDialog: (options?: any) => Promise<{ canceled: boolean; filePaths: string[] }>;
    openFolderDialog: () => Promise<{ canceled: boolean; filePaths: string[] }> | Promise<string[]>;
    showHourFolderList?: (folderPath: string) => Promise<{ ok: boolean; paths?: string[]; error?: string }>;
    showHourWatchRegister?: (payload: { slotId: string; folderPath: string; channelId?: string }) => Promise<{ ok: boolean; error?: string }>;
    showHourWatchUnregister?: (payload: { slotId: string; channelId?: string }) => Promise<{ ok: boolean }>;
    onShowHourWatchFiles?: (callback: (payload: { channelId: string; slotId: string; paths: string[] }) => void) => () => void;
    openExternal: (url: string) => Promise<boolean>;
    readFileAsBuffer: (filePath: string) => Promise<{ success: boolean; dataUrl?: string; mimeType?: string; size?: number; error?: string }>;
    getFileUrl: (filePath: string) => Promise<string>;
    copyFile: (sourcePath: string, destFolder: string) => Promise<{ success: boolean; destPath?: string; filename?: string; error?: string }>;
    copyFilesToCasperCG: (filePaths: string[]) => Promise<{ 
        success: boolean; 
        results: Array<{ 
            sourcePath: string; 
            destPath?: string; 
            filename: string; 
            success: boolean; 
            alreadyExists?: boolean; 
            error?: string 
        }>; 
        casperMediaPath: string 
    }>;
    getCasperCGMediaPath: () => Promise<string>;
    listCasperCGMedia: () => Promise<{ 
        success: boolean; 
        files: Array<{ 
            filename: string; 
            filePath: string; 
            size: number; 
            mtime: string 
        }>; 
        error?: string 
    }>;
    // Media DB
    mediaGetAll: () => Promise<any[]>;
    mediaSave: (item: any) => Promise<boolean>;
    mediaDelete: (id: string) => Promise<boolean>;
    mediaIncrementPlay: (mediaId: string, durationSeconds: number) => Promise<boolean>;
    mediaFindByFilename: (query: string) => Promise<any[]>;
    analyticsLogEvent: (payload: Record<string, unknown>) => Promise<boolean>;
    analyticsGetEvents: (filters?: Record<string, unknown>) => Promise<any[]>;
    analyticsGetSummary: () => Promise<any>;
    starfile?: {
        getStatus: () => Promise<any>;
        start: () => Promise<any>;
        stop: () => Promise<any>;
        restart: () => Promise<any>;
        openExternal: () => Promise<any>;
        listDrives: () => Promise<any[]>;
        listFiles: (requestedPath?: string) => Promise<any[]>;
        search: (query: string) => Promise<any[]>;
        getFileDetails: (filePath: string) => Promise<any>;
        getComments: (filePath: string) => Promise<any[]>;
        addComment: (filePath: string, comment: string) => Promise<any>;
        upload: (directory: string, filePaths: string[]) => Promise<any>;
        download: (filePath: string) => Promise<any>;
        rename: (filePath: string, newName: string) => Promise<any>;
        move: (filePath: string, targetDirectory: string) => Promise<any>;
        delete: (filePath: string) => Promise<any>;
        createDirectory: (parentPath: string, name: string) => Promise<any>;
        getShareUrl: (filePath: string) => Promise<string>;
        openPath: (filePath: string) => Promise<any>;
        revealPath: (filePath: string) => Promise<any>;
    };
    fileHub?: {
        getStatus: () => Promise<any>;
        listFiles: (requestedPath?: string) => Promise<any[]>;
        search: (query: string) => Promise<any[]>;
        upload: (directory: string, filePaths: string[]) => Promise<any>;
        download: (filePath: string) => Promise<any>;
        rename: (filePath: string, newName: string) => Promise<any>;
        move: (filePath: string, targetDirectory: string) => Promise<any>;
        delete: (filePath: string) => Promise<any>;
        share: (filePath: string) => Promise<string>;
        open: (filePath: string) => Promise<any>;
        reveal: (filePath: string) => Promise<any>;
        createDirectory: (parentPath: string, name: string) => Promise<any>;
        logAnalytics: (payload: Record<string, unknown>) => Promise<boolean>;
    };
    // VKM Segment Editor (external)
    openVKM: (filePath?: string) => Promise<{ success: boolean; command?: string; args?: string[]; error?: string }>;
    // Playout sync
    syncPlayoutState: (state: any) => void;
    onPlayoutStateUpdate: (callback: (state: any) => void) => () => void;
    notifyMediaEnded: () => void;
    onOutputMediaEnded: (callback: () => void) => () => void;
    onRequestStateSync: (callback: () => void) => () => void;
    requestPlayoutState: () => void;
    getAutostart: () => Promise<{ enabled: boolean; error?: string }>;
    setAutostart: (enabled: boolean) => Promise<{ ok: boolean; enabled?: boolean; error?: string }>;
    appendPlayoutLog: (entry: Record<string, unknown>) => Promise<{ ok: boolean; error?: string }>;
    readPlayoutLog: (dateStr?: string) => Promise<{ ok: boolean; entries: any[]; error?: string }>;
    getPlayoutLogDir: () => Promise<string>;
    appendEventSummary: (row: { channel?: string; event: string; id?: string; ts?: number }) => Promise<{ ok: boolean; error?: string }>;
    getEventSummaryLogPath: () => Promise<{ ok: boolean; path?: string; maxBytes?: number; error?: string }>;
    // CasparCG playout server
    casparConfigure: (opts: {
        casparDir: string;
        channels: number;
        videoMode?: string;
        casparMediaPath?: string;
        channelConfigs?: unknown;
    }) => Promise<{ ok: boolean; data?: any; error?: string }>;
    /** Persists ramdisk temp prefs for next main-process boot (Electron `temp` path). Restart required. */
    writePlayplayRamdiskTempStub: (payload: {
        enableElectronTempOnRamdisk: boolean;
        root: string;
        electronTempFolder: string;
    }) => Promise<{ ok: boolean; error?: string }>;
    casparStart: (casparDir: string) =>
        Promise<{ ok: boolean; data?: { pid?: number; alreadyRunning?: boolean }; error?: string }>;
    casparStop: () => Promise<{ ok: boolean; data?: any; error?: string }>;
    casparStatus: () => Promise<{
        ok: boolean;
        data: {
            running: boolean;
            pid: number | null;
            casparDir: string | null;
            videoMode?: string;
            seekFps?: number;
            mediaRoot?: string | null;
            allowAbsoluteMediaPaths?: boolean;
            amcp: { host: string; port: number; connected: boolean };
        };
    }>;
    casparPlayPlaylist: (channel: number, items: any[]) => Promise<{
        ok: boolean;
        data?: any;
        error?: string;
        recentAmcp?: string[];
    }>;
    casparStopChannel: (channel: number) => Promise<{ ok: boolean; data?: any; error?: string }>;
    casparClearChannel: (channel: number) => Promise<{ ok: boolean; data?: any; error?: string }>;
    casparRaw: (command: string) => Promise<{ ok: boolean; data?: any; error?: string }>;

    /** SVT Bridge companion (Caspar-focused UI) */
    bridgeStatus: () => Promise<{
        ok: boolean;
        data?: { running: boolean; pid: number | null; installDir: string | null; exeExists: boolean };
        error?: string;
    }>;
    bridgeStart: (installDir?: string) => Promise<{ ok: boolean; data?: any; error?: string }>;
    bridgeStop: () => Promise<{ ok: boolean; data?: any; error?: string }>;
    bridgeSetInstallDir: (dir: string) => Promise<{ ok: boolean; data?: any; error?: string }>;
    bridgeRevealInstall: () => Promise<{ ok: boolean; error?: string }>;

    // Channel engines (per-channel Node processes, OBS browser sources)
    channelEngineSpawn: (opts: { channelId: string; channelName?: string; freshState?: boolean }) =>
        Promise<{ ok: boolean; data?: { channelId: string; channelName: string; port: number; url: string; pid: number; status: string; dataDir: string }; error?: string }>;
    channelEngineKill: (channelId: string) => Promise<{ ok: boolean }>;
    channelEngineList: () => Promise<{ ok: boolean; data: Array<{ channelId: string; channelName: string; port: number; url: string; pid: number; status: string }> }>;
    channelEnginePushPlaylist: (channelId: string, playlist: any[], autoplay?: boolean) =>
        Promise<{ ok: boolean; data?: any; error?: string }>;
    channelEngineTransport: (channelId: string, action: 'start' | 'stop' | 'skip') =>
        Promise<{ ok: boolean; data?: any; error?: string }>;
    onPlayoutCommand: (callback: (data: { action?: string }) => void) => () => void;
    onAppMenuAction: (callback: (action: string) => void) => () => void;
    // CasperCG
    sendAMCPCommand: (host: string, port: number, command: string) => Promise<string>;
    discoverLocalSources: () => Promise<{ ok: boolean; ndi: string[]; devices: string[]; error?: string }>;
    readCasparConfig: () => Promise<CasparConfigResult>;
    // Schedule DB
    scheduleGetMeta: () => Promise<Record<string, string>>;
    scheduleSetMeta: (key: string, value: string) => Promise<boolean>;
    scheduleGetAllSlots: () => Promise<any[]>;
    scheduleSaveSlot: (slot: any) => Promise<boolean>;
    scheduleSaveWeekly: (payload: { schedule: any; meta?: Record<string, string | number> }) => Promise<{ ok: boolean; error?: string }>;
    scheduleGetAllPlaylists: () => Promise<any[]>;
    scheduleSavePlaylist: (playlist: any) => Promise<boolean>;
    scheduleDeletePlaylist: (id: string) => Promise<boolean>;
    // CG Templates
    listCGTemplates: () => Promise<{ success: boolean; templates: Array<{ id: string; name: string; category: string; filePath: string; parameters: Array<{ name: string; type: string; label: string; defaultValue?: string }> }>; error?: string }>;
    getCGTemplate: (templateId: string) => Promise<{ success: boolean; content?: string; filePath?: string; error?: string }>;
    openCGTemplateFolder: () => Promise<{ success: boolean; path?: string; error?: string }>;
    importCGTemplateFolder: () => Promise<{ success: boolean; copied?: number; dest?: string; cancelled?: boolean; error?: string }>;
    // File checks / recovery
    pathExists: (filePath: string) => Promise<{ exists: boolean; error?: string }>;
    // URL Info
    getAllUrls: () => Promise<{
        port: number;
        ip: string;
        outputBind?: string;
        urls: Record<string, string>;
    }>;
    // External Playout Engines
    getEnginePaths: () => Promise<EnginePaths>;
    checkEngineAvailability: () => Promise<EngineAvailability>;
    // FFmpeg
    ffmpegStart: (config: FFmpegStartConfig) => Promise<EngineStartResult>;
    ffprobeInfo: (filePath: string) => Promise<{ success: boolean; info?: FFprobeInfo; error?: string }>;
    // FFplayout
    ffplayoutStart: (config: FFplayoutStartConfig) => Promise<EngineStartResult>;
    // GStreamer
    gstreamerStart: (config: GStreamerStartConfig) => Promise<EngineStartResult>;
    gstreamerCheckPlugins: (pluginNames: string[]) => Promise<{ success: boolean; plugins: Record<string, { available: boolean; info?: string; error?: string }>; error?: string }>;
    gstreamerValidate: (pipeline: string) => Promise<{ success: boolean; valid: boolean; error?: string; details?: { stdout: string; stderr: string } }>;
    // NodeCG
    nodecgStart: (config: NodeCGStartConfig) => Promise<EngineStartResult>;
    // MPV
    mpvStart: (config: MPVStartConfig) => Promise<EngineStartResult & { ipcPath?: string }>;
    mpvSendCommand: (ipcPath: string, command: any) => Promise<any>;
    // DekTec
    dektecEnumerate: () => Promise<{ success: boolean; devices: DekTecDeviceInfo[]; error?: string }>;
    dektecPlay: (config: DekTecPlayConfig) => Promise<EngineStartResult>;
    // Engine Management
    engineStop: (key: string) => Promise<{ success: boolean; error?: string }>;
    engineList: () => Promise<RunningEngine[]>;
    // Engine Events
    onEngineOutput: (callback: (data: EngineOutputEvent) => void) => () => void;
    onEngineStopped: (callback: (data: EngineStoppedEvent) => void) => () => void;
    /** OpenRouter and bundled AI CLIs — requests run in main (no renderer CORS). */
    ai?: {
        status: () => Promise<unknown>;
        configure: (opts: unknown) => Promise<unknown>;
        send: (providerId: string, prompt: string, opts?: unknown) => Promise<unknown>;
        broadcast: (prompt: string, opts?: unknown) => Promise<unknown>;
        openRouterJson: (payload: Record<string, unknown>) => Promise<{
            success: boolean;
            error?: string;
            parsed?: unknown;
            content?: string;
            model?: string;
        }>;
        /** Base URL and provider id from Electron main (no secrets). */
        openRouterConfig: () => Promise<{ baseUrl: string; provider: string }>;
    };
    hermesSupervisor?: {
        getEnvDefault: () => Promise<{ fromEnv?: boolean }>;
        appendLearning: (line: string) => Promise<unknown>;
    };
    // Platform
    platform: string;
    isElectron: boolean;
}

// Engine Types
interface EnginePaths {
    ffmpeg: string;
    ffprobe: string;
    ffplay: string;
    ffplayout: string;
    gstreamer: string;
    gstInspect: string;
    dtapiDll: string;
    dtPlay: string;
    dtRecord: string;
    dtInfo: string;
}

interface EngineAvailability {
    ffmpeg: { available: boolean; path: string; version: string | null };
    ffplayout: { available: boolean; path: string; version: string | null };
    gstreamer: { available: boolean; path: string; version: string | null };
    dektec: { available: boolean; path: string; devices: DekTecDeviceInfo[] };
    nodecg: { available: boolean; path: string; version: string | null };
}

interface FFmpegStartConfig {
    id?: string;
    inputType?: 'pipe' | 'file';
    inputFile?: string;
    videoCodec?: string;
    audioCodec?: string;
    videoBitrate?: string;
    audioBitrate?: string;
    resolution?: string;
    framerate?: number;
    preset?: string;
    outputFormat?: string;
    outputUrl: string;
    noAudio?: boolean;
    additionalArgs?: string[];
}

interface FFplayoutStartConfig extends Partial<FFplayoutConfig> {
    id?: string;
}

interface NodeCGStartConfig {
    id?: string;
    cwd?: string;
    port?: number;
    bundlesPath?: string;
}

interface GStreamerStartConfig {
    id?: string;
    pipeline: string;
}

interface DekTecPlayConfig {
    id?: string;
    serial?: number;
    port?: number;
    bitrate?: number;
    inputFile?: string;
    loop?: boolean;
}

interface DekTecDeviceInfo {
    deviceName: string;
    serial: number;
    description: string;
    ports: Array<{ port: number; description: string }>;
}

interface EngineStartResult {
    success: boolean;
    key?: string;
    pid?: number;
    error?: string;
}

interface RunningEngine {
    key: string;
    type: 'ffmpeg' | 'ffplayout' | 'gstreamer' | 'dektec';
    pid: number;
    config: any;
}

interface EngineOutputEvent {
    key: string;
    type: 'ffmpeg' | 'ffplayout' | 'gstreamer' | 'dektec';
    data: string;
}

interface EngineStoppedEvent {
    key: string;
    type: 'ffmpeg' | 'ffplayout' | 'gstreamer' | 'dektec';
    code: number;
}

interface FFprobeInfo {
    format: {
        filename: string;
        nb_streams: number;
        format_name: string;
        format_long_name: string;
        start_time: string;
        duration: string;
        size: string;
        bit_rate: string;
        tags?: Record<string, string>;
    };
    streams: Array<{
        index: number;
        codec_name: string;
        codec_long_name: string;
        codec_type: 'video' | 'audio' | 'subtitle';
        width?: number;
        height?: number;
        coded_width?: number;
        coded_height?: number;
        pix_fmt?: string;
        r_frame_rate?: string;
        avg_frame_rate?: string;
        sample_rate?: string;
        channels?: number;
        channel_layout?: string;
        duration?: string;
        bit_rate?: string;
    }>;
}

declare global {
    interface Window {
        electronAPI?: ElectronAPI;
    }
}
