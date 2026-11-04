// ============================================
// OBS WebSocket Client
// Renderer view of the OBS sessions that starobs owns (docs/STAROBS-SERVE.md).
// Commands go to starobs over HTTP. Status and events come from starobs over SSE.
// starobs owns the OBS sockets, reconnect, backoff and auth.
// ============================================

import { OBSSession } from './types';
import {
    StarobsClient,
    StarobsEventStream,
    StarobsObsEvent,
    StarobsSessionConfig,
    StarobsSessionRow,
    StarobsSessionState,
    resolveStarobsBaseUrl,
} from './starobsClient';

// Event callback types
type OBSEventCallback = (event: string, data: Record<string, unknown>) => void;

const RESYNC_RETRY_MS = 2000;

export interface OBSSessionHealth {
    state: StarobsSessionState | 'unknown';
    lastError: string | null;
}

export interface OBSClientListener {
    onStatusChange?: (session: OBSSession) => void;
    onHealthChange?: (health: OBSSessionHealth) => void;
    onEvent?: OBSEventCallback;
}

// One starobs transport and one event stream per renderer. Session rows, recording and
// streaming flags, and stream health live here. Clients are views over this state.
type HubUpdate =
    | { kind: 'status' }
    | { kind: 'event'; eventType: string; eventData: Record<string, unknown> };

interface HubSession {
    row: StarobsSessionRow | null;
    recording: boolean;
    streaming: boolean;
    // Bumped on every OBS event. A snapshot taken before a bump is older than those events and is dropped.
    eventSeq: number;
}

let starobsTransport: StarobsClient | null = null;
let eventStream: StarobsEventStream | null = null;
let streamLive = false;
let streamEpoch = 0;
let streamDownReported = false;
let resyncTimer: ReturnType<typeof setTimeout> | null = null;
const hubSessions = new Map<string, HubSession>();
const hubSubscribers = new Map<string, Set<(update: HubUpdate) => void>>();
const obsClients = new Map<string, OBSWebSocketClient>();

function transport(): StarobsClient {
    if (!starobsTransport) starobsTransport = new StarobsClient(resolveStarobsBaseUrl());
    return starobsTransport;
}

function errorText(err: unknown): string {
    return err instanceof Error ? err.message : String(err);
}

function hubSessionFor(name: string): HubSession {
    let entry = hubSessions.get(name);
    if (!entry) {
        entry = { row: null, recording: false, streaming: false, eventSeq: 0 };
        hubSessions.set(name, entry);
    }
    return entry;
}

function isLive(name: string): boolean {
    return streamLive && hubSessions.get(name)?.row?.state === 'identified';
}

function publish(name: string, update: HubUpdate): void {
    hubSubscribers.get(name)?.forEach(notify => {
        try {
            notify(update);
        } catch (err) {
            console.error(`[OBS ${name}] Listener failed:`, err);
        }
    });
}

function publishAll(): void {
    hubSubscribers.forEach((_, name) => publish(name, { kind: 'status' }));
}

function subscribeToSession(name: string, notify: (update: HubUpdate) => void): () => void {
    let subscribers = hubSubscribers.get(name);
    if (!subscribers) {
        subscribers = new Set();
        hubSubscribers.set(name, subscribers);
    }
    subscribers.add(notify);
    return () => {
        hubSubscribers.get(name)?.delete(notify);
    };
}

// Stores a session row and returns the state it replaced.
function storeRow(row: StarobsSessionRow): StarobsSessionState | undefined {
    const entry = hubSessionFor(row.name);
    const previous = entry.row?.state;
    entry.row = row;
    if (row.state !== 'identified') {
        entry.recording = false;
        entry.streaming = false;
    }
    if (previous !== row.state) {
        console.log(`[OBS ${row.name}] starobs state: ${row.state}${row.lastError ? ` (${row.lastError})` : ''}`);
    }
    return previous;
}

function applyRow(row: StarobsSessionRow): void {
    const previous = storeRow(row);
    publish(row.name, { kind: 'status' });
    if (row.state === 'identified' && previous !== 'identified') void resyncSession(row.name);
}

