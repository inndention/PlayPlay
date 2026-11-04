// ============================================
// PLAY PLAY - Complete Broadcast Automation Suite
// Type Definitions
// ============================================

// --- MEDIA TYPES ---
export type MediaType =
    | 'video'
    | 'image'
    | 'live'
    | 'station_id'
    | 'bumper'
    | 'program'
    | 'paid_program'
    | 'ad'
    | 'promo'
    | 'gap';

export interface STTRecord {
    id: string;
    provider: 'local-whisper' | 'openai-whisper' | 'manual' | 'unknown';
    transcript: string;
    language?: string;
    confidence?: number;
    startedAt?: number;
    endedAt?: number;
    createdAt: string;
    source?: 'import' | 'as-played' | 'live' | 'operator';
}

export interface MediaContentIndex {
    summary?: string;
    topics: string[];
    people: string[];
    places: string[];
    clients: string[];
    events: string[];
    sponsorCandidates: string[];
    programCandidates: string[];
    contextTags: string[];
    lastIndexedAt: string;
}

// Bumper subtypes for more granular categorization
export type BumperSubtype = 'generic' | 'breaking' | 'program';

// Keywords that indicate station ID or promo content
export const ID_PROMO_KEYWORDS = ['id', 'star news', 'wgsr', 'wmdv', 'wyat', 'promo', 'legal', 'station', 'network'] as const;

// --- MEDIA ITEM ---
export interface MediaItem {
    id: string;
    title: string;
    duration: number; // in seconds
    src: string;
    type: MediaType;
    thumbnail?: string;
    // File path (for Electron persistence)
    filePath?: string; // Full file path when imported in Electron
    filename?: string; // Extracted filename for search/indexing
    // Browser storage
    storedFileId?: string; // ID for IndexedDB storage
    fileHandleId?: string; // For File System Access API (stored as string in localStorage)
    // Metadata
    client?: string; // For Ads - client ID
    programSeries?: string; // For Programs
    episodeDate?: string; // ISO Date for sorting
    notes?: string;
    segments?: Segment[]; // Legacy - use segmentCollections instead
    segmentCollections?: SegmentCollection[]; // Multiple segment collections
    activeCollectionId?: string; // Currently active collection ID
    // Virtual clip support
    inPoint?: number; // Start time in seconds (for virtual clips)
    outPoint?: number; // End time in seconds (for virtual clips)
    // Live sources (NDI / capture / OBS / custom GStreamer)
    liveSource?: LiveSourceConfig;
    // Tracking
    stats?: MediaStats;
    // Library management
    addedAt?: string; // ISO date when added to library
    dateMade?: string; // ISO date when media was created/produced
    lastPlayedAt?: string; // ISO date of last play
    playCount?: number; // Number of times played
    priority?: number; // Priority for scheduling (-10 to 10, 0 = normal)
    /** Library "heat" score (0–100), persisted; decays over time unless frozen. */
    priorityScore?: number;
    /** When true, `priorityScore` does not decay until unfrozen. */
    priorityFrozen?: boolean;
    /** Optional per-item multiplier on global burn rate (default 1). */
    priorityBurnRateMultiplier?: number;
    /** ISO time when `priorityScore` was last committed (decay anchor). */
    priorityUpdatedAt?: string;
    archived?: boolean; // If true, hidden from library but kept in playlists
    availableStart?: string; // ISO date/time when this media may begin airing
    availableEnd?: string; // ISO date/time when this media expires
    paused?: boolean; // If true, schedule/autofill should skip or replace it
    pauseReplacementType?: MediaType; // Preferred similar type when paused
    clientGroupId?: string; // Optional traffic/client group for rotating sets
    applicableSiteIds?: string[]; // Playout sites where this asset is preferred/allowed
    regionTags?: string[]; // Region tags for traffic and smart-fill matching
    mediaClass?: 'program' | 'ad' | 'bumper' | 'promo' | 'seasonal' | 'station_id' | 'filler' | 'unknown';
    importSourceFormat?: string; // Original extension/container noted during import, e.g. mpg-as-mp4
    detectedMediaType?: MediaType; // Type guessed during preimport/duration scan
    detectionNotes?: string[];
    // Asset management
    adMetadata?: AdMetadata;
    // Bumper subtype (only for type='bumper')
    bumperSubtype?: BumperSubtype;
    // Program association (for bumper subtype='program')
    bumperProgramId?: string;
    transcriptRecords?: STTRecord[];
    contentIndex?: MediaContentIndex;
    /** Default Caspar hints for playlist rows built from this asset (rows may override via `PlaylistItem.caspar`). */
    caspar?: PlaylistCasparHints;
}

export type LiveSourceKind = 'gstreamer' | 'ndi' | 'capture' | 'url' | 'obs_scene';

export interface LiveSourceConfig {
    kind: LiveSourceKind;
    // User label / identifier (NDI name, device name, URL, OBS scene)
    name?: string;
    // NDI
    ndiName?: string;
    // Capture device (platform-specific; user-provided for now)
    device?: string;
    // URL (SRT/RTSP/HTTP/UDP etc) if you want to treat as live
    url?: string;
    // OBS fallback
    obsScene?: string;
    // GStreamer pipeline override (preferred)
    // Use tokens: INPUT / ${INPUT} / ${NDI} / ${URL} / ${DEVICE}
    pipeline?: string;
}

export interface Segment {
    id: string;
    label: string;
    startTime: number;
    endTime: number;
    color?: string;
}

// --- PLAY TRACKING ---
export interface PlayEvent {
    timestamp: number;      // Unix timestamp when play started
    duration: number;       // Actual duration played (seconds)
    completed: boolean;     // Did it play to completion?
    hourSlot?: string;      // Which hour slot it was in (e.g., "3-14" = Wednesday 2PM)
    itemId?: string;        // Playlist item id
    mediaTitle?: string;    // Title at time of play
    scheduledStart?: number; // Planned start (ms)
    actualStart?: number;    // Actual start (ms)
    actualEnd?: number;      // Actual end (ms)
    playlistIndex?: number;  // Position in hour
    source?: string;         // playout/scheduler/etc
}

export interface MediaStats {
    playCount: number;
    totalPlayTime: number;  // Total seconds played
    lastPlayed?: number;    // Unix timestamp
    playHistory: PlayEvent[];
}

export interface AsPlayedEntry extends PlayEvent {
    mediaId: string;
}

// --- CLIENT MANAGEMENT ---
export interface AdClient {
    id: string;
    name: string;
    color: string;          // Hex color for UI

    // Contact
    contactName?: string;
    contactEmail?: string;
    contactPhone?: string;
    address?: string;
    website?: string;

    // Contract / Billing
    contractStart?: string;
    contractEnd?: string;
    budget?: number;
    ratePerPlay?: number;
    billingCode?: string;
    terms?: string;

    // Scheduling rules (defaults for this client)
    rules: SchedulingRule;

    // Status
    status?: 'active' | 'paused' | 'cancelled';

