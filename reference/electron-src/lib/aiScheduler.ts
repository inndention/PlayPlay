import OpenAI from 'openai';
import { MediaItem, WeeklySchedule, HourSlot, PlaylistItem } from './types';
import { persistentLogger } from './persistentLogger';
import {
    fetchOpenRouterPublicBaseUrl,
    getStoredOpenRouterCredentials,
    OPENROUTER_LS_API_KEY,
    OPENROUTER_LS_API_KEY_LEGACY,
} from './aiHelper';

export interface SchedulerConstraints {
    targetDurationMinutes: number;
    commercialBreaksPerHour: number;
    commercialBreakDurationMinutes: number;
    avoidConsecutiveCategories: boolean;
    prioritizeHighPlayCount: boolean;
    requiredMediaIds?: string[];
}

export interface SchedulerResult {
    success: boolean;
    schedule?: PlaylistItem[];
    error?: string;
    rawResponse?: string;
}

export class AIScheduler {
    private client: OpenAI | null = null;

    constructor() {
        void this.ensureOpenAIClient();
    }

    public updateApiKey(key: string) {
        try {
            localStorage.setItem(OPENROUTER_LS_API_KEY, key);
            localStorage.setItem(OPENROUTER_LS_API_KEY_LEGACY, key);
        } catch {
            /* noop */
        }
        void this.ensureOpenAIClient();
    }

    /** Aligns with Electron main `OPENROUTER_BASE_URL` via `ai:openrouter-config` when available. */
    private async ensureOpenAIClient() {
        const { apiKey: key } = getStoredOpenRouterCredentials();
        if (!key) {
            this.client = null;
            return;
        }
        const base = await fetchOpenRouterPublicBaseUrl();
        this.client = new OpenAI({
            baseURL: `${base}/api/v1`,
            apiKey: key,
            dangerouslyAllowBrowser: true,
            defaultHeaders: {
                'HTTP-Referer': 'https://github.com/play-play-tv',
                'X-Title': 'Play-Play TV Automation',
            },
        });
    }

    public async generateSchedule(
        library: MediaItem[],
        constraints: SchedulerConstraints
    ): Promise<SchedulerResult> {
        await this.ensureOpenAIClient();
        if (!this.client) {
            return {
                success: false,
                error: 'OpenRouter API key not configured. Please add your key in Settings.'
            };
        }

        try {
            persistentLogger.info('Generating AI schedule', undefined, 'AIScheduler');

            const libraryPrompt = library.map(item => 
                `ID: ${item.id} | Title: ${item.title} | Duration: ${item.duration}s | Category: ${item.programSeries || 'N/A'}`
            ).join('\n');

            const systemPrompt = `You are a professional TV playout scheduling AI.
Your task is to generate a playlist of media items that strictly adheres to the requested constraints.
Available Media Library:
${libraryPrompt}

Constraints:
- Target total duration: ${constraints.targetDurationMinutes} minutes
- Commercial breaks per hour: ${constraints.commercialBreaksPerHour}
- Break duration: ${constraints.commercialBreakDurationMinutes} minutes
- Avoid consecutive categories: ${constraints.avoidConsecutiveCategories ? 'Yes' : 'No'}

Respond ONLY with a valid JSON array of selected Media IDs in the exact sequence they should be played. Example:
["id1", "id2", "id3"]`;

            const { model } = getStoredOpenRouterCredentials();
            const response = await this.client.chat.completions.create({
                model,
                messages: [
                    { role: 'system', content: systemPrompt },
                    { role: 'user', content: 'Generate the optimal playlist now.' }
                ],
                response_format: { type: 'json_object' }
            });

            const content = response.choices[0]?.message?.content;
            if (!content) throw new Error('No response from AI');

            let parsedIds: string[] = [];
            try {
                // Handle case where model returns { "playlist": [...] } or just [...]
                const parsed = JSON.parse(content);
                if (Array.isArray(parsed)) {
                    parsedIds = parsed;
                } else if (parsed.playlist && Array.isArray(parsed.playlist)) {
                    parsedIds = parsed.playlist;
                } else if (parsed.ids && Array.isArray(parsed.ids)) {
                    parsedIds = parsed.ids;
                } else {
                    throw new Error('Unrecognized JSON structure');
                }
            } catch (e) {
                return { success: false, error: 'Failed to parse AI response as JSON array of IDs.', rawResponse: content };
            }

            const schedule: PlaylistItem[] = parsedIds
                .map(id => library.find(item => item.id === id))
                .filter((item): item is MediaItem => item !== undefined)
                .map(media => ({
                    id: `ai_sched_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,
                    mediaId: media.id,
                    media: media,
                    isFixedTime: false,
                    isLive: false,
                    title: media.title,
                }));

            return {
                success: true,
                schedule
            };

        } catch (error: any) {
            persistentLogger.error('AI scheduling failed', error, 'AIScheduler');
            return {
                success: false,
                error: error.message || 'Unknown error occurred during generation.'
            };
        }
    }
}

export const aiScheduler = new AIScheduler();