function applyObsEvent(event: StarobsObsEvent): void {
    if (typeof event.session !== 'string') return;
    const data = event.eventData ?? {};
    const entry = hubSessionFor(event.session);
    entry.eventSeq += 1;
    if (event.eventType === 'RecordStateChanged') entry.recording = data.outputActive === true;
    if (event.eventType === 'StreamStateChanged') entry.streaming = data.outputActive === true;
    publish(event.session, { kind: 'event', eventType: event.eventType, eventData: data });
}

async function resyncSession(name: string): Promise<void> {
    const entry = hubSessionFor(name);
    const seq = entry.eventSeq;
    try {
        const snapshot = await transport().snapshot(name);
        if (entry.eventSeq !== seq || entry.row?.state !== 'identified') return;
        entry.recording = snapshot.recording === true;
        entry.streaming = snapshot.streaming === true;
        if (typeof snapshot.currentScene === 'string') {
            publish(name, { kind: 'event', eventType: 'CurrentProgramSceneChanged', eventData: { sceneName: snapshot.currentScene } });
        }
        publish(name, { kind: 'status' });
    } catch (err) {
        console.warn(`[OBS ${name}] Snapshot failed: ${errorText(err)}`);
    }
}

function clearResyncTimer(): void {
    if (resyncTimer) {
        clearTimeout(resyncTimer);
        resyncTimer = null;
    }
}

// Runs on every stream open. The snapshots replace OBS events that were missed while the stream was down.
async function resyncAll(epoch: number): Promise<void> {
    clearResyncTimer();
    try {
        const rows = await transport().listSessions();
        if (epoch !== streamEpoch) return;
        rows.forEach(row => storeRow(row));
        streamLive = true;
        streamDownReported = false;
        publishAll();
        rows.filter(row => row.state === 'identified').forEach(row => void resyncSession(row.name));
    } catch (err) {
        if (epoch !== streamEpoch) return;
        console.warn(`[starobs] Session list failed, retrying: ${errorText(err)}`);
        resyncTimer = setTimeout(() => void resyncAll(epoch), RESYNC_RETRY_MS);
    }
}

function ensureEventStream(): void {
    if (eventStream) return;
    eventStream = transport().openEvents({
        onOpen: () => {
            streamEpoch += 1;
            void resyncAll(streamEpoch);
        },
        onObs: applyObsEvent,
        onState: applyRow,
        onClosed: err => {
            streamEpoch += 1;
            clearResyncTimer();
            if (streamLive) {
                streamLive = false;
                publishAll();
            }
            if (!streamDownReported) {
                streamDownReported = true;
                console.warn(`[starobs] Event stream unavailable (${err.message}). Retrying.`);
            }
        },
    });
}

// One client per session id. Callers reuse it across renders and mounts.
export function getOBSClient(session: OBSSession): OBSWebSocketClient {
    const existing = obsClients.get(session.id);
    if (existing) return existing;
    const client = new OBSWebSocketClient(session);
    obsClients.set(session.id, client);
    return client;
}

// Drops clients for sessions that are no longer listed, and deletes their starobs sessions.
export function pruneOBSClients(keepIds: string[]): void {
    obsClients.forEach((client, id) => {
        if (keepIds.includes(id)) return;
        obsClients.delete(id);
        client.dispose();
        void transport().deleteSession(id).catch(err => console.warn(`[OBS ${id}] starobs delete failed: ${errorText(err)}`));
    });
}

function toSessionFields(session: OBSSession): OBSSession {
    return { id: session.id, name: session.name, host: session.host, port: session.port, autoConnect: session.autoConnect };
}

export class OBSWebSocketClient {
    // UI fields only. configure() sends the password to starobs and does not keep it.
    private session: OBSSession;
    private lastStatus: OBSSession;
    private lastHealth: OBSSessionHealth;
    private listeners = new Set<OBSClientListener>();
    private unsubscribe: (() => void) | null;

    constructor(session: OBSSession, onStatusChange?: (session: OBSSession) => void, onEvent?: OBSEventCallback) {
        this.session = toSessionFields(session);
        this.lastStatus = this.statusSnapshot();
        this.lastHealth = this.healthSnapshot();
        this.unsubscribe = subscribeToSession(this.session.id, update => this.handleHubUpdate(update));
        if (onStatusChange || onEvent) this.attach({ onStatusChange, onEvent });
        ensureEventStream();
    }

