// ============================================
// starobs serve client (contract v1, docs/STAROBS-SERVE.md)
// HTTP for requests, SSE for events. starobs owns OBS sockets,
// reconnect, backoff and auth. This module only transports and parses.
// ============================================

export const DEFAULT_STAROBS_BASE_URL = 'http://127.0.0.1:7790';

const BASE_URL_STORAGE_KEY = 'starobs.baseUrl';
const LOOPBACK_HOSTS = new Set(['127.0.0.1', 'localhost', '[::1]']);
const HTTP_TIMEOUT_MS = 10000;
const OBS_REQUEST_TIMEOUT_MS = 10000;
// starobs answers 504 at timeoutMs. The HTTP abort fires later, so a hung serve process still ends the call.
const OBS_REQUEST_HTTP_SLACK_MS = 2000;
const EVENT_RETRY_MS = 2000;

export type StarobsSessionState = 'idle' | 'connecting' | 'identified' | 'reconnecting' | 'stopped' | 'auth_failed';

export interface StarobsSessionRow {
    name: string;
    host: string;
    port: number;
    autoConnect: boolean;
    hasPassword: boolean;
    state: StarobsSessionState;
    lastError: string | null;
    retries: number;
    obsVersion: string | null;
    rpcVersion: number | null;
    connectedAt: string | number | null;
}

export interface StarobsSnapshot {
    state: StarobsSessionState;
    scenes: Array<{ name: string; index: number }>;
    currentScene: string | null;
    previewScene: string | null;
    streaming: boolean;
    recording: boolean;
    virtualCam: boolean;
    replayBuffer: boolean;
    studioMode: boolean;
}

export interface StarobsSessionConfig {
    host: string;
    port: number;
    password?: string;
    autoConnect?: boolean;
}

export interface StarobsObsEvent {
    session: string;
    eventType: string;
    eventIntent: number;
    eventData: Record<string, unknown>;
}

export interface StarobsEventHandlers {
    onOpen: () => void;
    onObs: (event: StarobsObsEvent) => void;
    onState: (row: StarobsSessionRow) => void;
    onClosed: (error: Error) => void;
}

export interface StarobsEventStream {
    close: () => void;
}

export type StarobsErrorCode =
    | 'bad_request'
    | 'unknown_session'
    | 'not_identified'
    | 'timeout'
    | 'obs_error'
    | 'unreachable'
    | 'http_error'
    | 'bad_response';

// For obs_error, obsCode is the OBS status code and the message carries the OBS comment.
export class StarobsError extends Error {
    readonly code: StarobsErrorCode;
    readonly status: number;
    readonly obsCode?: number;

    constructor(code: StarobsErrorCode, message: string, status = 0, obsCode?: number) {
        super(message);
        this.name = 'StarobsError';
        this.code = code;
        this.status = status;
        this.obsCode = obsCode;
    }
}