    notes?: string;
    createdAt?: string;
    updatedAt?: string;
    applicableSiteIds?: string[]; // Regions/sites where this client inventory should be favored
    regionTags?: string[];
}

// --- CAMPAIGN MANAGEMENT ---
export interface Campaign {
    id: string;
    name: string;
    client: string;
    startDate: string;      // ISO date string
    endDate: string;        // ISO date string
    targetPlays: number;
    currentPlays: number;
    billingCode?: string;
    daypartRestrictions?: string[]; // e.g., ['morning', 'primetime']
    separationMinutes?: number; // min time between plays
    competitiveExclusions?: string[]; // client IDs to not air near
}

export interface AdMetadata {
    campaignId?: string;
    client?: string;
    isRotator?: boolean;
    weight?: number; // for probabilistic scheduling
}

export interface SchedulingRule {
    allowedHours: number[];     // 0-23, empty = all hours
    allowedDays: number[];      // 0-6 (Sun-Sat), empty = all days
    maxPlaysPerHour: number;    // 0 = unlimited
    maxPlaysPerDay: number;     // 0 = unlimited
    minIntervalMinutes: number; // Minimum gap between plays
    priority: number;           // Higher = more likely to be picked
}

// --- SMART SLOTS ---
export interface SmartSlotConfig {
    type: 'latest' | 'previous' | 'specific_date' | 'random';
    seriesName?: string;
    targetDate?: string;
    clientId?: string;      // For ad rotation
}

/** Second argument for MediaLibrary `onItemClick`: smart-slot resolution or command-tab metadata */
export type MediaLibraryItemClickOptions = SmartSlotConfig | { isCommand?: boolean; command?: unknown };

export function isSmartSlotConfig(o: unknown): o is SmartSlotConfig {
    if (!o || typeof o !== 'object') return false;
    const t = (o as { type?: unknown }).type;
    return t === 'latest' || t === 'previous' || t === 'specific_date' || t === 'random';
}

/** Caspar LOADBG hints aligned with SVT Bridge `caspar` item fields (subset). */
export interface PlaylistCasparHints {
    loop?: boolean;
    transitionName?: 'Cut' | 'Mix' | 'Push' | 'Slide' | 'Wipe' | number;
    transitionDuration?: number | string;
    transitionEasing?: string;
    transitionDirection?: 'Left' | 'Right' | number;
    /** Letterbox/pillarbox 9:16 (or other) inside 16:9 channel — use `Fit`; omit for default producer behavior. */
    scaleMode?: 'Stretch' | 'Fit' | 'Fill' | 'Original' | 'HFILL' | 'VFILL' | number;
}

// --- PLAYLIST ---
export interface PlaylistItem {
    id: string; // unique instance ID
    mediaId: string;
    media: MediaItem; // Resolved media info
    startTime?: number; // Calculated timestamp (ms)
    endTime?: number;
    isFixedTime?: boolean; // If true, this item MUST start at specific time
    fixedTime?: number;
    smartConfig?: SmartSlotConfig;
    isResolved?: boolean;
    // Time in/out (in seconds, relative to media start)
    timeIn?: number;  // Start playback at this time (seconds)
    timeOut?: number; // End playback at this time (seconds)
    // CG association
    cgOnStart?: string[];   // CG element IDs to trigger on start
    cgOnEnd?: string[];     // CG element IDs to trigger on end
    // OBS commands
    obsCommands?: string[]; // OBS command IDs to execute when this item plays
    // Automation actions (Facebook, etc.)
    actions?: AutomationAction[];
    parentMediaId?: string; // Segment/clip parent item, if this playlist item is a derived segment
    segmentId?: string; // Visual tag and parent follow-through for segment-derived playlist items
    segmentLabel?: string;
    /** Caspar / Bridge-style LOADBG options for this row (overrides `media.caspar` when both set). */
    caspar?: PlaylistCasparHints;
}

export type AutomationActionType =
    | 'facebook_post'
    | 'facebook_live_start'
    | 'facebook_live_stop';

export interface AutomationAction {
    id: string;
    type: AutomationActionType;
    createdAt: number;
    enabled?: boolean;
    // Facebook
    message?: string;
    link?: string;
    // Webhook extras (free-form JSON as string, optional)
    webhookData?: string;
}

// --- WEEKLY SCHEDULE ---
export interface HourSlot {
    id: string;             // Format: "D-H" (e.g., "3-14" = Wednesday 2PM) or "YYYY-MM-DD-H"
    dayOfWeek?: number;     // 0-6 (Sunday = 0), optional if date-based
    date?: string;          // ISO Date string "YYYY-MM-DD", optional if template-based
    hour: number;           // 0-23
    items: PlaylistItem[];
    isLooping: boolean;     // Repeat content if short
    randomizeAds: boolean;  // Shuffle ad order
    keepProgramOrder: boolean; // Lock program sequence
    blockType?: 'general' | 'show' | 'news' | 'sports' | 'paid_program' | 'maintenance';
    showBlock?: DedicatedShowBlock;
    updatedAt?: number;     // Unix timestamp of last content edit
    /**
     * Schedule builder “show hour”: optional template slot for rotating typical programming.
     * New programs (drag/drop or watch folder) keep normal `MediaItem.priority`; existing
     * program-like rows in the hour are demoted to low priority when new content is merged in.
     */
    showHour?: {
        enabled: boolean;
        /** Electron: top-level folder watched with fs.watch (non-recursive ingest). */
        watchFolderPath?: string;
    };
}

export interface DedicatedShowBlock {
    id: string;
    title: string;
    seriesName?: string;
    locked?: boolean;
    autofillTemplateId?: string;
    notes?: string;
}

export interface WeeklySchedule {
    slots: Record<string, HourSlot>; // Keyed by "D-H" or "YYYY-MM-DD-H"
    version: number;        // For change tracking
    lastModified: number;   // Unix timestamp
    startDate?: string;     // If this is a specific week, the Sunday or Monday start date
}

// --- FREE PLAYLIST ---
export interface FreePlaylist {
    id: string;
    name: string;
    items: PlaylistItem[];
    isLocked: boolean;      // If false, can play anytime (not time-bound)
    createdAt: number;
    createdFrom?: string;   // Source hour slot ID if exported
}

// --- CG SYSTEM ---
export type CGType = 'lower_third' | 'ticker' | 'bug' | 'clock' | 'weather' | 'custom';

export type CGAnimationIn = 'none' | 'fade' | 'slide_left' | 'slide_right' | 'slide_up' | 'slide_down' | 'zoom' | 'bounce';
export type CGAnimationOut = 'none' | 'fade' | 'slide_left' | 'slide_right' | 'slide_up' | 'slide_down' | 'zoom';
export type CGLoop = 'none' | 'pulse' | 'sheen' | 'shimmer' | 'glow_pulse' | 'ticker_scroll' | 'bounce';

export interface CGColors {
    primary: string;        // Hex
    secondary: string;      // Hex
    text: string;           // Hex
    gradient: boolean;
    gradientDirection?: 'horizontal' | 'vertical' | 'diagonal';
}

