/**
 * "Hermes supervisor" = PLAY PLAY internal supervised OpenRouter loop (bounded validate/retry).
 * Not Meta React Native Hermes; not Nous Research Hermes Agent CLI — see docs/HERMES_NOUS_AGENT.md.
 * @see docs/HERMES_SUPERVISOR.md
 */

import { getElectronApi } from './aiHelper';

export const HERMES_SUPERVISOR_LS = 'playplay_hermes_supervisor';
export const HERMES_MODEL_OVERRIDE_LS = 'playplay_hermes_model';

let envDefaultCached: boolean | null = null;
let envLoadPromise: Promise<boolean> | null = null;

function readLs(key: string): string | null {
    if (typeof localStorage === 'undefined') return null;
    try {
        return localStorage.getItem(key);
    } catch {
        return null;
    }
}

/** Explicit `0` disables even when PLAYPLAY_HERMES_SUPERVISOR=1 in main env. */
export function isHermesSupervisorEnabledSync(fromEnvDefault = false): boolean {
    const v = readLs(HERMES_SUPERVISOR_LS);
    if (v === '0') return false;
    if (v === '1') return true;
    if (envDefaultCached !== null) return envDefaultCached;
    return fromEnvDefault;
}

export async function loadHermesSupervisorEnvDefault(): Promise<boolean> {
    if (envDefaultCached !== null) return envDefaultCached;
    if (!envLoadPromise) {
        envLoadPromise = (async () => {
            const api = getElectronApi()?.hermesSupervisor?.getEnvDefault;
            if (!api) {
                envDefaultCached = false;
                return false;
            }
            try {
                const r = await api();
                envDefaultCached = !!r?.fromEnv;
                return envDefaultCached;
            } catch {
                envDefaultCached = false;
                return false;
            }
        })();
    }
    envDefaultCached = await envLoadPromise;
    return envDefaultCached;
}

export async function isHermesSupervisorEnabled(): Promise<boolean> {
    const fromEnv = await loadHermesSupervisorEnvDefault();
    return isHermesSupervisorEnabledSync(fromEnv);
}

/**
 * Hermes supervisor model: override from localStorage, else caller default, else omit so main uses OPENROUTER_MODEL / built-in.
 */
export function getHermesSupervisorModelOrDefault(defaultModel: string | undefined): string | undefined {
    const o = readLs(HERMES_MODEL_OVERRIDE_LS)?.trim();
    if (o) return o;
    const d = typeof defaultModel === 'string' ? defaultModel.trim() : '';
    return d || undefined;
}
