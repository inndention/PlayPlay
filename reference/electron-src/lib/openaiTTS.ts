// ============================================
// OpenAI TTS Integration
// Text-to-speech using OpenAI TTS API
// ============================================

import { loadSettings } from './storage';

export type TTSVoice = 'alloy' | 'echo' | 'fable' | 'onyx' | 'nova' | 'shimmer';
export type TTSFormat = 'mp3' | 'opus' | 'aac' | 'flac';

export interface TTSOptions {
    voice?: TTSVoice;
    speed?: number; // 0.25 to 4.0
    format?: TTSFormat;
    model?: 'tts-1' | 'tts-1-hd';
}

export interface TTSResult {
    audioBuffer: ArrayBuffer;
    audioUrl: string;
    duration?: number;
    format: TTSFormat;
}

class OpenAITTS {
    private apiKey: string | null = null;
    private cache: Map<string, TTSResult> = new Map();
    private cacheDir: string | null = null;

    constructor() {
        this.loadApiKey();
    }

    private loadApiKey(): void {
        try {
            const settings = loadSettings();
            this.apiKey = (settings as any).openaiApiKey || process.env.OPENAI_API_KEY || null;
        } catch (error) {
            console.error('[OpenAI TTS] Failed to load API key:', error);
        }
    }

    /**
     * Generate speech from text
     */
    public async generateSpeech(
        text: string,
        options: TTSOptions = {}
    ): Promise<TTSResult> {
        if (!this.apiKey) {
            throw new Error('OpenAI API key not configured. Set OPENAI_API_KEY or configure in settings.');
        }

        // Check cache
        const cacheKey = this.getCacheKey(text, options);
        if (this.cache.has(cacheKey)) {
            console.log('[OpenAI TTS] Using cached audio');
            return this.cache.get(cacheKey)!;
        }

        const {
            voice = 'nova',
            speed = 1.0,
            format = 'mp3',
            model = 'tts-1-hd',
        } = options;

        try {
            console.log(`[OpenAI TTS] Generating speech: "${text.substring(0, 50)}..."`);

            const response = await fetch('https://api.openai.com/v1/audio/speech', {
                method: 'POST',
                headers: {
                    'Authorization': `Bearer ${this.apiKey}`,
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    model,
                    input: text,
                    voice,
                    speed,
                    response_format: format,
                }),
            });

            if (!response.ok) {
                const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
                throw new Error(`OpenAI TTS API error: ${error.error?.message || response.statusText}`);
            }

            const audioBuffer = await response.arrayBuffer();
            const audioUrl = URL.createObjectURL(new Blob([audioBuffer], { type: `audio/${format}` }));

            const result: TTSResult = {
                audioBuffer,
                audioUrl,
                format,
            };

            // Cache result
            this.cache.set(cacheKey, result);

            // Limit cache size (keep last 50)
            if (this.cache.size > 50) {
                const firstKey = this.cache.keys().next().value;
                if (firstKey) {
                    this.cache.delete(firstKey);
                }
            }

            return result;
        } catch (error) {
            console.error('[OpenAI TTS] Failed to generate speech:', error);
            throw error;
        }
    }

    /**
     * Generate speech with SSML-like formatting
     */
    public async generateSpeechFormatted(
        text: string,
        options: TTSOptions = {}
    ): Promise<TTSResult> {
        // OpenAI TTS doesn't support SSML, but we can preprocess text
        // Remove SSML-like tags and adjust text
        const processedText = text
            .replace(/<break\s+time="(\d+)s?"\s*\/?>/gi, (_, time) => {
                const seconds = parseInt(time);
                return '. '.repeat(Math.ceil(seconds / 0.5));
            })
            .replace(/<emphasis\s+level="(strong|moderate)">/gi, '')
            .replace(/<\/emphasis>/gi, '')
            .replace(/<[^>]+>/g, '') // Remove any remaining tags
            .replace(/\s+/g, ' ')
            .trim();

        return this.generateSpeech(processedText, options);
    }

    /**
     * Generate speech and save to file (via Electron IPC)
     */
    public async generateSpeechToFile(
        text: string,
        filePath: string,
        options: TTSOptions = {}
    ): Promise<string> {
        const result = await this.generateSpeech(text, options);

        // Save via Electron IPC if available
        if (typeof window !== 'undefined' && (window as any).electronAPI?.saveTTSAudio) {
            try {
                const response = await (window as any).electronAPI.saveTTSAudio(filePath, Array.from(new Uint8Array(result.audioBuffer)));
                if (response.success) {
                    return filePath;
                } else {
                    throw new Error(response.error || 'Failed to save audio');
                }
            } catch (error) {
                console.error('[OpenAI TTS] Failed to save audio file:', error);
                throw error;
            }
        }

        // Fallback: return data URL
        return result.audioUrl;
    }

    /**
     * Get cache key for text and options
     */
    private getCacheKey(text: string, options: TTSOptions): string {
        const opts = JSON.stringify(options);
        return `${text}_${opts}`;
    }

    /**
     * Clear cache
     */
    public clearCache(): void {
        // Revoke object URLs
        this.cache.forEach(result => {
            if (result.audioUrl.startsWith('blob:')) {
                URL.revokeObjectURL(result.audioUrl);
            }
        });
        this.cache.clear();
    }

    /**
     * Update API key
     */
    public setApiKey(apiKey: string): void {
        this.apiKey = apiKey;
    }

    /**
     * Get available voices
     */
    public getVoices(): TTSVoice[] {
        return ['alloy', 'echo', 'fable', 'onyx', 'nova', 'shimmer'];
    }

    /**
     * Get voice description
     */
    public getVoiceDescription(voice: TTSVoice): string {
        const descriptions: Record<TTSVoice, string> = {
            alloy: 'Neutral, balanced voice',
            echo: 'Clear, professional voice',
            fable: 'Warm, friendly voice',
            onyx: 'Deep, authoritative voice',
            nova: 'Bright, energetic voice (recommended for news)',
            shimmer: 'Soft, gentle voice',
        };
        return descriptions[voice] || 'Unknown voice';
    }
}

// Singleton instance
let ttsInstance: OpenAITTS | null = null;

export function getOpenAITTS(): OpenAITTS {
    if (!ttsInstance) {
        ttsInstance = new OpenAITTS();
    }
    return ttsInstance;
}

export const openaiTTS = getOpenAITTS();

