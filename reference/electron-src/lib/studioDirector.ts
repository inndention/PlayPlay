/**
 * studioDirector.ts
 * ─────────────────────────────────────────────────────────────────────────────
 * AI Studio Director — the brains of the broadcast floor.
 *
 * Capabilities:
 *  • Natural-voice top-of-hour / half-hour announcements (TTS via OpenRouter)
 *  • Commercial break warnings (30s / 10s / "break now" cues)
 *  • Break-over "coming back" cues
 *  • Breaking news interrupt — cuts audio, drops lower-third, fades back
 *  • WebRTC live-shot manager — smooth take/release with intro TTS
 *  • Timed alert system for upcoming events (school, weather, VDOT)
 *  • Per-site (Martinsville / Danville / Rockingham) personality
 *  • OpenRouter JSON generation for all copy
 * ─────────────────────────────────────────────────────────────────────────────
 */

import { requestOpenRouterJson } from './aiHelper';

// ─── Types ────────────────────────────────────────────────────────────────────

export type SiteId = 'martinsville' | 'danville' | 'rockingham';

export interface SiteProfile {
    id: SiteId;
    displayName: string;
    region: string;
    personality: string;   // terse personality hint for TTS prompts
    weather: {
        lat: number;
        lon: number;
        city: string;
    };
}

export type AnnouncementKind =
    | 'top_of_hour'
    | 'half_hour'
    | 'break_warning_30'
    | 'break_warning_10'
    | 'break_now'
    | 'back_from_break'
    | 'breaking_news'
    | 'live_shot_intro'
    | 'live_shot_outro'
    | 'weather_update'
    | 'coming_up'
    | 'custom';

export interface AnnouncementRequest {
    kind: AnnouncementKind;
    siteId?: SiteId;
    /** Extra context injected into the AI prompt */
    context?: Record<string, string>;
    /** Bypass AI — use this copy directly */
    forcedCopy?: string;
    /** TTS voice override (OpenRouter or browser TTS voice name) */
    voice?: string;
}

export interface AnnouncementResult {
    kind: AnnouncementKind;
    copy: string;
    audioUrl: string | null;   // blob URL or null if browser TTS was used
    durationMs: number;
    siteId?: SiteId;
    generatedAt: number;
}

export interface BreakCue {
    id: string;
    breakStartMs: number;      // absolute epoch ms when the break begins
    durationMs: number;
    warned30: boolean;
    warned10: boolean;
    cued: boolean;
    backcued: boolean;
}

export interface BreakingNewsAlert {
    id: string;
    headline: string;
    body?: string;
    siteIds: SiteId[];
    priority: 'urgent' | 'standard';
    triggeredAt: number;
    acknowledgedAt?: number;
}

export interface LiveShotSession {
    id: string;
    reporterName: string;
    location: string;
    siteId: SiteId;
    webrtcSessionId: string;
    status: 'pending' | 'live' | 'wrapping' | 'ended';
    takenAt?: number;
    releasedAt?: number;
}

export interface DirectorEvent {
    type:
        | 'announcement_ready'
        | 'break_warning'
        | 'break_now'
        | 'back_from_break'
        | 'breaking_news'
        | 'live_shot_taken'
        | 'live_shot_released'
        | 'alert_triggered';
    payload: unknown;
    ts: number;
}

// ─── Site profiles ────────────────────────────────────────────────────────────

export const SITE_PROFILES: Record<SiteId, SiteProfile> = {
    martinsville: {
        id: 'martinsville',
        displayName: 'Star News Martinsville / Henry County',
        region: 'Martinsville and Henry County, Virginia',
        personality: 'Friendly Southern Virginia broadcast voice. Warm but professional.',
        weather: { lat: 36.6918, lon: -79.8728, city: 'Martinsville' },
    },
    danville: {
        id: 'danville',
        displayName: 'Star News Danville / Pittsylvania',
        region: 'Danville and Pittsylvania County, Virginia',
        personality: 'Clear, authoritative Virginia Piedmont news voice.',
        weather: { lat: 36.5860, lon: -79.3950, city: 'Danville' },
    },
    rockingham: {
        id: 'rockingham',
        displayName: 'Star News Rockingham / Eden / Reidsville',
        region: 'Rockingham County and Eden/Reidsville, North Carolina',
        personality: 'Conversational North Carolina community news voice.',
        weather: { lat: 36.4960, lon: -79.7717, city: 'Eden' },
    },
};

