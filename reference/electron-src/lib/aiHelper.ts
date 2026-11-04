import { MediaItem, MediaStats, MediaType } from './types';

export interface ImportCandidateFile {
    id: string;
    filename: string;
    filePath?: string;
    type?: MediaType;
    duration?: number;
}

export interface ImportClusterSuggestion {
    id: string;
    label: string;
    kind: 'client' | 'event' | 'sponsor' | 'program_series';
    confidence: number;
    files: ImportCandidateFile[];
    reason: string;
    question: string;
}

export interface ImportInsights {
    suggestions: ImportClusterSuggestion[];
    notes: string[];
}

/** Canonical Settings key; legacy `playplay_openrouter_key` is still read for migration. */
export const OPENROUTER_LS_API_KEY = 'openrouter_api_key';
export const OPENROUTER_LS_API_KEY_LEGACY = 'playplay_openrouter_key';
export const OPENROUTER_LS_MODEL = 'openrouter_model';

/**
 * Built-in default when OPENROUTER_MODEL is unset and Settings default model is blank.
 * OpenRouter model page (pricing + context change over time): https://openrouter.ai/xiaomi/mimo-v2-flash
 * Sync: OPENROUTER_BUILTIN_MODEL in electron/main.js — docs/OPENROUTER.md
 */
export const OPENROUTER_DEFAULT_MODEL = 'xiaomi/mimo-v2-flash';

/** Default API host when `OPENROUTER_BASE_URL` is unset — sync `OPENROUTER_DEFAULT_BASE_URL` in electron/main.js */
export const OPENROUTER_DEFAULT_BASE_URL = 'https://api.openrouter.ai';

/**
 * Public OpenRouter HTTP base (no `/api/v1` suffix) for renderer-side clients.
 * Prefers main-process `ai:openrouter-config`, then `VITE_OPENROUTER_BASE_URL`, then default.
 */
export async function fetchOpenRouterPublicBaseUrl(): Promise<string> {
    const api = getElectronApi()?.ai;
    if (api?.openRouterConfig) {
        try {
            const c = await api.openRouterConfig();
            if (c?.baseUrl && typeof c.baseUrl === 'string' && c.baseUrl.trim()) {
                return c.baseUrl.trim().replace(/\/+$/, '');
            }
        } catch {
            /* noop */
        }
    }
    try {
        const v = import.meta.env?.VITE_OPENROUTER_BASE_URL;
        if (v && String(v).trim()) return String(v).trim().replace(/\/+$/, '');
    } catch {
        /* noop */
    }
    return OPENROUTER_DEFAULT_BASE_URL;
}

/** Placeholder in Settings when model field is empty (Vite-only hint; main still uses OPENROUTER_MODEL). */
export function getOpenRouterModelPlaceholderHint(): string {
    try {
        const v = import.meta.env?.VITE_OPENROUTER_MODEL;
        if (typeof v === 'string' && v.trim()) return v.trim();
    } catch {
        /* non-Vite graphs */
    }
    return OPENROUTER_DEFAULT_MODEL;
}

export interface OpenRouterJsonRequest {
    system?: string;
    prompt: string;
    model?: string;
    temperature?: number;
    maxTokens?: number;
    /** Explicit override; otherwise key from Settings localStorage or main `OPENROUTER_API_KEY`. */
    apiKey?: string;
}

export function getStoredOpenRouterCredentials(): { apiKey: string; model: string } {
    if (typeof localStorage === 'undefined') {
        return { apiKey: '', model: '' };
    }
    try {
        const apiKey = (
            localStorage.getItem(OPENROUTER_LS_API_KEY) ||
            localStorage.getItem(OPENROUTER_LS_API_KEY_LEGACY) ||
            ''
        ).trim();
        const model = (localStorage.getItem(OPENROUTER_LS_MODEL) || '').trim();
        return { apiKey, model };
    } catch {
        return { apiKey: '', model: '' };
    }
}

const STOPWORDS = new Set([
    'the', 'and', 'for', 'with', 'from', 'into', 'final', 'edit', 'copy', 'new',
    'spot', 'spots', 'commercial', 'promo', 'program', 'show', 'episode', 'ep',
    'bumper', 'bump', 'id', 'station', 'news', 'video', 'master', 'render',
    'export', 'rev', 'v', 'hd', 'uhd', 'sd', 'mp4', 'mov', 'mxf', 'mpg',
]);

