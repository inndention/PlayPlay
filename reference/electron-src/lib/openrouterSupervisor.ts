/**
 * Bounded OpenRouter supervisor: plan (system) → call → validate JSON → optional retries with compressed critique.
 * Internal "Hermes supervisor" only — not Nous Hermes Agent CLI (docs/HERMES_NOUS_AGENT.md).
 * @see docs/HERMES_SUPERVISOR.md
 */

import { getElectronApi, getStoredOpenRouterCredentials, type OpenRouterJsonRequest } from './aiHelper';
import { HERMES_MODEL_OVERRIDE_LS, loadHermesSupervisorEnvDefault } from './hermesSupervisorFlags';

/** Total OpenRouter calls per supervised run (initial + retries). Invariant: never more than this. */
export const HERMES_SUPERVISOR_MAX_ATTEMPTS = 3;

/** Hard cap passed to main for max_tokens (main may still apply its own defaults). */
export const HERMES_SUPERVISOR_MAX_OUTPUT_TOKENS_CAP = 4096;

const LAST_RUN_LS = 'playplay_hermes_last_run';

const RE_SECRET = /sk-or-v1-[a-zA-Z0-9_-]{10,}|Bearer\s+[a-zA-Z0-9._-]+|api[_-]?key["']?\s*[:=]\s*["'][^"']{8,}["']/gi;

export function redactSecrets(text: string): string {
    if (!text) return '';
    return text
        .replace(RE_SECRET, '[REDACTED]')
        .replace(/sk-[a-zA-Z0-9]{10,}/g, '[REDACTED]');
}

export function compressCritique(message: string, maxLen = 420): string {
    const oneLine = message.replace(/\s+/g, ' ').trim();
    if (oneLine.length <= maxLen) return oneLine;
    return `${oneLine.slice(0, maxLen - 1)}…`;
}

export interface OpenRouterJsonEnvelope {
    success: boolean;
    error?: string;
    parsed?: unknown;
    content?: string;
    model?: string;
}

export interface SupervisorJsonOptions<T = unknown> {
    system: string;
    /** Build user message; receives prior validation critiques (newest last), each compressed. */
    buildUserPrompt: (critiques: string[]) => string;
    validate: (parsed: unknown) => { ok: boolean; critique?: string };
    model?: string;
    temperature?: number;
    maxTokens?: number;
    /** For durable learning line tag, e.g. scheduling_agent */
    learningTag: string;
    apiKey?: string;
}

export interface SupervisorJsonResult<T = unknown> {
    success: boolean;
    data: T | null;
    attempts: number;
    critiques: string[];
    lastError?: string;
}

async function invokeOpenRouterJson(request: OpenRouterJsonRequest): Promise<OpenRouterJsonEnvelope> {
    const api = getElectronApi()?.ai?.openRouterJson;
    if (!api) {
        return { success: false, error: 'electronAPI.ai.openRouterJson unavailable' };
    }
    const stored = getStoredOpenRouterCredentials();
    const apiKey = (request.apiKey && request.apiKey.trim()) || stored.apiKey;
    const reqModel = typeof request.model === 'string' ? request.model.trim() : '';
    const model = reqModel || stored.model.trim() || undefined;
    const result = await api({
        ...request,
        apiKey: apiKey || undefined,
        model,
    });
    return result as OpenRouterJsonEnvelope;
}

function persistLastRun(summary: Record<string, unknown>): void {
    if (typeof localStorage === 'undefined') return;
    try {
        const safe = JSON.parse(JSON.stringify(summary)) as Record<string, unknown>;
        if (typeof safe.lastError === 'string') safe.lastError = redactSecrets(safe.lastError);
        localStorage.setItem(LAST_RUN_LS, JSON.stringify({ ...safe, ts: Date.now() }));
    } catch {
        /* noop */
    }
}

async function appendLearningLine(line: string): Promise<void> {
    const api = getElectronApi()?.hermesSupervisor?.appendLearning;
    if (!api) return;
    const trimmed = redactSecrets(line).slice(0, 1800);
    try {
        await api(trimmed);
    } catch {
        /* non-fatal */
    }
}

/**
 * Runs up to HERMES_SUPERVISOR_MAX_ATTEMPTS OpenRouter JSON calls with the same system prompt
 * and growing user prompt (critiques appended via buildUserPrompt).
 */
export async function runOpenRouterSupervisorJson<T = unknown>(options: SupervisorJsonOptions<T>): Promise<SupervisorJsonResult<T>> {
    await loadHermesSupervisorEnvDefault();

    const critiques: string[] = [];
    let lastError: string | undefined;
    let attempts = 0;

    const capTokens = Math.min(
        options.maxTokens ?? 900,
        HERMES_SUPERVISOR_MAX_OUTPUT_TOKENS_CAP,
    );

    const stored = getStoredOpenRouterCredentials();
    let modelOverride = '';
    try {
        modelOverride = typeof localStorage !== 'undefined' ? localStorage.getItem(HERMES_MODEL_OVERRIDE_LS)?.trim() || '' : '';
    } catch {
        modelOverride = '';
    }
    const optModel = typeof options.model === 'string' ? options.model.trim() : '';
    const model = modelOverride || optModel || stored.model.trim() || undefined;

    for (let attempt = 0; attempt < HERMES_SUPERVISOR_MAX_ATTEMPTS; attempt++) {
        attempts = attempt + 1;
        const prompt = options.buildUserPrompt(critiques);
        const env: OpenRouterJsonEnvelope = await invokeOpenRouterJson({
            system: options.system,
            prompt,
            model,
            temperature: options.temperature,
            maxTokens: capTokens,
            apiKey: options.apiKey,
        });

        if (!env.success) {
            lastError = env.error || 'OpenRouter request failed';
            const crit = compressCritique(lastError);
            critiques.push(crit);
            continue;
        }

        const parsed = env.parsed ?? (typeof env.content === 'string' ? tryParseJson(env.content) : null);
        const v = options.validate(parsed);
        if (v.ok) {
            persistLastRun({
                tag: options.learningTag,
                attempts,
                success: true,
                model: env.model,
            });
            const iso = new Date().toISOString();
            void appendLearningLine(
                `${iso}|${options.learningTag}|ok|attempts=${attempts}|model=${env.model || model}`,
            );
            return { success: true, data: parsed as T, attempts, critiques };
        }

        const c = compressCritique(v.critique || 'Validation failed: output did not match required schema.');
        critiques.push(c);
        lastError = c;
    }

    persistLastRun({
        tag: options.learningTag,
        attempts,
        success: false,
        phase: 'exhausted',
        lastError,
    });
    const iso = new Date().toISOString();
    void appendLearningLine(
        `${iso}|${options.learningTag}|fail|attempts=${attempts}|${lastError || 'unknown'}`,
    );

    return { success: false, data: null, attempts, critiques, lastError };
}

function tryParseJson(text: string): unknown {
    try {
        return JSON.parse(text);
    } catch {
        return null;
    }
}