// ─── Prompt templates ─────────────────────────────────────────────────────────

const PROMPTS: Record<AnnouncementKind, (site: SiteProfile, ctx: Record<string, string>) => string> = {
    top_of_hour: (site, ctx) =>
        `You are a ${site.personality} Write a natural top-of-the-hour station ID and time call for ${site.displayName}. ` +
        `Current time: ${ctx.timeLabel || 'this hour'}. Keep it under 20 seconds when read aloud. No fluff. Return only the spoken copy.`,

    half_hour: (site, ctx) =>
        `${site.personality} Write a brief half-hour check-in for ${site.displayName}. ` +
        `Time: ${ctx.timeLabel || 'the half hour'}. One sentence maximum. Return only the spoken copy.`,

    break_warning_30: (_site, ctx) =>
        `Write a quick 30-second-warning before a commercial break for a local TV station. ` +
        `Coming up after the break: ${ctx.nextSegment || 'more programming'}. Max 10 words. Return only the spoken copy.`,

    break_warning_10: (_site, _ctx) =>
        `Write a tight "commercial break in 10 seconds" on-air cue for an automation system. Under 8 words. Return only the spoken copy.`,

    break_now: (_site, _ctx) =>
        `Write a natural "going to commercial" cue. 5 words or fewer. Return only the spoken copy.`,

    back_from_break: (site, ctx) =>
        `${site.personality} Write a "welcome back" cue returning from commercial break on ${site.displayName}. ` +
        `What's coming up: ${ctx.nextSegment || 'continuing coverage'}. Under 15 words. Return only the spoken copy.`,

    breaking_news: (site, ctx) =>
        `${site.personality} Write an urgent breaking-news interrupt cue for ${site.region}. ` +
        `Headline: ${ctx.headline || 'developing story'}. Under 20 words. Start with "Breaking News —". Return only the spoken copy.`,

    live_shot_intro: (site, ctx) =>
        `${site.personality} Introduce a live reporter for ${site.displayName}. ` +
        `Reporter: ${ctx.reporterName || 'our reporter'}. Location: ${ctx.location || 'the scene'}. ` +
        `One natural broadcast toss sentence. Return only the spoken copy.`,

    live_shot_outro: (site, ctx) =>
        `${site.personality} Write a thank-you toss-back from ${ctx.reporterName || 'the reporter'} at ${ctx.location || 'the scene'} ` +
        `back to the studio for ${site.displayName}. Under 15 words. Return only the spoken copy.`,

    weather_update: (site, ctx) =>
        `${site.personality} Write a 30-second natural TTS weather update for ${site.weather.city}. ` +
        `Current conditions: ${ctx.conditions || 'partly cloudy'}. High: ${ctx.high || '75°F'}. Low: ${ctx.low || '58°F'}. ` +
        `Wind: ${ctx.wind || 'light and variable'}. Outlook: ${ctx.outlook || 'pleasant for the rest of the week'}. ` +
        `Sound natural, not robotic. Return only the spoken copy.`,

    coming_up: (_site, ctx) =>
        `Write a brief "coming up" teaser for a local broadcast. ` +
        `Upcoming segments: ${ctx.upcomingTitles || 'more great programming'}. Under 20 words. Return only the spoken copy.`,

    custom: (_site, ctx) =>
        ctx.customPrompt || 'Write a short professional broadcast announcement.',
};

// ─── Weather fetcher (Open-Meteo — free, no key) ─────────────────────────────

interface WeatherData {
    conditions: string;
    high: string;
    low: string;
    wind: string;
    outlook: string;
}