function stripExtension(filename: string): string {
    return filename.replace(/\.[^/.]+$/, '');
}

function titleCase(value: string): string {
    return value
        .split(/\s+/)
        .filter(Boolean)
        .map(part => part.charAt(0).toUpperCase() + part.slice(1).toLowerCase())
        .join(' ');
}

function tokenize(filename: string): string[] {
    return stripExtension(filename)
        .replace(/([a-z])([A-Z])/g, '$1 $2')
        .toLowerCase()
        .split(/[^a-z0-9]+/)
        .map(token => token.trim())
        .filter(token => token.length >= 3 && !STOPWORDS.has(token) && !/^\d+$/.test(token));
}

function phraseCandidates(tokens: string[]): string[] {
    const phrases = new Set<string>();
    tokens.forEach(token => phrases.add(token));

    for (let i = 0; i < tokens.length - 1; i++) {
        const left = tokens[i];
        const right = tokens[i + 1];
        if (!STOPWORDS.has(left) && !STOPWORDS.has(right)) {
            phrases.add(`${left} ${right}`);
        }
    }

    return Array.from(phrases);
}

function inferKind(label: string, files: ImportCandidateFile[]): ImportClusterSuggestion['kind'] {
    const combined = `${label} ${files.map(file => file.filename).join(' ')}`.toLowerCase();
    const adLikeCount = files.filter(file => file.type === 'ad').length;
    const programLikeCount = files.filter(file => file.type === 'program' || file.type === 'paid_program').length;

    if (combined.includes('sponsor') || combined.includes('presented')) return 'sponsor';
    if (combined.includes('festival') || combined.includes('parade') || combined.includes('classic') || combined.includes('event')) return 'event';
    if (programLikeCount >= Math.max(2, adLikeCount)) return 'program_series';
    return adLikeCount > 0 ? 'client' : 'event';
}

export function buildImportInsights(files: ImportCandidateFile[]): ImportInsights {
    const usableFiles = files.filter(file => file.filename && file.type !== 'gap');
    const phraseMap = new Map<string, ImportCandidateFile[]>();

    for (const file of usableFiles) {
        const seenForFile = new Set<string>();
        for (const phrase of phraseCandidates(tokenize(file.filename))) {
            if (seenForFile.has(phrase)) continue;
            seenForFile.add(phrase);
            phraseMap.set(phrase, [...(phraseMap.get(phrase) || []), file]);
        }
    }

    const suggestions = Array.from(phraseMap.entries())
        .filter(([phrase, matches]) => matches.length >= 5 && phrase.length >= 3)
        .sort((a, b) => {
            const countDelta = b[1].length - a[1].length;
            if (countDelta !== 0) return countDelta;
            return b[0].length - a[0].length;
        })
        .slice(0, 6)
        .map(([phrase, matches]) => {
            const label = titleCase(phrase);
            const kind = inferKind(phrase, matches);
            const confidence = Math.min(0.96, 0.52 + matches.length * 0.07 + (phrase.includes(' ') ? 0.08 : 0));
            return {
                id: `cluster_${phrase.replace(/\s+/g, '_')}`,
                label,
                kind,
                confidence,
                files: matches,
                reason: `${matches.length} imported files share "${label}" in the title.`,
                question: `Should "${label}" be tracked as a ${kind.replace('_', ' ')} for these files?`,
            };
        });

    const notes = suggestions.length > 0
        ? ['Review the suggested client/event groups before import so spots and promos are tracked cleanly.']
        : ['No shared title pattern crossed the 5-file threshold yet.'];

    return { suggestions, notes };
}

export function isMediaAvailableNow(media: MediaItem, now = Date.now()): boolean {
    if (media.paused || media.archived) return false;
    if (media.availableStart && new Date(media.availableStart).getTime() > now) return false;
    if (media.availableEnd && new Date(media.availableEnd).getTime() < now) return false;
    return true;
}

