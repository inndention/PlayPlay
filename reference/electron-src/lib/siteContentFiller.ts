import { PlaylistItem, MediaItem } from './types';
import { persistentLogger } from './persistentLogger';
import { logError, logInfo, logWarning } from './errorBus';
import { AI_GUIDED_GENERATOR_TEMPLATES, AiGuidedGeneratorTemplate, requestOpenRouterJson } from './aiHelper';

export type FillerType = 'traffic' | 'closing' | 'sports' | 'obituary' | 'history' | 'headline' | 'tipline' | 'facts' | 'psa' | 'weather';

export interface FillerSlate {
    id: string;
    title: string;
    content: string;
    type: FillerType;
    createdAt: number;
    expiresAt: number;
    media?: string[];
    regionTags?: string[];
    sourceUrl?: string;
    templateId?: string;
    aiGenerated?: boolean;
}

const STORAGE_KEY = 'site_filler_slates';
const API_URL_KEY = 'starnews_api_url';
const API_KEY_KEY = 'starnews_api_key';

const TTL_MS: Record<FillerType, number> = {
    traffic: 2 * 60 * 60 * 1000,      // 2 hours
    closing: 12 * 60 * 60 * 1000,     // 12 hours
    sports: 24 * 60 * 60 * 1000,      // 24 hours
    obituary: 72 * 60 * 60 * 1000,    // 72 hours
    history: 48 * 60 * 60 * 1000,     // 48 hours
    headline: 12 * 60 * 60 * 1000,    // 12 hours
    tipline: 7 * 24 * 60 * 60 * 1000,
    facts: 48 * 60 * 60 * 1000,
    psa: 48 * 60 * 60 * 1000,
    weather: 2 * 60 * 60 * 1000,
};

let missingApiKeyLogged = false;

function loadConfig() {
    const url = localStorage.getItem(API_URL_KEY) || 'https://getstarnews.com/router.php?endpoint=posts';
    const key = localStorage.getItem(API_KEY_KEY) || '';
    return { url, key };
}

function classifyType(title: string): FillerType {
    const t = title.toLowerCase();
    if (t.includes('traffic') || t.includes('road') || t.includes('crash')) return 'traffic';
    if (t.includes('closing') || t.includes('delay') || t.includes('school')) return 'closing';
    if (t.includes('score') || t.includes('vs') || t.includes('final') || t.includes('sports')) return 'sports';
    if (t.includes('obit') || t.includes('obituary') || t.includes('remembering')) return 'obituary';
    if (t.includes('history') || t.includes('this day')) return 'history';
    if (t.includes('scoop') || t.includes('tipline')) return 'tipline';
    if (t.includes('weather')) return 'weather';
    if (t.includes('fact')) return 'facts';
    if (t.includes('psa') || t.includes('public service')) return 'psa';
    return 'headline';
}

function saveSlates(slates: FillerSlate[]) {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(slates));
    } catch {
        /* ignore */
    }
}

export function getAiGuidedGeneratorTemplates(): AiGuidedGeneratorTemplate[] {
    return AI_GUIDED_GENERATOR_TEMPLATES;
}

export async function generateAiFillerSlate(templateId: string, sourceText = '', regionTags: string[] = []): Promise<FillerSlate> {
    const template = AI_GUIDED_GENERATOR_TEMPLATES.find(item => item.id === templateId) || AI_GUIDED_GENERATOR_TEMPLATES[0];
    const now = Date.now();
    const baseType = template.category === 'school' ? 'closing' : template.category === 'facts' ? 'facts' : template.category;
    const type = baseType === 'tipline' || baseType === 'traffic' || baseType === 'sports' || baseType === 'weather' || baseType === 'history' || baseType === 'facts' || baseType === 'psa'
        ? baseType
        : 'headline';

    const ai = await requestOpenRouterJson<{ title?: string; content?: string }>({
        system: 'You generate concise broadcast-safe local TV/radio filler copy. Return JSON with title and content.',
        prompt: `${template.prompt}\n\nRequired example style:\n${template.exampleCopy}\n\nSource text:\n${sourceText || 'No source text provided. Use the template safely without inventing specific unverifiable facts.'}`,
        temperature: 0.35,
        maxTokens: 500,
    });

    const slate: FillerSlate = {
        id: `ai_slate_${template.id}_${now}`,
        title: ai && typeof ai !== 'string' && ai.title ? ai.title : template.title,
        content: ai && typeof ai !== 'string' && ai.content ? ai.content : template.exampleCopy,
        type,
        createdAt: now,
        expiresAt: now + (TTL_MS[type] || TTL_MS.headline),
        regionTags: regionTags.length ? regionTags : template.regionTags,
        templateId: template.id,
        aiGenerated: !!ai,
    };

    const slates = [slate, ...getFillerSlates()].slice(0, 200);
    saveSlates(slates);
    window.dispatchEvent(new CustomEvent('site-filler-updated', { detail: slates }));
    return slate;
}