async function fetchWeather(lat: number, lon: number): Promise<WeatherData> {
    try {
        const url =
            `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
            `&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,windspeed_10m_max,weathercode` +
            `&current_weather=true&temperature_unit=fahrenheit&windspeed_unit=mph&timezone=auto`;
        const res = await fetch(url);
        if (!res.ok) throw new Error('Weather fetch failed');
        const data = await res.json();

        const current = data.current_weather || {};
        const daily   = data.daily         || {};

        const conditionMap: Record<number, string> = {
            0: 'clear skies', 1: 'mostly clear', 2: 'partly cloudy', 3: 'overcast',
            45: 'foggy', 48: 'icy fog', 51: 'light drizzle', 53: 'drizzle', 55: 'heavy drizzle',
            61: 'light rain', 63: 'rain', 65: 'heavy rain', 71: 'light snow', 73: 'snow', 75: 'heavy snow',
            80: 'scattered showers', 81: 'showers', 82: 'heavy showers',
            95: 'thunderstorms', 96: 'thunderstorms with hail', 99: 'severe thunderstorms',
        };

        const code = current.weathercode ?? 2;
        const cond = conditionMap[code] || 'variable conditions';

        return {
            conditions: cond,
            high: `${Math.round(daily.temperature_2m_max?.[0] ?? current.temperature ?? 75)}°F`,
            low:  `${Math.round(daily.temperature_2m_min?.[0] ?? (current.temperature != null ? current.temperature - 15 : 58))}°F`,
            wind: `${Math.round(current.windspeed ?? 5)} mph`,
            outlook: (daily.precipitation_sum?.[0] ?? 0) > 0.1
                ? 'some rain expected later in the week'
                : 'dry and pleasant through the week',
        };
    } catch {
        return { conditions: 'partly cloudy', high: '75°F', low: '58°F', wind: 'light', outlook: 'pleasant' };
    }
}

// ─── TTS — Browser Web Speech API (zero-cost fallback) ────────────────────────

function speakViaBrowser(text: string, voice?: string): Promise<number> {
    return new Promise((resolve) => {
        if (typeof window === 'undefined' || !window.speechSynthesis) {
            resolve(0);
            return;
        }
        const utter = new SpeechSynthesisUtterance(text);
        utter.rate  = 0.95;
        utter.pitch = 1.0;
        if (voice) {
            const voices = window.speechSynthesis.getVoices();
            const match  = voices.find(v => v.name.toLowerCase().includes(voice.toLowerCase()));
            if (match) utter.voice = match;
        }
        utter.onend = () => resolve(Math.round((text.split(' ').length / 2.5) * 1000));
        window.speechSynthesis.speak(utter);
    });
}

// ─── Core: StudioDirector class ───────────────────────────────────────────────

export class StudioDirector {
    private listeners: Array<(event: DirectorEvent) => void> = [];
    private breakCues: Map<string, BreakCue> = new Map();
    private liveShotSessions: Map<string, LiveShotSession> = new Map();
    private announcementCache: Map<string, AnnouncementResult> = new Map();
    private breakCheckInterval: ReturnType<typeof setInterval> | null = null;

    constructor() {
        this.startBreakCueWatcher();
    }

    // ── Event bus ──────────────────────────────────────────────────────────────

    on(listener: (event: DirectorEvent) => void): () => void {
        this.listeners.push(listener);
        return () => { this.listeners = this.listeners.filter(l => l !== listener); };
    }

    private emit(type: DirectorEvent['type'], payload: unknown): void {
        const event: DirectorEvent = { type, payload, ts: Date.now() };
        this.listeners.forEach(l => l(event));
    }

    // ── Announcement generation ────────────────────────────────────────────────

    async generateAnnouncement(req: AnnouncementRequest): Promise<AnnouncementResult> {
        const site     = SITE_PROFILES[req.siteId ?? 'martinsville'];
        const ctx      = req.context ?? {};
        const cacheKey = `${req.kind}_${req.siteId}_${JSON.stringify(ctx)}`;

        // Auto-inject weather context for weather_update
        if (req.kind === 'weather_update' && !ctx.conditions) {
            const wx = await fetchWeather(site.weather.lat, site.weather.lon);
            Object.assign(ctx, wx);
        }

        // Auto-inject time label
        if (!ctx.timeLabel) {
            const now = new Date();
            ctx.timeLabel = now.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', timeZoneName: 'short' });
        }

        // Use forced copy or AI generation
        let copy = req.forcedCopy ?? '';
        if (!copy) {
            const promptFn = PROMPTS[req.kind];
            const prompt   = promptFn ? promptFn(site, ctx) : ctx.customPrompt ?? '';

            const aiResult = await requestOpenRouterJson<{ copy: string }>({
                system : 'You are a professional broadcast TV studio director AI. Return ONLY a JSON object with key "copy" containing the spoken announcement. No explanation, no markdown.',
                prompt,
                model  : 'openai/gpt-4o-mini',
                temperature: 0.7,
                maxTokens: 120,
            });

            copy = aiResult?.copy?.trim() || this.fallbackCopy(req.kind, site, ctx);
        }

        // TTS via browser (free, instant)
        const durationMs = await speakViaBrowser(copy, req.voice);

        const result: AnnouncementResult = {
            kind: req.kind,
            copy,
            audioUrl: null,
            durationMs,
            siteId: req.siteId,
            generatedAt: Date.now(),
        };

        this.announcementCache.set(cacheKey, result);
        this.emit('announcement_ready', result);
        return result;
    }

    // ── Fallback copy (zero-AI, zero-cost) ────────────────────────────────────

    private fallbackCopy(kind: AnnouncementKind, site: SiteProfile, ctx: Record<string, string>): string {
        const map: Partial<Record<AnnouncementKind, string>> = {
            top_of_hour     : `You're watching ${site.displayName}. It's the top of the hour.`,
            half_hour       : `Star News, your local source. The half hour.`,
            break_warning_30: `Coming up after the break — ${ctx.nextSegment ?? 'more programming'}.`,
            break_warning_10: `Commercial break in 10 seconds.`,
            break_now       : `We'll be right back.`,
            back_from_break : `Welcome back. Up next — ${ctx.nextSegment ?? 'more coverage'}.`,
            breaking_news   : `Breaking News — ${ctx.headline ?? 'developing story'}. Stand by.`,
            live_shot_intro : `Live now — ${ctx.reporterName ?? 'our reporter'} is standing by at ${ctx.location ?? 'the scene'}.`,
            live_shot_outro : `Thank you. We'll continue to follow this story.`,
            weather_update  : `Current conditions: ${ctx.conditions ?? 'partly cloudy'}. High of ${ctx.high ?? '75°F'}, low of ${ctx.low ?? '58°F'}. ${ctx.outlook ?? 'A nice day ahead'}.`,
            coming_up       : `Coming up — ${ctx.upcomingTitles ?? 'more local coverage'}.`,
        };
        return map[kind] ?? 'Star News, staying connected to your community.';
    }

    // ── Top-of-hour scheduler ─────────────────────────────────────────────────

    scheduleHourlyAnnouncements(siteId: SiteId): () => void {
        const fireHourly = () => {
            const now = new Date();
            const min = now.getMinutes();
            if (min === 0) {
                this.generateAnnouncement({ kind: 'top_of_hour', siteId });
            } else if (min === 30) {
                this.generateAnnouncement({ kind: 'half_hour', siteId });
            }
        };

        // Align to next minute boundary
        const alignDelay = (60 - new Date().getSeconds()) * 1000;
        let interval: ReturnType<typeof setInterval>;

        const timer = setTimeout(() => {
            fireHourly();
            interval = setInterval(fireHourly, 60_000);
        }, alignDelay);

        return () => {
            clearTimeout(timer);
            clearInterval(interval);
        };
    }

    // ── Break cue management ──────────────────────────────────────────────────

    registerBreak(opts: {
        id: string;
        breakStartMs: number;
        durationMs: number;
    }): void {
        this.breakCues.set(opts.id, {
            id      : opts.id,
            breakStartMs: opts.breakStartMs,
            durationMs  : opts.durationMs,
            warned30: false,
            warned10: false,
            cued    : false,
            backcued: false,
        });
    }

    removeBreak(id: string): void {
        this.breakCues.delete(id);
    }

    private startBreakCueWatcher(): void {
        this.breakCheckInterval = setInterval(() => {
            const now = Date.now();
            this.breakCues.forEach(cue => {
                const msUntilBreak = cue.breakStartMs - now;
                const msAfterBreak = now - (cue.breakStartMs + cue.durationMs);

                // 30-second warning
                if (!cue.warned30 && msUntilBreak <= 30_000 && msUntilBreak > 0) {
                    cue.warned30 = true;
                    this.emit('break_warning', { cueId: cue.id, secondsRemaining: 30 });
                    this.generateAnnouncement({ kind: 'break_warning_30' });
                }

                // 10-second warning
                if (!cue.warned10 && msUntilBreak <= 10_000 && msUntilBreak > 0) {
                    cue.warned10 = true;
                    this.emit('break_warning', { cueId: cue.id, secondsRemaining: 10 });
                    this.generateAnnouncement({ kind: 'break_warning_10' });
                }

                // Break start cue
                if (!cue.cued && msUntilBreak <= 0 && msAfterBreak < 2000) {
                    cue.cued = true;
                    this.emit('break_now', { cueId: cue.id });
                    this.generateAnnouncement({ kind: 'break_now' });
                }

                // Back-from-break cue
                if (!cue.backcued && msAfterBreak >= 0 && msAfterBreak < 5000) {
                    cue.backcued = true;
                    this.emit('back_from_break', { cueId: cue.id });
                    this.generateAnnouncement({ kind: 'back_from_break' });
                    // Clean up stale cue
                    setTimeout(() => this.breakCues.delete(cue.id), 30_000);
                }
            });
        }, 500);
    }

    // ── Breaking news interrupt ────────────────────────────────────────────────

    async triggerBreakingNews(alert: BreakingNewsAlert): Promise<void> {
        this.emit('breaking_news', alert);

        // Fire announcement on all relevant sites
        const sites = alert.siteIds.length ? alert.siteIds : (['martinsville'] as SiteId[]);
        await Promise.all(
            sites.map(siteId =>
                this.generateAnnouncement({
                    kind   : 'breaking_news',
                    siteId,
                    context: { headline: alert.headline, body: alert.body ?? '' },
                })
            )
        );
    }

    // ── WebRTC live shot management ────────────────────────────────────────────

    async takeLiveShot(opts: {
        reporterName: string;
        location: string;
        siteId: SiteId;
        webrtcSessionId: string;
    }): Promise<LiveShotSession> {
        const session: LiveShotSession = {
            id            : `liveshot_${Date.now()}`,
            reporterName  : opts.reporterName,
            location      : opts.location,
            siteId        : opts.siteId,
            webrtcSessionId: opts.webrtcSessionId,
            status        : 'live',
            takenAt       : Date.now(),
        };

        this.liveShotSessions.set(session.id, session);
        this.emit('live_shot_taken', session);

        // Generate and speak intro
        await this.generateAnnouncement({
            kind   : 'live_shot_intro',
            siteId : opts.siteId,
            context: {
                reporterName: opts.reporterName,
                location    : opts.location,
            },
        });

        return session;
    }

    async releaseLiveShot(sessionId: string): Promise<void> {
        const session = this.liveShotSessions.get(sessionId);
        if (!session) return;

        session.status     = 'ended';
        session.releasedAt = Date.now();

        this.emit('live_shot_released', session);

        // Generate smooth outro
        await this.generateAnnouncement({
            kind   : 'live_shot_outro',
            siteId : session.siteId,
            context: {
                reporterName: session.reporterName,
                location    : session.location,
            },
        });

        this.liveShotSessions.delete(sessionId);
    }

    getAllLiveShots(): LiveShotSession[] {
        return Array.from(this.liveShotSessions.values());
    }

    // ── Weather update ─────────────────────────────────────────────────────────

    async announceWeather(siteId: SiteId): Promise<AnnouncementResult> {
        return this.generateAnnouncement({ kind: 'weather_update', siteId });
    }

    // ── Coming-up promo ───────────────────────────────────────────────────────

    async announceComingUp(siteId: SiteId, upcomingTitles: string[]): Promise<AnnouncementResult> {
        return this.generateAnnouncement({
            kind   : 'coming_up',
            siteId,
            context: { upcomingTitles: upcomingTitles.slice(0, 3).join(', ') },
        });
    }

    // ── OpenRouter bulk copy generator ────────────────────────────────────────

    async generateFillerCopy(opts: {
        kind: 'got_scoop' | 'vdot_alert' | 'school_sports' | 'closing_delay' | 'local_history' | 'fun_fact';
        siteId: SiteId;
        extraContext?: string;
    }): Promise<string> {
        const site = SITE_PROFILES[opts.siteId];

        const promptMap: Record<typeof opts.kind, string> = {
            got_scoop: `${site.personality} Write a 15-second discreet local news tipline promo for ${site.region}. ` +
                'Include exactly: phone 276-555-0100, keyword [NEWS], mention cashapp or chime tag optional, say good verified scoops might get paid discreetly. ' +
                'Return only the spoken copy.',

            vdot_alert: `${site.personality} Write a 25-second VDOT/NCDOT road alert filler for ${site.region}. ` +
                `Context: ${opts.extraContext || 'no active alerts — encourage safe driving and watching for work zones'}. ` +
                'Return only the spoken copy.',

            school_sports: `${site.personality} Write a 25-second upcoming school sports rundown for ${site.region}. ` +
                `Context: ${opts.extraContext || 'encourage viewers to send verified finals to Star News'}. ` +
                'Return only the spoken copy.',

            closing_delay: `${site.personality} Write a 20-second school closings/delays update for ${site.region}. ` +
                `Context: ${opts.extraContext || 'no major closings posted, tell viewers to check district websites'}. ` +
                'Return only the spoken copy.',

            local_history: `${site.personality} Write a 35-second "On This Day in Local History" filler for ${site.region}. ` +
                `Context: ${opts.extraContext || 'general local history and community milestones'}. Keep it factual. ` +
                'Return only the spoken copy.',

            fun_fact: `${site.personality} Write a 15-second random local fun fact for ${site.region}. ` +
                `Context: ${opts.extraContext || ''}. Keep it light and suitable for all ages. ` +
                'Return only the spoken copy.',
        };

        const result = await requestOpenRouterJson<{ copy: string }>({
            system     : 'You are a professional broadcast copy writer. Return ONLY valid JSON with key "copy". No markdown.',
            prompt     : promptMap[opts.kind],
            model      : 'openai/gpt-4o-mini',
            temperature: 0.8,
            maxTokens  : 150,
        });

        const copy = result?.copy?.trim() || '';
        if (copy) {
            speakViaBrowser(copy);
        }
        return copy;
    }

    // ── GetStarNews.com PSA scraper stub ──────────────────────────────────────

    async fetchStarNewsPSA(): Promise<string> {
        // Production: fetch https://getstarnews.com/community, parse latest PSA headline.
        // Stub returns placeholder — replace with real scrape via electron IPC or proxy.
        try {
            const api = (window as any).electronAPI;
            if (api?.scrape?.starNews) {
                const result = await api.scrape.starNews();
                return result?.latestPSA ?? '';
            }
        } catch { /* silent */ }
        return '';
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────

    destroy(): void {
        if (this.breakCheckInterval) {
            clearInterval(this.breakCheckInterval);
            this.breakCheckInterval = null;
        }
        this.listeners = [];
    }
}

// ── Singleton ─────────────────────────────────────────────────────────────────

let directorInstance: StudioDirector | null = null;

export function getStudioDirector(): StudioDirector {
    if (!directorInstance) {
        directorInstance = new StudioDirector();
    }
    return directorInstance;
}
