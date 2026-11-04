/**
 * Remote API v2 — typed client for Play Play renderers.
 *
 * Talks to either:
 *   - this instance's own apiServer (default: http://localhost:3000)
 *   - any remote Play Play instance — by passing baseUrl + token
 *
 * Every method returns the standard envelope:
 *     { ok: true, data: T }        on success
 *     { ok: false, error: { code, message, ... } }  on failure
 *
 * This means callers never need try/catch — they just check `.ok`.
 *
 * The class instance is cheap; create one per remote you talk to.
 */

import type { PeerPlayoutState } from './simulatedPreview';
export type { PeerPlayoutState };

export type ApiOk<T> = { ok: true; data: T };
export type ApiErr = { ok: false; error: { code: string; message: string; httpStatus?: number; [k: string]: unknown } };
export type ApiResult<T> = ApiOk<T> | ApiErr;

export type Scope = 'read' | 'write' | 'admin' | 'playlist' | 'command' | 'file' | 'events';

export interface IdentityInfo {
    instanceId: string;
    displayName: string;
    hostname: string;
    platform: string;
    arch: string;
    appVersion: string;
    electronVersion: string;
    nodeVersion: string;
    uptimeSec: number;
    tags: string[];
    description: string;
    interfaces: Array<{ name: string; family: string; address: string; mac: string }>;
}

export interface TokenInfo {
    id: string;
    name: string;
    scopes: Scope[];
    createdAt: number;
    lastUsed: number | null;
    disabled: boolean;
}

export interface CreatedToken extends TokenInfo {
    /** Returned ONCE at creation. Save it — it's not retrievable later. */
    secret: string;
}

export interface InstanceRef {
    id: string;
    name: string;
    baseUrl: string;
    token: string | null;       // '***' for masked
    instanceId: string | null;
    addedAt: number;
    lastSeen: number | null;
    lastError: string | null;
}

export interface Group {
    id: string;
    name: string;
    description: string;
    memberIds: string[];
    createdAt: number;
}

export interface GroupWithMembers extends Group {
    members: InstanceRef[];
}

export interface Target {
    instanceId?: string;
    instanceIds?: string[];
    groupId?: string;
}

export interface ProbeResult {
    ok: boolean;
    instanceId: string;
    name: string;
    baseUrl: string;
    rttMs: number;
    identity?: IdentityInfo;
    schedule?: unknown;
    current?: unknown;
    error?: { code: string; message: string };
}

export interface CommandResult {
    instanceId: string;
    instanceName: string;
    result: ApiResult<unknown>;
}

export interface DiscoveryPeer {
    instanceId: string;
    displayName: string;
    hostname: string;
    address: string;
    apiPort: number;
    lastSeen: number;
}

export interface CatalogItem {
    id: string;
    title: string;
    hash: string | null;
    sizeBytes: number | null;
    duration: number;
    type: string;
}

export interface RemoteEvent<T = unknown> {
    topic: string;
    t: number;
    instanceId: string;
    data: T;
}

export type EventHandler = (evt: RemoteEvent) => void;

export interface RemoteApiOptions {
    baseUrl?: string;
    token?: string;
    /** Default per-call timeout, ms */
    timeoutMs?: number;
}

export class RemoteApiClient {
    private baseUrl: string;
    private token: string;
    private timeoutMs: number;

    constructor(opts: RemoteApiOptions = {}) {
        this.baseUrl = (opts.baseUrl || 'http://localhost:3000').replace(/\/+$/, '');
        this.token = opts.token || '';
        this.timeoutMs = opts.timeoutMs ?? 8000;
    }

    setToken(token: string): void { this.token = token; }
    setBaseUrl(url: string): void { this.baseUrl = url.replace(/\/+$/, ''); }