export function rankMediaForSmartFill(
    candidates: MediaItem[],
    statsByMediaId: Record<string, MediaStats>,
    options: { preferredSeries?: string; mediaType?: MediaType; now?: number } = {},
): MediaItem[] {
    const now = options.now || Date.now();

    return candidates
        .filter(media => (!options.mediaType || media.type === options.mediaType) && isMediaAvailableNow(media, now))
        .map(media => {
            const stats = statsByMediaId[media.id];
            const playCount = stats?.playCount ?? media.playCount ?? 0;
            const lastPlayed = stats?.lastPlayed || (media.lastPlayedAt ? new Date(media.lastPlayedAt).getTime() : 0);
            const hoursSincePlayed = lastPlayed ? Math.max(0, (now - lastPlayed) / 36e5) : 999;
            const priority = media.priority || 0;
            const seriesBoost = options.preferredSeries && media.type === 'bumper'
                ? ((media as any).bumperConfig?.programSeries === options.preferredSeries ? 40 : ((media as any).bumperConfig?.isGeneric ? 15 : 0))
                : 0;
            const freshnessScore = Math.min(80, hoursSincePlayed * 4);
            const score = freshnessScore + priority * 4 + seriesBoost - playCount * 8 + Math.random();
            return { media, score };
        })
        .sort((a, b) => b.score - a.score)
        .map(entry => entry.media);
}

export interface DynamicFillerIdea {
    id: string;
    title: string;
    type: 'weather_tts' | 'coming_up' | 'psa' | 'station_promo';
    duration: number;
    source: 'open_meteo' | 'schedule' | 'getstarnews' | 'local';
    loopable: boolean;
    notes: string;
}

export function buildDynamicFillerIdeas(context: { city?: string; upcomingTitles?: string[] } = {}): DynamicFillerIdea[] {
    const city = context.city || 'local area';
    const upcoming = (context.upcomingTitles || []).slice(0, 3).join(', ');

    return [
        {
            id: 'filler_weather_tts',
            title: `Weather update for ${city}`,
            type: 'weather_tts',
            duration: 45,
            source: 'open_meteo',
            loopable: false,
            notes: 'Generate short natural TTS over dynamic vector weather graphics when schedule has a gap.',
        },
        {
            id: 'filler_coming_up',
            title: upcoming ? `Coming up: ${upcoming}` : 'Coming up soon',
            type: 'coming_up',
            duration: 30,
            source: 'schedule',
            loopable: false,
            notes: 'Use current playlist data to build a short coming-up promo.',
        },
        {
            id: 'filler_public_service',
            title: 'Public service update',
            type: 'psa',
            duration: 60,
            source: 'getstarnews',
            loopable: false,
            notes: 'Pull approved local PSA/community copy into a lightweight dynamic graphic.',
        },
        {
            id: 'filler_station_promo',
            title: 'Station promo loop',
            type: 'station_promo',
            duration: 30,
            source: 'local',
            loopable: true,
            notes: 'Safe repeating filler when no time-sensitive content is available.',
        },
    ];
}

export interface AiGuidedGeneratorTemplate {
    id: string;
    title: string;
    category: 'tipline' | 'traffic' | 'sports' | 'school' | 'weather' | 'history' | 'facts' | 'psa';
    durationSeconds: number;
    preferredCadence: 'hourly' | 'daily' | 'as-needed' | 'weekend';
    regionTags: string[];
    prompt: string;
    exampleCopy: string;
}