export interface CGEffects {
    shadow: boolean;
    shadowColor?: string;
    shadowBlur?: number;
    glow: boolean;
    glowColor?: string;
    glowIntensity?: number;
    borderRadius?: number;
    opacity?: number;
    glassMorphism?: boolean; // Glass morphism effect
}

export interface CGPosition {
    x: number;              // Percentage from left (0-100)
    y: number;              // Percentage from top (0-100)
    width: number;          // Percentage of screen width
    height: number;         // Percentage of screen height
    anchor: 'top-left' | 'top-center' | 'top-right' | 'center-left' | 'center' | 'center-right' | 'bottom-left' | 'bottom-center' | 'bottom-right';
}

export interface CGElement {
    id: string;
    name: string;
    type: CGType;
    // Content
    title: string;
    subtitle?: string;
    tickerText?: string;    // For ticker type
    imageSrc?: string;      // For bug type
    // Styling
    colors: CGColors;
    effects: CGEffects;
    position: CGPosition;
    fontSize?: number;
    fontFamily?: string;
    // Animation
    animationIn: CGAnimationIn;
    animationOut: CGAnimationOut;
    animationDuration: number; // ms
    loop: CGLoop;
    loopDuration?: number;  // ms per loop cycle
    // State
    isVisible: boolean;
    autoHideAfter?: number; // ms, 0 = manual hide
    // Metadata
    isPreset: boolean;
    presetCategory?: string;
}

export interface CGPreset {
    id: string;
    name: string;
    category: string;       // e.g., "Lower Thirds", "Bugs", "Tickers"
    element: Omit<CGElement, 'id' | 'isVisible'>;
    thumbnail?: string;
}

// --- CG TEMPLATE SYSTEM ---
export interface CGParameter {
    name: string;
    type: 'text' | 'color' | 'number' | 'select' | 'url';
    label: string;
    defaultValue?: string;
    options?: string[]; // For select type
}

export interface CGTemplate {
    id: string;
    name: string;
    category: 'lower_third' | 'ticker' | 'bug' | 'fullscreen' | 'social' | 'weather' | 'banner';
    filePath: string;
    parameters: CGParameter[];
    thumbnail?: string;
}

export interface CGTemplatePreset {
    id: string;
    name: string;
    templateId: string;
    values: Record<string, string>;
    createdAt: string;
}

export interface CGHistoryItem {
    id: string;
    templateId: string;
    values: Record<string, string>;
    usedAt: string;
}

export interface CGScheduledEvent {
    id: string;
    templateId: string;
    templateName?: string;
    values: Record<string, string>;
    layer: number;
    channel: CGChannel;
    startTime: string;
    endTime?: string;
    durationSeconds?: number;
    enabled: boolean;
    status?: 'scheduled' | 'armed' | 'played' | 'skipped' | 'cancelled';
    createdAt: string;
    updatedAt?: string;
    notes?: string;
}

// --- CG LIVE WORKFLOW ---
export type CGChannel = 'preview' | 'program';
export type CGTransition = 'cut' | 'fade' | 'wipe_left' | 'wipe_right' | 'push_up' | 'push_down';
export type CGAnchorPreset =
    | 'top-left'
    | 'top-center'
    | 'top-right'
    | 'bottom-left'
    | 'bottom-center'
    | 'bottom-right'
    | 'lower-third'
    | 'top-ticker'
    | 'bottom-ticker'
    | 'bug-top-left'
    | 'bug-top-right'
    | 'bug-bottom-left'
    | 'bug-bottom-right';

export interface UnifiedCGCoreFields {
    text1: string;
    text2: string;
    text3: string;
    style1: string;
    style2: string;
    style3: string;
    animationIn: string;
    animationLoop: string;
    animationOut: string;
    anchorPreset: CGAnchorPreset;
    margin: number;
    width: number;
    height: number;
}

export interface UnifiedCGFieldDefinition {
    key: string;
    label: string;
    type: 'text' | 'textarea' | 'number' | 'color' | 'select' | 'checkbox' | 'url' | 'datetime';
    defaultValue?: string;
    options?: string[];
}

export interface UnifiedCGSourceTemplate {
    templateId: string;
    templateName: string;
    category: string;
    defaults: Record<string, string>;
    fields: UnifiedCGFieldDefinition[];
    fieldMap?: Partial<Record<'text1' | 'text2' | 'text3' | 'style1' | 'style2' | 'style3', string>>;
}

export interface UnifiedCGLayerState {
    id: string;
    type: 'element' | 'template';
    element?: CGElement;
    templateId?: string;
    templateUrl?: string;
    values?: Record<string, string>;
    isVisible: boolean;
    transition: CGTransition;
    transitionDuration: number;
    layer: number;
    displayName?: string;
    templateCategory?: string;
    channel?: CGChannel;
    core?: UnifiedCGCoreFields;
    advancedValues?: Record<string, string>;
    fieldDefinitions?: UnifiedCGFieldDefinition[];
    sourceElementId?: string;
}

export interface UnifiedCGStateSnapshot {
    previewLayers: UnifiedCGLayerState[];
    programLayers: UnifiedCGLayerState[];
    selectedLayer: number;
    selectedLayerId?: string;
    selectedTemplate?: UnifiedCGSourceTemplate | null;
}

// --- PLAYOUT ENGINE APPROACHES (optional transport / output paths) ---
/** Ramdisk / fast-drive roots for play engines (Windows e.g. ImDisk `B:\\`). */
export interface RamdiskEngineConfig {
    /** Drive or folder root, e.g. `B:\\` or `B:/PlayPlayRAM` (slashes normalized in code). */
    root: string;
    /** Subfolder under `root` used for Caspar `<media-path>` when enabled. */
    casparMediaFolder: string;
    /** Rewrite `casparcg.config` `<media-path>` on next `caspar:configure` to `root/casparMediaFolder`. */
    enableCasparMediaOnRamdisk: boolean;
    /** Subfolder under `root` for Electron `app.setPath('temp', …)` (next launch after flag is written). */
    electronTempFolder: string;
    /**
     * When true, renderer writes `userData/playplay_ramdisk_temp.json`; main reads it **before**
     * `app.whenReady()` on the **next** launch and applies `app.setPath('temp', …)`.
     */
    enableElectronTempOnRamdisk: boolean;
}

/** Reviewed playout paths that exist in this codebase; all default to current on-air behavior. */
export interface EngineApproachesConfig {
    /** Route play/stop/pause and transport state to CasparCG when it is running (AMCP path). */
    casparTransport: boolean;
    /** On launch, probe Caspar dir, write config, and start Caspar if needed. */
    casparAutoconfigureOnLaunch: boolean;
    /** At each :00, push the hour grid to Caspar (requires Caspar + schedule). */
    casparHourAutoPush: boolean;
    /** When Caspar starts or restarts, boss-clock push current hour into Caspar. */
    casparStartupCatchup: boolean;
    /** Prefer MPV broadcast IPC over built-in HTML5 in PlayoutEngineManager (Electron only). */
    mpvBroadcastEngine: boolean;
    /** If MPV fails to start, fall back to built-in HTML5. */
    mpvFallbackToElectron: boolean;
    /** NVMe/RAM disk paths for Caspar media + Electron scratch (merged from defaults when missing in storage). */
    ramdisk: RamdiskEngineConfig;
}