    // Returns a detach function. The current status is delivered at once.
    attach(listener: OBSClientListener): () => void {
        this.listeners.add(listener);
        this.deliverTo(listener, l => l.onStatusChange?.({ ...this.lastStatus }));
        this.deliverTo(listener, l => l.onHealthChange?.({ ...this.lastHealth }));
        return () => {
            this.listeners.delete(listener);
        };
    }

    dispose(): void {
        this.unsubscribe?.();
        this.unsubscribe = null;
        this.listeners.clear();
    }

    // Writes host, port, password and autoConnect to the starobs session. starobs reconnects only when host, port or password change.
    async configure(settings: StarobsSessionConfig): Promise<void> {
        this.session = {
            ...this.session,
            host: settings.host,
            port: settings.port,
            autoConnect: settings.autoConnect ?? this.session.autoConnect,
        };
        ensureEventStream();
        const row = await transport().putSession(this.session.id, {
            host: settings.host,
            port: settings.port,
            password: settings.password,
            autoConnect: this.session.autoConnect,
        });
        applyRow({ ...row, name: this.session.id });
    }

    async connect(): Promise<void> {
        ensureEventStream();
        await transport().connectSession(this.session.id);
    }

    // Sticky: starobs does not reconnect until connect() is called again.
    async disconnect(): Promise<void> {
        await transport().disconnectSession(this.session.id);
    }

    async refreshStatus(): Promise<void> {
        await resyncSession(this.session.id);
    }

    async sendRequest(requestType: string, requestData?: Record<string, unknown>): Promise<unknown> {
        return transport().request(this.session.id, requestType, requestData);
    }

    async call(requestType: string, requestData?: Record<string, unknown>): Promise<unknown> {
        return this.sendRequest(requestType, requestData);
    }

    // --- OBS Commands ---

    async getSceneList(): Promise<{ scenes: { sceneName: string }[]; currentProgramSceneName: string }> {
        return this.sendRequest('GetSceneList') as Promise<{ scenes: { sceneName: string }[]; currentProgramSceneName: string }>;
    }

    async setCurrentScene(sceneName: string): Promise<void> {
        await this.sendRequest('SetCurrentProgramScene', { sceneName });
    }

    async getSceneItemList(sceneName: string): Promise<{ sceneItems: Array<{ sceneItemId: number; sourceName: string; sceneItemEnabled: boolean }> }> {
        return this.sendRequest('GetSceneItemList', { sceneName }) as Promise<{ sceneItems: Array<{ sceneItemId: number; sourceName: string; sceneItemEnabled: boolean }> }>;
    }

    async getSceneCollections(): Promise<{ sceneCollections: string[]; currentSceneCollectionName: string }> {
        return this.sendRequest('GetSceneCollectionList') as Promise<{ sceneCollections: string[]; currentSceneCollectionName: string }>;
    }

    async setSceneCollection(collectionName: string): Promise<void> {
        await this.sendRequest('SetCurrentSceneCollection', { sceneCollectionName: collectionName });
    }

    async startRecord(): Promise<void> {
        await this.sendRequest('StartRecord');
    }

    async stopRecord(): Promise<void> {
        await this.sendRequest('StopRecord');
    }

    async toggleRecord(): Promise<void> {
        await this.sendRequest('ToggleRecord');
    }

    async startStream(): Promise<void> {
        await this.sendRequest('StartStream');
    }

    async stopStream(): Promise<void> {
        await this.sendRequest('StopStream');
    }

    async toggleStream(): Promise<void> {
        await this.sendRequest('ToggleStream');
    }

    // --- Source Filter Commands (for Branch Output plugin) ---

    async getSourceFilterList(sourceName: string): Promise<{ filters: Array<{ filterName: string; filterEnabled: boolean; filterKind: string }> }> {
        return this.sendRequest('GetSourceFilterList', { sourceName }) as Promise<{ filters: Array<{ filterName: string; filterEnabled: boolean; filterKind: string }> }>;
    }

    async getSourceFilter(sourceName: string, filterName: string): Promise<{ filterEnabled: boolean; filterIndex: number; filterKind: string; filterSettings: Record<string, unknown> }> {
        return this.sendRequest('GetSourceFilter', { sourceName, filterName }) as Promise<{ filterEnabled: boolean; filterIndex: number; filterKind: string; filterSettings: Record<string, unknown> }>;
    }