export const AI_GUIDED_GENERATOR_TEMPLATES: AiGuidedGeneratorTemplate[] = [
    {
        id: 'got_scoop_paid_tipline',
        title: 'Got Scoop? Paid Tipline',
        category: 'tipline',
        durationSeconds: 20,
        preferredCadence: 'hourly',
        regionTags: ['martinsville', 'danville', 'rockingham', 'local'],
        prompt: 'Generate a short discreet local-news tipline announcement. Keep the phone number and [NEWS] keyword exact. Mention cashapp/chime tag only as an optional way for the tipster to get paid for strong verified scoops.',
        exampleCopy: 'Got scoop? Text 276-555-0100 with [NEWS]. Drop your $cashtag or Chime tag if you want. Good verified scoops might get paid discreetly.',
    },
    {
        id: 'vdot_ncdot_alert',
        title: 'VDOT / NCDOT Road Alert',
        category: 'traffic',
        durationSeconds: 30,
        preferredCadence: 'as-needed',
        regionTags: ['va', 'nc', 'vdot', 'ncdot', 'traffic'],
        prompt: 'Summarize a current road closure, crash, detour, construction window, or lane closure for a TV/radio filler. Lead with location, impact, timing, and safe alternate-route language.',
        exampleCopy: 'Traffic alert: crews are working near the corridor this hour. Give yourself extra time, watch for posted detours, and slow down in work zones.',
    },
    {
        id: 'school_sports_upcoming',
        title: 'Upcoming School Sports',
        category: 'sports',
        durationSeconds: 30,
        preferredCadence: 'daily',
        regionTags: ['sports', 'school', 'martinsville', 'danville', 'rockingham'],
        prompt: 'Turn upcoming school sports events into a concise on-air rundown. Include school, sport, opponent, date/time when known, and a neutral call for score submissions.',
        exampleCopy: 'Coming up in local sports: keep an eye on tonight’s school matchups, and send verified finals to Star News after the game.',
    },
    {
        id: 'closings_delays',
        title: 'Closings and Delays',
        category: 'school',
        durationSeconds: 25,
        preferredCadence: 'as-needed',
        regionTags: ['school', 'closing', 'delay', 'weather'],
        prompt: 'Generate a clear closings/delays update. If no verified closings exist, say no major closings have been posted and tell viewers to check official school channels.',
        exampleCopy: 'Closings and delays update: no major verified school closings are posted in our desk right now. Check your district before heading out.',
    },
    {
        id: 'local_history_day',
        title: 'On This Day in Local History',
        category: 'history',
        durationSeconds: 45,
        preferredCadence: 'daily',
        regionTags: ['history', 'local', 'martinsville', 'danville', 'rockingham'],
        prompt: 'Create an “On this day in local history” filler. Keep it factual, dated when possible, and suitable for a family audience. If unsure, frame as a general local-history note.',
        exampleCopy: 'On this day in local history, we remember the mills, tracks, schools, and downtown corners that shaped our region.',
    },
    {
        id: 'random_local_fun_fact',
        title: 'Random Local Fun Fact',
        category: 'facts',
        durationSeconds: 20,
        preferredCadence: 'daily',
        regionTags: ['local', 'facts', 'community'],
        prompt: 'Generate a short random fun fact tied to the local region, weather, sports, geography, or community life. Avoid claims that need verification unless provided by source text.',
        exampleCopy: 'Local fun fact: this region sits at a crossroads of Piedmont roads, river valleys, old rail lines, and high-school rivalries.',
    },
];

/** Preload bridge; cast because `Window` merge from `vite-env.d.ts` is not always visible to `tsc` in every graph. */
export function getElectronApi(): ElectronAPI | undefined {
    if (typeof window === 'undefined') return undefined;
    return (window as unknown as { electronAPI?: ElectronAPI }).electronAPI;
}

export async function requestOpenRouterJson<T = unknown>(request: OpenRouterJsonRequest): Promise<T | null> {
    const api = getElectronApi();
    if (!api?.ai?.openRouterJson) return null;
    const stored = getStoredOpenRouterCredentials();
    const apiKey = (request.apiKey && request.apiKey.trim()) || stored.apiKey;
    const modelFromRequest = typeof request.model === 'string' ? request.model.trim() : '';
    const modelFromStore = stored.model.trim();
    const model = modelFromRequest || modelFromStore || undefined;
    const result = await api.ai.openRouterJson({
        ...request,
        apiKey: apiKey || undefined,
        model,
    });
    if (!result?.success) return null;
    return (result.parsed ?? result.content) as T;
}

export async function buildAiReportBrief(range: { startDate: string; endDate: string; clientId?: string }): Promise<string> {
    const {
        buildDetailedReportBundle,
        summarizeDetailedReportWithAI,
    } = await import('./detailedReportBuilder');
    const bundle = await buildDetailedReportBundle(range);
    return summarizeDetailedReportWithAI(bundle);
}