// --- SETTINGS ---
export interface AutomationSettings {
    // Program settings
    alwaysPlayLatestEpisode: boolean;
    shuffleAdsInHour: boolean;
    // Fill settings
    fillGapsWithIds: boolean;
    fillGapsWithBumpers: boolean;
    maxFillDuration: number;    // seconds (default 60)
    // Playback
    autoAdvance: boolean;
    loopAtEndOfSchedule: boolean;
    // CG defaults
    defaultCGAnimationIn: CGAnimationIn;
    defaultCGAnimationOut: CGAnimationOut;
    defaultCGDuration: number;
    // Analytics
    trackPlayHistory: boolean;
    maxHistoryDays: number;

    // Social / Facebook automation
    facebookWebhookUrl?: string;          // Optional: call your own webhook
    facebookWebhookEnabled?: boolean;     // Default true when url set
    facebookGraphEnabled?: boolean;       // Optional Graph API mode
    facebookGraphPageId?: string;
    facebookGraphAccessToken?: string;    // Page access token
    facebookPreferGraph?: boolean;        // If both available, use Graph first
    openaiApiKey?: string;                // OpenAI API key for AI-assisted content
    ttsProvider?: 'auto' | 'kokoro-local' | 'starnews' | 'openai';
    kokoroTtsBaseUrl?: string;            // Local Kokoro/OpenAI-compatible speech endpoint
    kokoroVoice?: string;                 // Preferred local voice, e.g. af_heart
    sttProvider?: 'auto' | 'local-whisper' | 'openai-whisper';
    localWhisperBaseUrl?: string;         // Local OpenAI-compatible transcription endpoint
    youtubeAccessToken?: string;          // YouTube OAuth2 access token
    salesReportingEndpoint?: string;      // Sales reporting endpoint URL
    ttsAnnouncementsEnabled?: boolean;
    ttsAnnounceHourlyTime?: boolean;
    ttsWarnNoContent3Min?: boolean;
    ttsAnnounceQueuedStarts?: boolean;

    /** Optional toggles for Caspar / Electron / MPV playout paths (see EngineApproachesConfig). */
    engineApproaches?: EngineApproachesConfig;
}

// --- APP STATE ---
export interface AppState {
    weeklySchedule: WeeklySchedule;
    freePlaylists: FreePlaylist[];
    clients: AdClient[];
    cgElements: CGElement[];
    cgPresets: CGPreset[];
    settings: AutomationSettings;
    version: number;
}

// ============================================
// ADVANCED FEATURES
// ============================================

// --- OBS WEBSOCKET (for browser source control, CG overlays) ---
// --- BUILT-IN PLAYOUT ENGINE ---
export interface PlayoutEngineConfig {
    id: string;
    name: string;
    type: 'internal'; // Built-in HTML5 player is the only supported engine
    enabled: boolean;
    // Hardware acceleration settings for built-in player
    hwAccel?: {
        enabled: boolean;
        decoder: 'auto' | 'nvdec' | 'd3d11va' | 'vaapi' | 'videotoolbox' | 'none';
        upscaling?: 'none' | 'lanczos' | 'bilinear' | 'bicubic' | 'ai_super_resolution';
        frameInterpolation?: 'none' | 'motion_blur' | 'frame_blending' | 'optical_flow';
        deblocking?: 'none' | 'light' | 'medium' | 'heavy';
    };
}

// --- FFMPEG ENGINE ---
export interface FFmpegConfig {
    outputType: 'rtmp' | 'srt' | 'udp' | 'file' | 'hls' | 'decklink' | 'pipe' | 'dektec_asi' | 'dektec_sdi';
    outputUrl: string;
    videoCodec: string; // libx264, libx265, h264_nvenc, h265_nvenc, etc.
    audioCodec: string; // aac, mp3, ac3, eac3, opus, etc.
    videoBitrate: string; // e.g., '5M'
    audioBitrate: string; // e.g., '192k'
    resolution: string; // e.g., '1920x1080'
    framerate: number;
    preset: string; // ultrafast, superfast, veryfast, fast, medium, slow, etc.
    
    // Broadcast-grade settings
    gopSize?: number; // Group of Pictures size (frames)
    bFrames?: number; // Number of B-frames
    profile?: string; // H.264 profile: baseline, main, high, high422, high444
    level?: string; // H.264 level: 3.0, 3.1, 4.0, 4.1, etc.
    colorSpace?: string; // bt709, bt2020, smpte170m
    colorRange?: 'tv' | 'pc'; // TV range (16-235) or PC range (0-255)
    pixelFormat?: string; // yuv420p, yuv422p, yuv444p, etc.
    audioChannels?: number; // 1 (mono), 2 (stereo), 6 (5.1), 8 (7.1)
    audioSampleRate?: number; // 44100, 48000, 96000, etc.
    
    // Advanced encoding options
    crf?: number; // Constant Rate Factor (0-51, lower = better quality)
    maxBitrate?: string; // Maximum bitrate (for VBR)
    bufsize?: string; // Buffer size
    keyframeInterval?: number; // Keyframe interval in seconds
    
    // Hardware acceleration
    hardwareAccel?: 'none' | 'nvenc' | 'qsv' | 'amf' | 'videotoolbox';
    
    // UDP/MPEG-TS specific
    mpegtsMuxrate?: number; // Muxrate for MPEG-TS (bits per second)
    mpegtsPmtPid?: number; // PMT PID
    mpegtsStartPid?: number; // Starting PID
    
    additionalArgs?: string[];
}

// --- NODECG ENGINE ---
export interface NodeCGConfig {
    host?: string;
    port?: number;
    protocol?: 'http' | 'https';
    namespace?: string;
    autoConnect?: boolean;
}

// --- MPV ENGINE ---
export interface MPVConfig {
    executablePath?: string; // Path to mpv.exe (default: mpv in PATH or ./mpv/mpv.exe)
    ipcPath?: string; // Path for JSON IPC socket/pipe (default: auto-generated)
    windowTitle?: string; // Window title for OBS window capture (default: "MPV Output")
    alwaysOnTop?: boolean; // Keep window on top
    fullscreen?: boolean; // Start in fullscreen
    borderless?: boolean; // Borderless window
    width?: number; // Window width (default: 1920)
    height?: number; // Window height (default: 1080)
    x?: number; // Window X position
    y?: number; // Window Y position
    vo?: string; // Video output driver (default: 'gpu' or 'direct3d' on Windows)
    hwdec?: string; // Hardware decoding (default: 'auto')
    audioDevice?: string; // Audio device name
    volume?: number; // Initial volume (0-100, default: 100)
    additionalArgs?: string[]; // Additional MPV command-line arguments
}

