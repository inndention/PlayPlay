// ============================================
// BUILT-IN PLAYOUT ENGINE
// Optimized internal HTML5 player with broadcast features
// ============================================

import { PlaylistItem } from './types';
import { runPlaylistItemActions } from './automationActions';

export type PlayoutEngineType = 'internal';

/** Options for PlayoutEngineManager */
export interface PlayoutEngineManagerOptions {
    /** Prefer MPV broadcast engine over built-in HTML5 player (Electron only). Default: false */
    preferMpv?: boolean;
    /** If MPV fails to start, automatically fall back to the built-in Electron HTML5 player. Default: true */
    fallbackToElectron?: boolean;
}

export interface PlayoutEngine {
    id: string;
    name: string;
    type: PlayoutEngineType;
    enabled: boolean;
    status: 'connected' | 'disconnected' | 'error';
    error?: string;
}

export interface PlayoutEngineConfig {
    id: string;
    name: string;
    type: PlayoutEngineType;
    enabled: boolean;
}

export interface PlaybackState {
    currentItem: PlaylistItem | null;
    position: number; // seconds
    isPlaying: boolean;
    isPaused: boolean;
    lastUpdate: number;
    extra?: {
        audioLevels?: {
            left?: number;
            right?: number;
        };
    };
}

// Internal player callback types
type PlaybackStateCallback = (state: PlaybackState) => void;
type EngineStatusCallback = (engine: PlayoutEngine) => void;

export class PlayoutEngineManager {
    private engines: Map<string, PlayoutEngine> = new Map();
    private activeEngineId: string | null = null;
    private onEngineStatusChange?: EngineStatusCallback;
    private onPlaybackStateChange?: PlaybackStateCallback;
    private options: Required<PlayoutEngineManagerOptions>;
    
    // Internal player state
    private internalState: PlaybackState = {
        currentItem: null,
        position: 0,
        isPlaying: false,
        isPaused: false,
        lastUpdate: Date.now(),
    };

    constructor(
        onEngineStatusChange?: EngineStatusCallback,
        onPlaybackStateChange?: PlaybackStateCallback,
        options: PlayoutEngineManagerOptions = {}
    ) {
        this.onEngineStatusChange = onEngineStatusChange;
        this.onPlaybackStateChange = onPlaybackStateChange;
        this.options = {
            preferMpv: options.preferMpv ?? false,
            fallbackToElectron: options.fallbackToElectron ?? true,
        };
        
        // Auto-register built-in engine
        this.addEngine({
            id: 'built-in',
            name: 'Built-in HTML5 Player',
            type: 'internal',
            enabled: true,
        });

        // Register MPV Engine if running in Electron AND preferMpv is enabled
        if (
            this.options.preferMpv &&
            typeof window !== 'undefined' &&
            (window as any).electronAPI &&
            (window as any).electronAPI.mpvBroadcastStart
        ) {
            this.addEngine({
                id: 'mpv',
                name: 'MPV Broadcast Engine',
                type: 'internal', // Treat as internal for compatibility but route to IPC
                enabled: true,
            });

            // Start it
            (window as any).electronAPI.mpvBroadcastStart({
                executablePath: 'mpv/mpv.exe'
            }).then((res: any) => {
                if (res && res.success) {
                    console.log('[PlayoutEngineManager] MPV Engine Started via IPC');
                    this.setActiveEngine('mpv');
                } else {
                    this._handleMpvFailure(`MPV start returned: ${JSON.stringify(res)}`);
                }
            }).catch((err: any) => {
                this._handleMpvFailure(err?.message || String(err));
            });
        }
        
        this.setActiveEngine('built-in');
    }

    /** Called when MPV fails to start */
    private _handleMpvFailure(reason: string): void {
        console.error('[PlayoutEngineManager] MPV Engine failed to start:', reason);
        if (this.options.fallbackToElectron) {
            console.warn('[PlayoutEngineManager] Falling back to built-in HTML5 player.');
            this.setActiveEngine('built-in');
        }
    }

    // Add/Update engine configuration
    async addEngine(config: PlayoutEngineConfig): Promise<void> {
        const engine: PlayoutEngine = {
            id: config.id,
            name: config.name,
            type: config.type,
            enabled: config.enabled,
            status: 'connected', // Internal player is always connected
        };

        this.engines.set(config.id, engine);
        this.notifyStatusChange(engine);
    }

    // Remove engine
    removeEngine(engineId: string): void {
        if (engineId === 'built-in') {
            console.warn('[PlayoutEngine] Cannot remove built-in engine');
            return;
        }
        this.engines.delete(engineId);
        if (this.activeEngineId === engineId) {
            this.activeEngineId = 'built-in';
        }
    }

    // Set active engine
    setActiveEngine(engineId: string): boolean {
        const engine = this.engines.get(engineId);
        if (!engine || !engine.enabled || engine.status !== 'connected') {
            return false;
        }
        this.activeEngineId = engineId;
        return true;
    }