    async createSourceFilter(sourceName: string, filterName: string, filterKind: string, filterSettings?: Record<string, unknown>): Promise<void> {
        await this.sendRequest('CreateSourceFilter', {
            sourceName,
            filterName,
            filterKind,
            filterSettings: filterSettings || {},
        });
    }

    async removeSourceFilter(sourceName: string, filterName: string): Promise<void> {
        await this.sendRequest('RemoveSourceFilter', { sourceName, filterName });
    }

    async setSourceFilterEnabled(sourceName: string, filterName: string, filterEnabled: boolean): Promise<void> {
        await this.sendRequest('SetSourceFilterEnabled', {
            sourceName,
            filterName,
            filterEnabled,
        });
    }

    async setSourceFilterSettings(sourceName: string, filterName: string, filterSettings: Record<string, unknown>): Promise<void> {
        await this.sendRequest('SetSourceFilterSettings', {
            sourceName,
            filterName,
            filterSettings,
        });
    }

    // --- Enhanced OBS Commands ---

    async setSceneItemEnabled(sceneName: string, sourceName: string, enabled: boolean): Promise<void> {
        // First get the scene item ID
        const sceneItems = await this.getSceneItemList(sceneName);
        const item = sceneItems.sceneItems.find(i => i.sourceName === sourceName);
        if (!item) {
            throw new Error(`Source "${sourceName}" not found in scene "${sceneName}"`);
        }
        await this.sendRequest('SetSceneItemEnabled', {
            sceneName,
            sceneItemId: item.sceneItemId,
            sceneItemEnabled: enabled,
        });
    }

    async startRecordWithFilename(fileName?: string, directory?: string): Promise<void> {
        if (directory) {
            await this.sendRequest('SetRecordDirectory', { recordDirectory: directory });
        }
        if (fileName) {
            // OBS v5 uses SetRecordFilenameFormat for wildcard-based naming
            // But we can also set a custom output path
            await this.sendRequest('SetRecordFilenameFormat', { filenameFormatting: fileName });
        }
        await this.startRecord();
    }

    get isConnected(): boolean {
        return isLive(this.session.id);
    }

    get sessionInfo(): OBSSession {
        return this.statusSnapshot();
    }

    get health(): OBSSessionHealth {
        return this.healthSnapshot();
    }

    private statusSnapshot(): OBSSession {
        const live = isLive(this.session.id);
        const entry = hubSessions.get(this.session.id);
        return {
            ...this.session,
            isConnected: live,
            isRecording: live && entry?.recording === true,
            isStreaming: live && entry?.streaming === true,
        };
    }

    private healthSnapshot(): OBSSessionHealth {
        if (!streamLive) return { state: 'unknown', lastError: 'starobs is not reachable' };
        const row = hubSessions.get(this.session.id)?.row;
        return row ? { state: row.state, lastError: row.lastError ?? null } : { state: 'unknown', lastError: null };
    }

    private handleHubUpdate(update: HubUpdate): void {
        this.pushChanges();
        if (update.kind === 'event') {
            const { eventType, eventData } = update;
            this.deliver(listener => listener.onEvent?.(eventType, eventData));
        }
    }

    // Pushes only what changed, so an unchanged status does not re-render the parent.
    private pushChanges(): void {
        const status = this.statusSnapshot();
        if (
            status.isConnected !== this.lastStatus.isConnected ||
            status.isRecording !== this.lastStatus.isRecording ||
            status.isStreaming !== this.lastStatus.isStreaming
        ) {
            this.lastStatus = status;
            this.deliver(listener => listener.onStatusChange?.({ ...status }));
        }
        const health = this.healthSnapshot();
        if (health.state !== this.lastHealth.state || health.lastError !== this.lastHealth.lastError) {
            this.lastHealth = health;
            this.deliver(listener => listener.onHealthChange?.({ ...health }));
        }
    }

    private deliver(send: (listener: OBSClientListener) => void): void {
        this.listeners.forEach(listener => this.deliverTo(listener, send));
    }

    private deliverTo(listener: OBSClientListener, send: (listener: OBSClientListener) => void): void {
        try {
            send(listener);
        } catch (err) {
            console.error(`[OBS ${this.session.name}] Listener failed:`, err);
        }
    }
}
