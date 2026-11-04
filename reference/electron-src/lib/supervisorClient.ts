/**
 * supervisorClient.ts — REST + WebSocket client for the C++ Play-Play Supervisor.
 *
 * Replaces in-renderer PlayoutEngineManager / channelEngine IPC with HTTP/JSON
 * calls to the out-of-process Supervisor on localhost:8080 and live telemetry
 * on ws://localhost:8081.
 *
 * Responsibility split:
 *   - React UI: schedule editing, media browsing, telemetry display
 *   - Supervisor: spawn/respawn engines, time authority, REST/WS broker
 *   - ChannelEngine: decode, mix, encode, push to outputs
 */

export interface ChannelInfo {
    id: string;
    name: string;
    state?: string;
    pid?: number;
    healthy?: boolean;
    frameNumber?: number;
}

export interface PlaylistItem {
    id: string;
    mediaId: string;
    url?: string;
    inFrame?: number;
    outFrame?: number;
    startFrame?: number;
}

export interface TransportCommand {
    action: 'play' | 'pause' | 'stop' | 'skip' | 'cue';
    targetFrame?: number;
}

export interface MasterTime {
    frameNumber: number;
    nowNs: number;
    ntpOffsetNs: number;
}

export interface SupervisorHealth {
    score: number;
    status: 'healthy' | 'degraded' | 'critical';
    activeChannels: number;
    failedChannels: number;
}

export type TelemetryHandler = (event: TelemetryEvent) => void;

export type TelemetryEvent =
    | { type: 'connected'; clientId: string }
    | { type: 'channelSpawned'; channelId: string; port: number }
    | { type: 'channelDied'; channelId: string; reason: string }
    | { type: 'heartbeat'; channelId: string; frameNumber: number; healthy: boolean }
    | { type: 'stateChange'; channelId: string; [key: string]: unknown }
    | { type: 'asRun'; channelId: string; [key: string]: unknown }
    | { type: string; [key: string]: unknown };

export interface SupervisorClientOptions {
    restBaseUrl?: string;     // default http://localhost:8080
    wsUrl?: string;           // default ws://localhost:8081
    autoReconnect?: boolean;  // default true
    reconnectDelayMs?: number;// default 2000
}

export class SupervisorClient {
    private restBaseUrl: string;
    private wsUrl: string;
    private autoReconnect: boolean;
    private reconnectDelayMs: number;

    private ws: WebSocket | null = null;
    private wsHandlers = new Set<TelemetryHandler>();
    private reconnectTimer: number | null = null;
    private clientId: string | null = null;
    private isClosing = false;

    constructor(opts: SupervisorClientOptions = {}) {
        this.restBaseUrl = (opts.restBaseUrl ?? 'http://localhost:8080').replace(/\/$/, '');
        this.wsUrl = opts.wsUrl ?? 'ws://localhost:8081';
        this.autoReconnect = opts.autoReconnect ?? true;
        this.reconnectDelayMs = opts.reconnectDelayMs ?? 2000;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // REST API
    // ─────────────────────────────────────────────────────────────────────────

    private async request<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
        const res = await fetch(`${this.restBaseUrl}${path}`, {
            method,
            headers: body ? { 'Content-Type': 'application/json' } : undefined,
            body: body ? JSON.stringify(body) : undefined,
        });
        if (!res.ok) {
            const text = await res.text().catch(() => '');
            throw new Error(`Supervisor ${method} ${path} failed: ${res.status} ${text}`);
        }
        return res.json() as Promise<T>;
    }

    // ── Channels ────────────────────────────────────────────────────────────
    listChannels(): Promise<{ channels: ChannelInfo[] }> {
        return this.request('GET', '/api/v1/channels');
    }

    createChannel(channel: { id: string; name?: string; port?: number }): Promise<{ success: boolean; id: string }> {
        return this.request('POST', '/api/v1/channels', channel);
    }