    private async call<T = unknown>(method: string, path: string, body?: unknown, opts?: { timeoutMs?: number; raw?: boolean; headers?: Record<string, string> }): Promise<ApiResult<T>> {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), opts?.timeoutMs ?? this.timeoutMs);
        try {
            const init: RequestInit = {
                method,
                headers: {
                    'Content-Type': 'application/json',
                    ...(this.token ? { Authorization: `Bearer ${this.token}` } : {}),
                    ...(opts?.headers || {}),
                },
                signal: controller.signal,
            };
            if (body !== undefined && method !== 'GET' && method !== 'HEAD') {
                init.body = body instanceof ArrayBuffer || body instanceof Uint8Array || typeof body === 'string'
                    ? (body as BodyInit)
                    : JSON.stringify(body);
            }
            const res = await fetch(this.baseUrl + path, init);
            const text = await res.text();
            let parsed: any = null;
            try { parsed = text ? JSON.parse(text) : null; } catch { /* not JSON */ }
            if (!res.ok) {
                return {
                    ok: false,
                    error: {
                        code: parsed?.error?.code || `http_${res.status}`,
                        message: parsed?.error?.message || `HTTP ${res.status}`,
                        httpStatus: res.status,
                    },
                };
            }
            if (parsed && typeof parsed === 'object' && 'ok' in parsed) return parsed as ApiResult<T>;
            return { ok: true, data: parsed as T };
        } catch (err) {
            const e = err as Error;
            return {
                ok: false,
                error: {
                    code: e.name === 'AbortError' ? 'timeout' : 'network_error',
                    message: e.message,
                },
            };
        } finally {
            clearTimeout(timer);
        }
    }

    // ── Identity ────────────────────────────────────────────────────────────
    identity()                                       { return this.call<IdentityInfo>('GET',   '/api/v2/identity'); }
    patchIdentity(p: Partial<Pick<IdentityInfo, 'displayName' | 'tags' | 'description'>>) {
        return this.call<IdentityInfo>('PATCH', '/api/v2/identity', p);
    }

    // ── Auth / tokens ───────────────────────────────────────────────────────
    listTokens()                                     { return this.call<TokenInfo[]>('GET',   '/api/v2/auth/tokens'); }
    createToken(name: string, scopes: Scope[])       { return this.call<CreatedToken>('POST', '/api/v2/auth/tokens', { name, scopes }); }
    revokeToken(id: string)                          { return this.call<{ revoked: string }>('DELETE', `/api/v2/auth/tokens/${id}`); }
    setTokenDisabled(id: string, disabled: boolean)  { return this.call<{ id: string; disabled: boolean }>('PATCH', `/api/v2/auth/tokens/${id}`, { disabled }); }
    currentToken()                                   { return this.call<{ id: string; name: string; scopes: Scope[] }>('GET', '/api/v2/auth/current'); }

    // ── Instances (the address book) ────────────────────────────────────────
    listInstances()                                  { return this.call<InstanceRef[]>('GET', '/api/v2/instances'); }
    getInstance(id: string)                          { return this.call<InstanceRef>('GET',   `/api/v2/instances/${id}`); }
    addInstance(p: { name: string; baseUrl: string; token?: string; instanceId?: string }) {
        return this.call<InstanceRef>('POST', '/api/v2/instances', p);
    }
    updateInstance(id: string, p: Partial<{ name: string; baseUrl: string; token: string }>) {
        return this.call<InstanceRef>('PATCH', `/api/v2/instances/${id}`, p);
    }
    removeInstance(id: string)                       { return this.call<{ removed: string }>('DELETE', `/api/v2/instances/${id}`); }
    health(id: string)                               { return this.call<ProbeResult>('GET',   `/api/v2/instances/${id}/health`); }
    probe(id: string)                                { return this.call<ProbeResult>('GET',   `/api/v2/instances/${id}/probe`); }
    probeBatch(target: Target, deep = false)         { return this.call<ProbeResult[]>('POST','/api/v2/probe-batch', { ...target, deep }); }

    // ── Groups ──────────────────────────────────────────────────────────────
    listGroups()                                     { return this.call<Group[]>('GET',  '/api/v2/groups'); }
    getGroup(id: string)                             { return this.call<GroupWithMembers>('GET', `/api/v2/groups/${id}`); }
    createGroup(name: string, description = '')      { return this.call<Group>('POST', '/api/v2/groups', { name, description }); }
    updateGroup(id: string, p: Partial<{ name: string; description: string }>) {
        return this.call<Group>('PATCH', `/api/v2/groups/${id}`, p);
    }
    deleteGroup(id: string)                          { return this.call<{ removed: string }>('DELETE', `/api/v2/groups/${id}`); }
    addMember(groupId: string, instanceId: string)   { return this.call<Group>('POST',   `/api/v2/groups/${groupId}/members`, { instanceId }); }
    removeMember(groupId: string, instanceId: string){ return this.call<Group>('DELETE', `/api/v2/groups/${groupId}/members/${instanceId}`); }

    // ── Schedules ───────────────────────────────────────────────────────────
    getSchedule()                                    { return this.call<unknown>('GET', '/api/v2/schedule/current'); }
    putSchedule(schedule: unknown)                   { return this.call<{ received: boolean; slots: number }>('PUT', '/api/v2/schedule/current', schedule); }
    pushSchedule(target: Target, schedule?: unknown) { return this.call<CommandResult[]>('POST', '/api/v2/schedule/push', { ...target, schedule }); }
    pullSchedule(fromInstanceId: string)             { return this.call<{ stored: boolean; source: string }>('POST', '/api/v2/schedule/pull', { instanceId: fromInstanceId }); }

    // ── Playlists ───────────────────────────────────────────────────────────
    listPlaylists()                                  { return this.call<unknown[]>('GET',   '/api/v2/playlists'); }
    getPlaylist(id: string)                          { return this.call<unknown>('GET',     `/api/v2/playlists/${id}`); }
    putPlaylist(id: string, playlist: unknown)       { return this.call<unknown>('PUT',     `/api/v2/playlists/${id}`, playlist); }
    pushPlaylist(playlistId: string, target: Target, playlist?: unknown) {
        return this.call<CommandResult[]>('POST', '/api/v2/playlists/push', { playlistId, target, playlist });
    }

    // ── Commands ────────────────────────────────────────────────────────────
    listCommandTypes()                               { return this.call<{ types: string[] }>('GET', '/api/v2/commands/types'); }
    dispatch(type: string, params?: unknown, target?: Target) {
        return this.call<CommandResult[]>('POST', '/api/v2/commands/dispatch', { type, params, target });
    }

    // ── Files ───────────────────────────────────────────────────────────────
    catalog()                                        { return this.call<CatalogItem[]>('GET', '/api/v2/files/catalog'); }
    pushFile(filePath: string, target: Target)       { return this.call<unknown>('POST',     '/api/v2/files/push', { filePath, target }); }
    /** Upload raw bytes (for direct browser→server uploads). */
    uploadBytes(filename: string, bytes: ArrayBuffer | Uint8Array, hash?: string) {
        return this.call<{ storedAt: string; hash: string; sizeBytes: number }>(
            'POST',
            '/api/v2/files/upload',
            bytes,
            {
                headers: {
                    'Content-Type': 'application/octet-stream',
                    'X-File-Name': filename,
                    ...(hash ? { 'X-File-Hash': hash } : {}),
                },
                timeoutMs: 60000,
            },
        );
    }

    // ── Playout state (for simulated confidence previews) ───────────────────
    getPlayoutCurrent()                              { return this.call<PeerPlayoutState>('GET', '/api/v2/playout/current'); }
    getPlayoutCurrentBatch(target: Target) {
        return this.call<Array<{ instanceRefId: string; instanceName: string; baseUrl: string; ok: boolean; state: PeerPlayoutState | null; error: { code: string; message: string } | null }>>(
            'POST', '/api/v2/playout/current-batch', target
        );
    }

    // ── Discovery ───────────────────────────────────────────────────────────
    listDiscoveredPeers()                            { return this.call<DiscoveryPeer[]>('GET', '/api/v2/discovery/peers'); }

    // ── Events ──────────────────────────────────────────────────────────────
    recentEvents(opts?: { limit?: number; topics?: string }) {
        const q = new URLSearchParams();
        if (opts?.limit) q.set('limit', String(opts.limit));
        if (opts?.topics) q.set('topics', opts.topics);
        const qs = q.toString();
        return this.call<RemoteEvent[]>('GET', `/api/v2/events/recent${qs ? `?${qs}` : ''}`);
    }

    /**
     * Open a WebSocket subscription to live events.
     * Returns an object with { close }; pass an onEvent callback.
     *
     * topics: comma-separated, e.g. "schedule.*,command.*". '*' = all.
     */
    subscribeEvents(opts: { onEvent: EventHandler; topics?: string; replay?: number; onClose?: (e: CloseEvent) => void; onError?: (e: Event) => void }): { close(): void } {
        const wsBase = this.baseUrl.replace(/^http/, 'ws');
        const params = new URLSearchParams();
        params.set('token', this.token);
        if (opts.topics) params.set('topics', opts.topics);
        if (opts.replay) params.set('replay', String(opts.replay));
        const ws = new WebSocket(`${wsBase}/api/v2/events?${params.toString()}`);
        ws.onmessage = (m) => {
            try { opts.onEvent(JSON.parse(m.data)); } catch { /* ignore */ }
        };
        if (opts.onClose) ws.onclose = opts.onClose;
        if (opts.onError) ws.onerror = opts.onError;
        return { close: () => { try { ws.close(); } catch { /* */ } } };
    }
}

/** Convenience: a singleton client pointed at this instance's local API. */
export const localRemoteApi = new RemoteApiClient({ baseUrl: 'http://localhost:3000' });