// --- FFPLAYOUT ENGINE ---
export interface FFplayoutConfig {
    configPath?: string; // Path to ffplayout config file (relative to ffplayout dir or absolute)
    channel?: number; // Channel ID (default: 1)
    apiPort?: number; // API server port (default: 8787)
    apiListen?: string; // API listen address (e.g., "127.0.0.1:8787")
    logLevel?: 'trace' | 'debug' | 'info' | 'warn' | 'error'; // Log level
    dbPath?: string; // Database path (optional)
    // API authentication (optional, for API control)
    apiUsername?: string;
    apiPassword?: string;
    apiBaseUrl?: string; // Override default API URL
}

// --- GSTREAMER ENGINE ---
export interface GStreamerConfig {
    pipeline: string; // GStreamer pipeline string
    outputType: 'rtmp' | 'srt' | 'udp' | 'file' | 'decklink' | 'ndi';
    outputUrl: string;
    videoFormat: string; // NV12, I420, etc.
    audioFormat: string; // S16LE, F32LE, etc.
    width: number;
    height: number;
    framerate: number;
}

// --- DEKTEC ENGINE ---
export interface DekTecDevice {
    serial: number;
    deviceName: string;
    description: string;
    port: number;
    isSdi: boolean;
    isAvFifo: boolean;
    isInput: boolean;
    isOutput: boolean;
}

export interface DekTecConfig {
    serial: number;
    port: number;
    bitrate: number; // For ASI output
    outputMode: 'asi' | 'sdi' | 'ip';
    videoFormat?: string; // For SDI: 1080i50, 720p60, etc.
    // Enhanced SDI settings
    sdiVideoFormat?: SdiVideoFormat; // SDI video format preset
    sdiStandard?: 'smpte292m' | 'smpte259m'; // HD-SDI or SD-SDI
    clockSource?: 'internal' | 'external' | 'word'; // Clock synchronization
    embeddedAudio?: boolean; // Embed audio in SDI
    audioChannels?: number; // Number of embedded audio channels (2, 4, 8, 16)
}

export type SdiVideoFormat = 
    | '720p59.94' | '720p60'
    | '1080i59.94' | '1080i60'
    | '1080p29.97' | '1080p30'
    | '1080p23.98' | '1080p24'
    | '1080p25' | '1080i50'
    | '720p50' | '720p25';

// --- NDI ENGINE ---
export interface NdiSource {
    name: string;
    url?: string;
    ipAddress?: string;
    port?: number;
    isAvailable: boolean;
    lastSeen?: number;
    resolution?: string;
    framerate?: number;
}

export interface NdiConfig {
    sourceName: string;
    sourceUrl?: string;
    lowBandwidth?: boolean;
    audioEnabled?: boolean;
    videoEnabled?: boolean;
}

export interface NdiOutputConfig {
    enabled: boolean;
    sourceName: string;
    useEngineNDI?: boolean; // Use engine's built-in NDI (e.g., CasperCG)
    fallbackSender?: boolean; // Use separate NDI sender if engine doesn't support NDI
    groups?: string;
    clockVideo?: boolean;
    clockAudio?: boolean;
}

// --- DECKLINK ENGINE ---
export interface DeckLinkDevice {
    index: number;
    displayName: string;
    modelName: string;
    supportsInput: boolean;
    supportsOutput: boolean;
    supportsKeying: boolean;
}

export interface DeckLinkConfig {
    deviceIndex: number;
    mode?: string; // Video mode string (e.g., "1080i5994", "720p5994")
    pixelFormat?: 'yuv422' | 'yuv422p10' | 'rgb444' | 'argb';
    timecodeFormat?: 'none' | 'rp188vitc' | 'rp188vitc2' | 'rp188ltc' | 'rp188any';
    outputMode: 'playback' | 'key';
}

// --- ATSC MUX SYSTEM ---
export type MuxInputSourceType = 
    | 'playplay_instance' 
    | 'udp_stream' 
    | 'srt_stream' 
    | 'local_file' 
    | 'ndi_source' 
    | 'decklink_input';

export interface MuxChannelConfig {
    channelId: number; // 1-4
    enabled: boolean;
    inputSource: MuxInputSourceType;
    inputConfig: {
        // For playplay_instance
        instanceId?: string;
        instanceUrl?: string;
        // For udp_stream
        udpUrl?: string;
        // For srt_stream
        srtUrl?: string;
        // For local_file
        filePath?: string;
        // For ndi_source
        ndiSourceName?: string;
        // For decklink_input
        deckLinkDeviceIndex?: number;
    };
    encoding: {
        codec: 'h264_nvenc' | 'libx264' | 'mpeg2video';
        videoBitrate: string; // e.g., '5M'
        audioCodec: 'aac' | 'ac3' | 'mp2';
        audioBitrate: string; // e.g., '192k'
        resolution: string; // e.g., '1920x1080'
        framerate: number;
        gopSize?: number;
        bFrames?: number;
        profile?: string;
        level?: string;
    };
    output: {
        destination: 'dektec_asi' | 'dektec_sdi' | 'udp' | 'file';
        dektecSerial?: number;
        dektecPort?: number;
        dektecSdiFormat?: SdiVideoFormat;
        udpUrl?: string;
        filePath?: string;
    };
}

export interface MuxChannelStatus {
    channelId: number;
    enabled: boolean;
    inputConnected: boolean;
    encodingActive: boolean;
    outputActive: boolean;
    currentBitrate?: number;
    frameRate?: number;
    errors?: string[];
}

export interface ATSCMuxConfig {
    enabled: boolean;
    channels: MuxChannelConfig[];
    statmux: {
        enabled: boolean;
        totalBitrate: number; // Total bitrate for all channels (e.g., 19392658 for ATSC)
        minChannelBitrate?: number; // Minimum per-channel bitrate
        maxChannelBitrate?: number; // Maximum per-channel bitrate
    };
    psiSi: {
        enabled: boolean;
        // ATSC 1.0 PSIP Tables
        psip: {
            enabled: boolean;
            // MGT (Master Guide Table)
            mgt: {
                tableId: number; // 0xC7
                version: number;
                protocolVersion: number;
            };
            // VCT (Virtual Channel Table) - Terrestrial
            vct: {
                enabled: boolean;
                transportStreamId: number;
                protocolVersion: number;
                channels: Array<{
                    shortName: string; // 7 chars max
                    majorChannelNumber: number; // 0-999
                    minorChannelNumber: number; // 0-999
                    modulationMode: number; // 8-VSB = 0x04
                    carrierFrequency: number; // Hz
                    channelTSID: number;
                    programNumber: number;
                    ETMLocation: number;
                    accessControlled: boolean;
                    hidden: boolean;
                    hideGuide: boolean;
                    serviceType: number; // 0x02 = ATSC digital television
                    sourceId: number;
                }>;
            };
            // EIT (Event Information Table)
            eit: {
                enabled: boolean;
                events: Array<{
                    eventId: number;
                    startTime: number; // Unix timestamp
                    lengthInSeconds: number;
                    title: string;
                    description?: string;
                    rating?: string;
                }>;
            };
            // STT (System Time Table)
            stt: {
                enabled: boolean;
                systemTime?: number; // Unix timestamp (auto-updated, optional)
            };
            // RRT (Rating Region Table) - Optional
            rrt: {
                enabled: boolean;
                ratingRegion: number; // 0 = US
            };
        };
        // ATSC 3.0 VTC (Virtual Transport Channel)
        atsc3: {
            enabled: boolean;
            vtc: {
                enabled: boolean;
                channelFrequency: number; // MHz (e.g., 26.1)
                channelNumber: number; // Physical channel (e.g., 26)
                subChannelNumber: number; // Sub-channel (e.g., 1)
                serviceId: number;
                plpId: number; // Physical Layer Pipe ID
            };
        };
        // Low jitter / constant bitrate for professional exciter
        lowJitter?: boolean;
        constantBitrate?: number; // bps (e.g., 19392658 for ATSC 1.0)
        // Basic PSI/SI (for compatibility)
        networkName?: string;
        serviceName?: string;
        serviceId?: number;
        programNumber?: number;
        pmtPid?: number;
        videoPid?: number;
        audioPid?: number;
    };
    compliance: {
        validate: boolean;
        atscVersion: '1.0' | '3.0';
    };
}