    getChannelStatus(id: string): Promise<ChannelInfo> {
        return this.request('GET', `/api/v1/channels/${encodeURIComponent(id)}`);
    }

    startChannel(id: string): Promise<{ success: boolean }> {
        return this.request('POST', `/api/v1/channels/${encodeURIComponent(id)}/start`);
    }

    stopChannel(id: string): Promise<{ success: boolean }> {
        return this.request('POST', `/api/v1/channels/${encodeURIComponent(id)}/stop`);
    }

    killChannel(id: string): Promise<{ success: boolean }> {
        return this.request('POST', `/api/v1/channels/${encodeURIComponent(id)}/kill`);
    }

    // ── Playlist / transport ────────────────────────────────────────────────
    pushPlaylist(channelId: string, items: PlaylistItem[], autoplay = false): Promise<{ success: boolean }> {
        return this.request('POST', `/api/v1/channels/${encodeURIComponent(channelId)}/playlist`, {
            items,
            autoplay,
        });
    }

    sendTransport(channelId: string, cmd: TransportCommand): Promise<{ success: boolean }> {
        return this.request('POST', `/api/v1/channels/${encodeURIComponent(channelId)}/transport`, cmd);
    }

    // ── Time + health ───────────────────────────────────────────────────────
    getMasterTime(): Promise<MasterTime> {
        return this.request('GET', '/api/v1/time');
    }

    getHealth(): Promise<SupervisorHealth> {
        return this.request('GET', '/api/v1/health');
    }

    // ─────────────────────────────────────────────────────────────────────────
    // WebSocket telemetry
    // ─────────────────────────────────────────────────────────────────────────

    connect(): void {
        if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
            return;
        }
        this.isClosing = false;
        try {
            this.ws = new WebSocket(this.wsUrl);
        } catch (err) {
            console.error('[SupervisorClient] WebSocket construction failed', err);
            this.scheduleReconnect();
            return;
        }

        this.ws.onopen = () => {
            console.debug('[SupervisorClient] WebSocket connected');
        };

        this.ws.onmessage = (ev) => {
            try {
                const data = JSON.parse(ev.data) as TelemetryEvent;
                if (data.type === 'connected' && 'clientId' in data) {
                    this.clientId = data.clientId as string;
                }
                this.wsHandlers.forEach((h) => {
                    try { h(data); } catch (e) { console.error('[SupervisorClient] handler error', e); }
                });
            } catch (err) {
                console.warn('[SupervisorClient] bad WS frame', err);
            }
        };

        this.ws.onerror = (ev) => {
            console.warn('[SupervisorClient] WebSocket error', ev);
        };

        this.ws.onclose = () => {
            this.ws = null;
            this.clientId = null;
            if (!this.isClosing) this.scheduleReconnect();
        };
    }

    disconnect(): void {
        this.isClosing = true;
        if (this.reconnectTimer != null) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.ws) {
            try { this.ws.close(); } catch { /* ignore */ }
            this.ws = null;
        }
    }

    private scheduleReconnect(): void {
        if (!this.autoReconnect || this.isClosing) return;
        if (this.reconnectTimer != null) return;
        this.reconnectTimer = window.setTimeout(() => {
            this.reconnectTimer = null;
            this.connect();
        }, this.reconnectDelayMs);
    }

    onTelemetry(handler: TelemetryHandler): () => void {
        this.wsHandlers.add(handler);
        return () => { this.wsHandlers.delete(handler); };
    }

    isConnected(): boolean {
        return this.ws?.readyState === WebSocket.OPEN;
    }

    getClientId(): string | null {
        return this.clientId;
    }
}

// Singleton convenience instance
let _instance: SupervisorClient | null = null;
export function getSupervisorClient(opts?: SupervisorClientOptions): SupervisorClient {
    if (!_instance) _instance = new SupervisorClient(opts);
    return _instance;
}