    // Play media on active engine (basic)
    async playMedia(item: PlaylistItem): Promise<void> {
        return this.playMediaAt(item, 0);
    }

    // Play media at specific position (for time-accurate sync)
    async playMediaAt(item: PlaylistItem, positionSeconds: number = 0): Promise<void> {
        if (!this.activeEngineId) {
            throw new Error('No active playout engine');
        }

        const engine = this.engines.get(this.activeEngineId);
        if (!engine || engine.status !== 'connected') {
            throw new Error(`Engine ${this.activeEngineId} is not connected`);
        }

        // Run automation actions attached to this item (Facebook, etc.)
        runPlaylistItemActions(item).catch(err => console.error('[AutomationActions] Failed:', err));

        if (this.activeEngineId === 'mpv' && (window as any).electronAPI) {
            try {
                // If the item provides an item.id or we just pass it to load
                await (window as any).electronAPI.mpvBroadcastPlayDirect(item, positionSeconds);
            } catch (err) {
                console.error('[PlayoutEngineManager] MPV Play Error:', err);
            }
        }

        // Internal player - update state and notify React component
        this.internalState = {
            currentItem: item,
            position: positionSeconds,
            isPlaying: true,
            isPaused: false,
            lastUpdate: Date.now(),
        };
        
        this.notifyPlaybackStateChange();
        console.log(`[PlayoutEngine] Playing: ${item.media.title} at ${positionSeconds}s on engine ${this.activeEngineId}`);
    }

    // Pause active engine
    async pause(): Promise<void> {
        if (this.activeEngineId === 'mpv' && (window as any).electronAPI) {
            await (window as any).electronAPI.mpvBroadcastPause().catch(console.error);
        }
        this.internalState.isPaused = true;
        this.internalState.isPlaying = false;
        this.internalState.lastUpdate = Date.now();
        this.notifyPlaybackStateChange();
    }

    // Resume active engine
    async resume(): Promise<void> {
        if (this.activeEngineId === 'mpv' && (window as any).electronAPI) {
            await (window as any).electronAPI.mpvBroadcastResume().catch(console.error);
        }
        this.internalState.isPaused = false;
        this.internalState.isPlaying = true;
        this.internalState.lastUpdate = Date.now();
        this.notifyPlaybackStateChange();
    }

    // Stop active engine
    async stop(): Promise<void> {
        if (this.activeEngineId === 'mpv' && (window as any).electronAPI) {
            await (window as any).electronAPI.mpvBroadcastStopPlayback().catch(console.error);
        }
        this.internalState.isPlaying = false;
        this.internalState.isPaused = false;
        this.internalState.currentItem = null;
        this.internalState.position = 0;
        this.internalState.lastUpdate = Date.now();
        this.notifyPlaybackStateChange();
    }

    // Seek active engine
    async seek(seconds: number): Promise<void> {
        if (this.activeEngineId === 'mpv' && (window as any).electronAPI) {
            await (window as any).electronAPI.mpvBroadcastSeek(seconds).catch(console.error);
        }
        this.internalState.position = seconds;
        this.internalState.lastUpdate = Date.now();
        this.notifyPlaybackStateChange();
    }

    /**
     * Update internal state from PlayerEngine component
     * Called via IPC or direct reference
     */
    updateInternalState(state: Partial<PlaybackState>): void {
        this.internalState = {
            ...this.internalState,
            ...state,
            lastUpdate: Date.now(),
        };
        this.notifyPlaybackStateChange();
    }

    // Get playback state
    getPlaybackState(): PlaybackState | null {
        return this.internalState;
    }

    // Get all engines
    getEngines(): PlayoutEngine[] {
        return Array.from(this.engines.values());
    }

    // Get active engine
    getActiveEngine(): PlayoutEngine | null {
        if (!this.activeEngineId) return null;
        return this.engines.get(this.activeEngineId) || null;
    }

    // Internal player is always active
    isInternalActive(): boolean {
        return true;
    }

    private notifyStatusChange(engine: PlayoutEngine): void {
        if (this.onEngineStatusChange) {
            this.onEngineStatusChange(engine);
        }
    }

    private notifyPlaybackStateChange(): void {
        if (this.onPlaybackStateChange) {
            this.onPlaybackStateChange(this.internalState);
        }
    }
}

// Singleton instance
let engineManagerInstance: PlayoutEngineManager | null = null;

export function getPlayoutEngineManager(
    onEngineStatusChange?: EngineStatusCallback,
    onPlaybackStateChange?: PlaybackStateCallback,
    options?: PlayoutEngineManagerOptions
): PlayoutEngineManager {
    if (!engineManagerInstance) {
        engineManagerInstance = new PlayoutEngineManager(
            onEngineStatusChange,
            onPlaybackStateChange,
            options
        );
    }
    return engineManagerInstance;
}

export function resetPlayoutEngineManager(): void {
    engineManagerInstance = null;
}