// --- TSDUCK CONFIG ---
export interface TSDUCKConfig {
    enabled: boolean;
    statmux: {
        enabled: boolean;
        totalBitrate: number;
        channels: Array<{
            channelId: number;
            minBitrate?: number;
            maxBitrate?: number;
            priority?: number;
        }>;
    };
    packetInjection: {
        enabled: boolean;
        pat?: boolean;
        pmt?: boolean;
        sdt?: boolean;
        eit?: boolean;
        customScript?: string;
    };
    compliance: {
        validate: boolean;
        analyze: boolean;
    };
}

export interface OBSSession {
    id: string;
    name: string;           // e.g., "Main OBS", "Backup OBS"
    host: string;           // default: localhost
    port: number;           // default: 4455
    password?: string;
    autoConnect: boolean;
    isConnected?: boolean;
    isRecording?: boolean;
    isStreaming?: boolean;
}

export interface OBSAction {
    type: 'change_scene' | 'load_scene_collection' | 'start_record' | 'stop_record' | 'start_stream' | 'stop_stream' | 'toggle_source';
    sessionId: string;      // Which OBS session
    sceneName?: string;
    collectionName?: string;
    sourceName?: string;
}

export type OBSCommandType =
    | 'change_scene'
    | 'change_scene_collection' // New
    | 'show_source'
    | 'hide_source'
    | 'toggle_source'
    | 'set_source_visibility' // New (explicit on/off)
    | 'start_record'
    | 'stop_record'
    | 'start_stream'
    | 'stop_stream'
    | 'start_udp_source' // New
    | 'custom';

export interface OBSCommand {
    id: string;
    label: string;
    commandType: OBSCommandType;
    // Target session (if empty, applies to all connected sessions)
    sessionId?: string;
    // Scene/source configuration
    sceneName?: string;
    sourceName?: string;
    sceneCollection?: string; // New
    visible?: boolean; // New (for set_source_visibility)
    // Recording configuration
    recordFileName?: string; // Supports wildcards: {date}, {time}, {title}, {slot}, {hour}
    recordDirectory?: string;
    // Custom command (for commandType='custom')
    requestType?: string;
    requestData?: string; // JSON string
    // Metadata
    description?: string;
    duration?: number; // Duration in seconds (for playlist items)
}

// --- DDR (Digital Disk Recorder) ---
export type DDRChannel = 'A' | 'B' | 'C' | 'D';

export interface DDRPlaylist {
    id: string;
    name: string;
    channelId: DDRChannel;
    items: PlaylistItem[];
    // Options
    isLooping: boolean;
    isRandomized: boolean;
    playFolder?: string;    // If set, play all items from this folder
    playFolderDuration?: number; // Max duration in seconds for folder play
    // PiP settings
    pipEnabled: boolean;
    pipPosition: 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right';
    pipSize: number;        // Percentage of screen (5-50)
    pipOpacity: number;     // 0-1
    // State
    isPlaying: boolean;
    volume: number;         // 0-1
    currentItemIndex: number;
}

export interface DDRState {
    channels: Record<DDRChannel, DDRPlaylist>;
}

// --- PROGRAM-SPECIFIC BUMPERS ---
export interface BumperConfig {
    programSeries?: string;     // If set, only plays with this program
    position: 'pre' | 'post' | 'both';
    isGeneric: boolean;         // Generic bumpers play with any program
}

// Extend MediaItem for bumper support (existing interface, add optional field)
export interface MediaItemExtended extends MediaItem {
    bumperConfig?: BumperConfig;
}

// --- LIVE INPUT ---
export interface LiveInputDevice {
    id: string;
    deviceId: string;       // Browser device ID
    label: string;
    type: 'audio' | 'video' | 'both';
    isActive: boolean;
    isMuted: boolean;
    volume: number;         // 0-1
    audioLevel?: number;    // Current level 0-1
}

export interface LiveInputState {
    devices: LiveInputDevice[];
    maxDevices: number;     // Default 4
}

// --- SOCIAL MEDIA ---
export type SocialPlatform = 'facebook' | 'twitter' | 'instagram' | 'custom_webhook';

export interface SocialAccount {
    id: string;
    platform: SocialPlatform;
    name: string;
    accessToken?: string;   // Stored separately for security
    isConnected: boolean;
}

export interface SocialPost {
    id: string;
    accountId: string;
    template: string;       // Supports {title}, {time}, {duration}, {description}
    triggerType: 'manual' | 'on_program_start' | 'on_program_end' | 'scheduled';
    scheduledTime?: number;
    linkedMediaId?: string;
    status: 'pending' | 'sent' | 'failed';
}

// --- CONTEXT MENU ---
export interface ContextMenuItem {
    id: string;
    label: string;
    icon?: string;
    shortcut?: string;
    disabled?: boolean;
    danger?: boolean;
    divider?: boolean;
    submenu?: ContextMenuItem[];
    action?: () => void;
    actionId?: ContextMenuActionId;
    payload?: Record<string, unknown>;
    group?: 'primary' | 'transport' | 'cg' | 'file' | 'analytics' | 'destructive';
}

export interface ContextMenuState {
    visible: boolean;
    x: number;
    y: number;
    items: ContextMenuItem[];
    targetId?: string;
    targetType?: 'media' | 'playlist' | 'hour' | 'cg' | 'ddr' | 'starfile' | 'cg_template';
}