export function resolveStarobsBaseUrl(): string {
    let override: string | null = null;
    try {
        override = globalThis.localStorage?.getItem(BASE_URL_STORAGE_KEY) ?? null;
    } catch {
        override = null;
    }
    if (!override) return DEFAULT_STAROBS_BASE_URL;
    try {
        const url = new URL(override);
        // The OBS password is sent to this host, so only loopback is accepted.
        if (url.protocol === 'http:' && LOOPBACK_HOSTS.has(url.hostname)) return url.origin;
    } catch {
        // Not a URL. Use the default.
    }
    console.warn(`[starobs] ${BASE_URL_STORAGE_KEY} must be a loopback http URL. Using ${DEFAULT_STAROBS_BASE_URL}.`);
    return DEFAULT_STAROBS_BASE_URL;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function sessionPath(name: string): string {
    return `/sessions/${encodeURIComponent(name)}`;
}

function codeForStatus(status: number): StarobsErrorCode {
    if (status === 400) return 'bad_request';
    if (status === 404) return 'unknown_session';
    if (status === 409) return 'not_identified';
    return 'http_error';
}

function parseResponse<T>(status: number, text: string, requestType?: string): T {
    let body: unknown = null;
    if (text) {
        try {
            body = JSON.parse(text);
        } catch {
            body = null;
        }
    }
    if (status === 504) throw new StarobsError('timeout', 'Request timeout', status);
    if (status >= 400) {
        const detail = isRecord(body) && typeof body.error === 'string' ? body.error : `HTTP ${status}`;
        throw new StarobsError(codeForStatus(status), detail, status);
    }
    if (!isRecord(body)) throw new StarobsError('bad_response', `starobs sent an invalid response (HTTP ${status})`, status);
    if (body.ok === false) {
        const obsCode = typeof body.code === 'number' ? body.code : undefined;
        const comment = typeof body.error === 'string' ? body.error : 'no detail';
        const suffix = obsCode === undefined ? '' : ` (OBS code ${obsCode})`;
        throw new StarobsError('obs_error', `${requestType ?? 'Request'} failed${suffix}: ${comment}`, status, obsCode);
    }
    return body as T;
}

export class StarobsClient {
    readonly baseUrl: string;

    constructor(baseUrl: string) {
        this.baseUrl = baseUrl.replace(/\/+$/, '');
    }

    async listSessions(): Promise<StarobsSessionRow[]> {
        const body = await this.send<{ sessions?: unknown }>('GET', '/sessions');
        return Array.isArray(body.sessions) ? (body.sessions as StarobsSessionRow[]) : [];
    }

    putSession(name: string, config: StarobsSessionConfig): Promise<StarobsSessionRow> {
        const payload: Record<string, unknown> = { host: config.host, port: config.port };
        if (config.password !== undefined) payload.password = config.password;
        if (config.autoConnect !== undefined) payload.autoConnect = config.autoConnect;
        return this.send<StarobsSessionRow>('PUT', sessionPath(name), payload);
    }

    async deleteSession(name: string): Promise<void> {
        await this.send('DELETE', sessionPath(name));
    }

    async connectSession(name: string): Promise<void> {
        await this.send('POST', `${sessionPath(name)}/connect`);
    }

    async disconnectSession(name: string): Promise<void> {
        await this.send('POST', `${sessionPath(name)}/disconnect`);
    }

    snapshot(name: string): Promise<StarobsSnapshot> {
        return this.send<StarobsSnapshot>('GET', `${sessionPath(name)}/snapshot`);
    }

    async request(name: string, requestType: string, requestData?: Record<string, unknown>): Promise<Record<string, unknown>> {
        const body = await this.send<Record<string, unknown>>(
            'POST',
            `${sessionPath(name)}/request?timeoutMs=${OBS_REQUEST_TIMEOUT_MS}`,
            { requestType, requestData: requestData ?? {} },
            OBS_REQUEST_TIMEOUT_MS + OBS_REQUEST_HTTP_SLACK_MS,
            requestType,
        );
        return isRecord(body.responseData) ? body.responseData : {};
    }

    openEvents(handlers: StarobsEventHandlers, retryMs = EVENT_RETRY_MS): StarobsEventStream {
        let closed = false;
        let controller: AbortController | null = null;
        let timer: ReturnType<typeof setTimeout> | null = null;
        let wake: (() => void) | null = null;

        const dispatch = (name: string, raw: string): void => {
            let payload: unknown;
            try {
                payload = JSON.parse(raw);
            } catch {
                console.warn(`[starobs] Dropped a malformed "${name}" event.`);
                return;
            }
            if (!isRecord(payload)) return;
            try {
                if (name === 'obs') handlers.onObs(payload as unknown as StarobsObsEvent);
                if (name === 'state') handlers.onState(payload as unknown as StarobsSessionRow);
            } catch (err) {
                console.error('[starobs] Event handler failed:', err instanceof Error ? err.message : err);
            }
        };

        const pump = async (): Promise<void> => {
            controller = new AbortController();
            const response = await fetch(`${this.baseUrl}/v1/events`, {
                headers: { Accept: 'text/event-stream' },
                signal: controller.signal,
            });
            if (!response.ok || !response.body) {
                throw new StarobsError('http_error', `event stream rejected (HTTP ${response.status})`, response.status);
            }
            handlers.onOpen();
            const reader = response.body.getReader();
            const decoder = new TextDecoder();
            let buffer = '';
            let eventName = 'message';
            let data: string[] = [];
            for (;;) {
                const chunk = await reader.read();
                if (chunk.done) throw new StarobsError('unreachable', 'event stream ended');
                buffer += decoder.decode(chunk.value, { stream: true });
                for (let end = buffer.indexOf('\n'); end >= 0; end = buffer.indexOf('\n')) {
                    const line = buffer.slice(0, end).replace(/\r$/, '');
                    buffer = buffer.slice(end + 1);
                    if (line === '') {
                        if (data.length > 0) dispatch(eventName, data.join('\n'));
                        eventName = 'message';
                        data = [];
                    } else if (line.startsWith('event:')) {
                        eventName = line.slice(6).trim();
                    } else if (line.startsWith('data:')) {
                        data.push(line.slice(5).replace(/^ /, ''));
                    }
                }
            }
        };

        const run = async (): Promise<void> => {
            while (!closed) {
                try {
                    await pump();
                } catch (err) {
                    if (closed) return;
                    handlers.onClosed(err instanceof Error ? err : new Error(String(err)));
                }
                if (closed) return;
                await new Promise<void>(resolve => {
                    wake = () => resolve();
                    timer = setTimeout(() => resolve(), retryMs);
                });
                wake = null;
                timer = null;
            }
        };

        void run();

        return {
            close: () => {
                closed = true;
                controller?.abort();
                if (timer) clearTimeout(timer);
                wake?.();
            },
        };
    }

    private async send<T>(method: string, path: string, payload?: unknown, timeoutMs = HTTP_TIMEOUT_MS, requestType?: string): Promise<T> {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        try {
            const response = await fetch(`${this.baseUrl}/v1${path}`, {
                method,
                headers: payload === undefined ? undefined : { 'Content-Type': 'application/json' },
                body: payload === undefined ? undefined : JSON.stringify(payload),
                signal: controller.signal,
            });
            return parseResponse<T>(response.status, await response.text(), requestType);
        } catch (err) {
            if (err instanceof StarobsError) throw err;
            if (controller.signal.aborted) throw new StarobsError('timeout', 'Request timeout');
            throw new StarobsError('unreachable', `starobs is not reachable at ${this.baseUrl}`);
        } finally {
            clearTimeout(timer);
        }
    }
}