export function seedDefaultAiFillerSlates(): FillerSlate[] {
    const now = Date.now();
    const existing = getFillerSlates();
    const existingTemplateIds = new Set(existing.map(slate => slate.templateId));
    const seeded = AI_GUIDED_GENERATOR_TEMPLATES
        .filter(template => !existingTemplateIds.has(template.id))
        .map<FillerSlate>(template => {
            const type = template.category === 'tipline'
                ? 'tipline'
                : template.category === 'facts'
                    ? 'facts'
                    : template.category === 'school'
                        ? 'closing'
                        : template.category;
            return {
                id: `seed_${template.id}`,
                title: template.title,
                content: template.exampleCopy,
                type: (type in TTL_MS ? type : 'headline') as FillerType,
                createdAt: now,
                expiresAt: now + (TTL_MS[(type in TTL_MS ? type : 'headline') as FillerType] || TTL_MS.headline),
                regionTags: template.regionTags,
                templateId: template.id,
                aiGenerated: false,
            };
        });
    const next = [...seeded, ...existing].slice(0, 200);
    saveSlates(next);
    return next;
}

function loadSlates(): FillerSlate[] {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return [];
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) return parsed as FillerSlate[];
    } catch {
        /* ignore */
    }
    return [];
}

export function getFillerSlates(): FillerSlate[] {
    const now = Date.now();
    const filtered = loadSlates().filter(s => s.expiresAt > now);
    if (filtered.length !== loadSlates().length) {
        saveSlates(filtered);
    }
    return filtered;
}

export async function refreshSiteFiller(): Promise<FillerSlate[]> {
    const { url, key } = loadConfig();
    if (!key) {
        if (!missingApiKeyLogged) {
            logInfo('Star News API key not set; site filler using cached content', {});
            missingApiKeyLogged = true;
        }
        return getFillerSlates();
    }
    missingApiKeyLogged = false;

    try {
        const res = await fetch(url, {
            headers: { Authorization: `Bearer ${key}` },
        });
        if (!res.ok) {
            const text = await res.text();
            throw new Error(`HTTP ${res.status}: ${text}`);
        }
        const data = await res.json();
        const posts = Array.isArray(data) ? data : data.posts || [];
        const now = Date.now();
        const slates: FillerSlate[] = posts.map((p: any) => {
            const title = p.title || p.name || 'Update';
            const content = p.content || p.body || p.description || '';
            const type = classifyType(title);
            const ttl = TTL_MS[type] || TTL_MS.headline;
            return {
                id: `slate_${p.id || Math.random().toString(36).slice(2, 8)}`,
                title,
                content,
                type,
                createdAt: now,
                expiresAt: now + ttl,
                media: p.media || [],
            };
        });

        saveSlates(slates);
        logInfo('Site filler refreshed', { count: slates.length });
        if (typeof window !== 'undefined') {
            window.dispatchEvent(new CustomEvent('site-filler-updated', { detail: slates }));
        }
        return slates;
    } catch (error) {
        logError('Failed to refresh site filler', { error: String(error) });
        return getFillerSlates();
    }
}

export function buildFillerPlaylist(durationSeconds = 10): PlaylistItem[] {
    const slates = getFillerSlates();
    const now = Date.now();
    return slates
        .filter(s => s.expiresAt > now)
        .map<PlaylistItem>(s => {
            const media: MediaItem = {
                id: s.id,
                title: `[${s.type.toUpperCase()}] ${s.title}`,
                type: 'image',
                duration: durationSeconds,
                src: 'signal:bars',
                description: s.content,
                mediaClass: 'filler',
                regionTags: s.regionTags,
            } as MediaItem;
            return {
                id: `${s.id}_item`,
                mediaId: media.id,
                media,
                isResolved: true,
                notes: s.content,
            };
        });
}