export type ContextMenuActionId =
    | 'open_preview'
    | 'add_to_playlist'
    | 'send_to_playlist'
    | 'send_to_ddr'
    | 'trigger_cg'
    | 'edit_segments'
    | 'attach_obs_command'
    | 'reveal_folder'
    | 'copy_share_link'
    | 'download'
    | 'rename'
    | 'move'
    | 'delete'
    | 'view_analytics'
    | 'import_to_library'
    | 'edit_hour'
    | 'copy_hour'
    | 'paste_hour'
    | 'clear_hour'
    | 'take_to_program'
    | 'update_program'
    | 'clear_preview'
    | 'clear_program'
    | 'select_template'
    | 'set_media_start'
    | 'set_media_expire'
    | 'open_ad_cadence_wizard'
    | 'generate_asrun_report'
    | 'copy_day'
    | 'paste_day'
    | 'clear_day'
    | 'shake_hour_ads'
    | 'undo_shake_hour_ads'
    | 'toggle_show_hour'
    | 'show_hour_watch_folder'
    | 'show_hour_clear_watch_folder'
    | 'show_hour_refresh_watch'
    | 'media_priority_increase'
    | 'media_priority_decrease'
    | 'media_priority_freeze_toggle';

// --- STARFILE / FILE HUB ---
export type StarfileItemType = 'folder' | 'image' | 'video' | 'audio' | 'document' | 'archive' | 'file';

export interface StarfileNodePresence {
    nodeId: string;
    label: string;
    hostname?: string;
    siteId?: string;
    playerId?: string;
    online?: boolean;
    role?: 'core' | 'edge';
}

export interface StarfileNetworkPresence {
    matchedBy: 'fingerprint' | 'filename_size' | 'none';
    copyCount: number;
    onlineCount: number;
    targetCopies: number;
    healthy: boolean;
    availabilityLabel: string;
    fingerprint?: string | null;
    nodes: StarfileNodePresence[];
    missingNodes?: StarfileNodePresence[];
}

export interface StarfileFileItem {
    id: string;
    name: string;
    path: string;
    directory: string;
    isDir: boolean;
    type: StarfileItemType;
    size: number;
    createdDate?: string | null;
    modifiedDate?: string | null;
    previewUrl?: string | null;
    shareUrl?: string | null;
    fingerprint?: string | null;
    fingerprintAlgorithm?: string | null;
    network?: StarfileNetworkPresence | null;
    analyticsKey: string;
}

export interface StarfileComment {
    user: string;
    comment: string;
    timestamp?: string;
}

export interface StarfileFileDetails {
    size: number;
    created_date?: string | null;
    modified_date?: string | null;
    last_accessed?: string | null;
    download_count?: number;
    fingerprint?: string | null;
    fingerprint_algorithm?: string | null;
}

// --- ANALYTICS ---
export interface AnalyticsEventRecord {
    id: number;
    family: string;
    action: string;
    entityType?: string;
    entityId?: string;
    entityLabel?: string;
    createdAt: string;
    payload?: Record<string, unknown>;
}

export interface AnalyticsEntitySummary {
    entityId?: string;
    entityLabel: string;
    total: number;
}

export interface AnalyticsTimeBucket {
    bucket: string;
    family: string;
    total: number;
}

export interface AnalyticsSummary {
    totalsByFamily: Record<string, number>;
    totalsByAction: Record<string, Array<{ action: string; total: number }>>;
    topEntities: Record<string, AnalyticsEntitySummary[]>;
    timeBuckets: AnalyticsTimeBucket[];
    recentEvents: AnalyticsEventRecord[];
}

// --- EXTENDED SETTINGS ---
export interface ExtendedSettings extends AutomationSettings {
    // OBS
    obsSessions: OBSSession[];
    // DDR
    ddrAutoPreload: boolean;
    ddrDefaultVolume: number;
    // Social
    socialAccounts: SocialAccount[];
    socialDefaultTemplate: string;
    // Live Input
    liveInputEnabled: boolean;
    // Sales Reporting
    salesReportingEndpoint?: string;
    // Star News API
    starNewsApiBaseUrl?: string; // e.g., "https://getstarnews.com"
    // Metro Weather (legacy, use starNewsApiBaseUrl if available)
    metroWeatherServiceUrl?: string;
    // Tower Cams
    towerCamUrls?: string[];
    // OpenAI
    openaiApiKey?: string;
    // Voice / Speech intelligence
    ttsProvider?: 'auto' | 'kokoro-local' | 'starnews' | 'openai';
    kokoroTtsBaseUrl?: string;
    kokoroVoice?: string;
    sttProvider?: 'auto' | 'local-whisper' | 'openai-whisper';
    localWhisperBaseUrl?: string;
    // Content Feeds (legacy, use starNewsApiBaseUrl if available)
    storyFeedUrl?: string;
    cinemaFeedUrl?: string;
}

// --- MULTI-SITE TICKER ---
export interface SiteLocation {
    id: string;
    name: string;
    type: 'radio' | 'website' | 'playout';
    regionTags?: string[];
    city?: string;
    state?: string;
    timezone?: string;
    defaultShiftHours?: number;
}

export interface SiteState {
    locationId: string;
    currentMedia: { title: string; id?: string } | null;
    nextMedia: { title: string; id?: string } | null;
    isPlaying: boolean;
    currentTime: number;
    timestamp: number;
}

export interface PlayoutSiteProfile extends SiteLocation {
    type: 'playout';
    schedulePrefix: string;
    callSign?: string;
    adRegionWeight?: number;
    enabled: boolean;
}

// --- SEGMENT COLLECTIONS ---
export interface SegmentCollection {
    id: string;
    name: string;
    segments: Segment[];
    isActive: boolean;
}

// --- FILE INDEXING ---
export interface FileIndexEntry {
    hash: string; // SHA-256 hash
    filePath: string;
    size: number;
    modifiedTime: number;
    siteLocations: string[]; // which sites have this file
    metadata?: {
        duration?: number;
        codec?: string;
        resolution?: string;
    };
}

export interface SiteFileAvailability {
    siteId: string;
    hasFile: boolean;
    transferStatus?: 'pending' | 'downloading' | 'complete' | 'error';
    filePath?: string;
}

// --- MULTI-GRID ---
export interface HourGridCell {
    siteId: string;
    day: number; // 0-6
    hour: number; // 0-23
    filledDuration: number; // seconds
    itemCount: number;
    adCount: number;
    /** Total seconds of scheduled ad content in the hour */
    adDurationSec: number;
    /**
     * Max `rules.maxPlaysPerHour` across active traffic clients (playplay_clients); 0 = no cap configured.
     * Used only as a cadence hint for UI (slot count vs station traffic rules).
     */
    targetAdSlotsPerHour: number;
    /** When targetAdSlotsPerHour > 0: min(100, round(100 * adCount / target)) */
    adSlotCoveragePct: number | null;
    isEmpty: boolean;
}

// --- COMMAND CONTAINERS ---
export interface CommandContainer {
    id: string;
    name: string;
    type: 'scene' | 'live_stream' | 'social_post' | 'go_live' | 'equipment_control' | 'automation' | 'custom';
    equipment: 'onvif' | 'obs' | 'atem' | 'tc1' | 'none';
    target: string; // equipment identifier (camera IP, OBS scene name, etc.)
    parameters: Record<string, any>;
    conditions?: {
        onStart?: boolean;
        onEnd?: boolean;
        onTime?: number; // execute at specific time
        onCondition?: string; // custom condition
    };
    handlers?: {
        onSuccess?: string; // command ID to run on success
        onFailure?: string; // command ID to run on failure
    };
    metadata?: {
        description?: string;
        category?: string;
        tags?: string[];
    };
}

// --- SCHEDULE COMMANDS (Run alongside media on their own timeline) ---
export type ScheduleCommandType =
    | 'show_webpage'      // Display webpage in OBS browser source for specified time
    | 'switch_scene'      // Switch to OBS scene for specified time, then back
    | 'play_stream'       // Play stream (native or OBS media source)
    | 'cg_display'        // Show CG overlay for specified time
    | 'execute_script'    // Run custom script/command
    | 'social_post'       // Post to social media at scheduled time
    | 'send_to_remote'    // Send command to remote instance
    | 'audio_ducking'     // Duck audio for specified time
    | 'tally_control'     // Control tally lights
    | 'custom';

export interface ScheduleCommand {
    id: string;
    type: ScheduleCommandType;
    name: string;
    description?: string;
    // Timing (relative to slot start, in seconds)
    inPoint: number;      // Start time within the hour (0-3600)
    outPoint: number;     // End time within the hour (0-3600)
    // Target configuration
    target: {
        obsScene?: string;           // Scene to switch to
        browserUrl?: string;         // URL for webpage command
        streamUrl?: string;          // Stream URL
        streamType?: 'native' | 'obs_media_source';
        cgTemplateId?: string;       // CG template ID
        remoteInstanceId?: string;   // Remote instance for send_to_remote
        scriptPath?: string;         // Script to execute
    };
    // Return behavior
    returnToAutomation?: boolean;    // Return to AUTOMATION scene after
    automationSceneName?: string;    // Scene name to return to (default: "AUTOMATION")
    // OBS integration
    obsSessionId?: string;           // Target OBS session
    browserSourceName?: string;      // Name of browser source to use
    mediaSourceName?: string;        // Name of media source for streams
    // Styling
    color?: string;                  // Display color in timeline
    // Status
    enabled: boolean;
    lastExecuted?: number;
}

// --- STREAM SOURCES ---
export interface StreamSource {
    id: string;
    name: string;
    type: 'rtsp' | 'rtmp' | 'srt' | 'hls' | 'udp' | 'ndi';
    url: string;
    // Playback configuration
    playbackMode: 'native' | 'obs_media_source';
    // OBS configuration (for obs_media_source mode)
    obsSessionId?: string;
    obsSceneName?: string;           // Scene containing the media source
    obsMediaSourceName?: string;     // Name of the media source in OBS
    automationSceneName?: string;    // Scene to return to after stream ends
    // Stream behavior
    duration?: number;               // Max duration (0 = unlimited)
    autoReturn?: boolean;            // Return to automation after stream ends
    // Metadata
    thumbnail?: string;
    description?: string;
    tags?: string[];
}

// --- MULTI-CHANNEL SUPPORT ---
export interface PlayoutChannel {
    id: string;
    name: string;
    scheduleId: string;              // Reference to the schedule used by this channel
    // Preview configuration
    previewEnabled: boolean;
    previewPosition: 'main' | 'secondary' | 'grid';
    // Output configuration
    outputConfig?: {
        mpvIpcPath?: string;
        obsSessionId?: string;
        ndiOutputName?: string;
    };
    // Status
    isPlaying: boolean;
    currentItem?: PlaylistItem;
    nextItem?: PlaylistItem;
    // Playmode configuration
    playMode?: PlayMode;
    nonTimeConfig?: NonTimePlayConfig;
    // Standalone engine — when this channel was spawned, the channelEngineManager
    // started a Node child process serving its own browser-source URL on this port.
    enginePort?: number;
    engineUrl?: string;
    // CasparCG 1-based channel index (CH1 → 1). Set when added via [+].
    casparChannel?: number;
}

export interface MultiChannelState {
    channels: PlayoutChannel[];
    activeChannelId: string;
    layoutMode: '1x1' | '2x1' | '2x2'; // Preview grid layout
    maxChannels: number;             // 1-4
}

// --- PLAY MODE TYPES ---
export type PlayMode = 'scheduled' | 'playlist_loop' | 'folder_loop';
export type FolderSortOrder = 'name' | 'date' | 'size' | 'random';

export interface NonTimePlayConfig {
    mode: PlayMode;
    loopUntilHour?: number; // 0-23, undefined = loop forever
    folderPath?: string;
    sortOrder?: FolderSortOrder;
    playlistId?: string; // For playlist_loop mode
}

// --- REMOTE INSTANCES ---
export interface RemoteInstance {
    id: string;
    name: string;
    address: string;                 // IP or hostname
    port: number;
    isConnected: boolean;
    lastSeen?: number;
    capabilities?: string[];
}

// --- CG LAYER CONFIGURATION ---
export interface CGLayerConfig {
    channelId: string;
    layerId: number; // 1-4
    obsSourceName: string;
    obsSceneName: string;
    visible: boolean;
    content?: CGElement;
}

export interface ChannelCGConfig {
    channelId: string;
    layers: CGLayerConfig[]; // 4 layers per channel
}

export interface EquipmentConnection {
    id: string;
    type: 'onvif' | 'obs' | 'atem' | 'tc1';
    name: string;
    address: string;
    port?: number;
    credentials?: {
        username?: string;
        password?: string;
    };
    isConnected: boolean;
    lastConnected?: number;
    capabilities?: string[]; // available features
}

// --- COMPLIANCE MONITORING ---
export interface ComplianceStatus {
    siteId: string;
    isCompliant: boolean;
    issues: ComplianceIssue[];
    lastChecked: number;
}

export interface ComplianceIssue {
    type: 'not_playing' | 'off_sync' | 'missing_file' | 'schedule_conflict' | 'over_filled' | 'under_filled';
    severity: 'warning' | 'error' | 'critical';
    itemId?: string;
    message: string;
    timestamp: number;
}

export interface TapeFeedItem {
    id: string;
    siteId: string;
    startTime: number;
    endTime: number;
    title: string;
    type: MediaType;
    isPlaying: boolean;
    isCompliant: boolean;
    complianceIssues?: ComplianceIssue[];
}

// --- ERROR HANDLING ---
export interface ErrorEntry {
    id: string;
    instanceId: string; // site ID or 'local'
    timestamp: number;
    severity: 'warning' | 'error' | 'critical';
    message: string;
    source: string; // component/function that generated error
    acknowledged: boolean;
    details?: any; // additional error context
}

// --- DRIVE WATCHING ---
export interface DriveWatchConfig {
    paths: string[];
    includePatterns?: string[];
    excludePatterns?: string[];
    watchSubdirectories: boolean;
}
