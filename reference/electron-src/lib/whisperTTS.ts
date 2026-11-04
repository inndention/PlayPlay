// ============================================
// OpenAI Whisper TTS Integration
// Text-to-speech for slate narration
// ============================================

import { ExtendedSettings } from './types';
import { loadSettings } from './storage';

export interface TTSOptions {
    voice?: 'alloy' | 'echo' | 'fable' | 'onyx' | 'nova' | 'shimmer' | string;
    speed?: number; // 0.25 to 4.0
    model?: 'tts-1' | 'tts-1-hd' | 'kokoro' | string;
    provider?: 'auto' | 'kokoro-local' | 'starnews' | 'openai';
}

async function generateKokoroLocalTTS(text: string, options: TTSOptions, settings: ExtendedSettings): Promise<Blob | null> {
    const baseUrl = (settings.kokoroTtsBaseUrl || 'http://127.0.0.1:8880').replace(/\/$/, '');
    const response = await fetch(`${baseUrl}/v1/audio/speech`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            model: options.model || 'kokoro',
            input: text,
            voice: options.voice || settings.kokoroVoice || 'af_heart',
            speed: options.speed || 1.0,
            response_format: 'wav',
        }),
    });

    if (!response.ok) {
        throw new Error(`Kokoro TTS error: ${response.status} - ${await response.text()}`);
    }

    return response.blob();
}

// Generate TTS audio from text
// Supports both Star News narrate endpoint and direct OpenAI API
export async function generateTTS(text: string, options: TTSOptions = {}): Promise<Blob | null> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        const provider = options.provider || settings.ttsProvider || 'auto';

        if (provider === 'kokoro-local' || provider === 'auto') {
            try {
                return await generateKokoroLocalTTS(text, options, settings);
            } catch (kokoroError) {
                if (provider === 'kokoro-local') throw kokoroError;
                console.warn('[WhisperTTS] Kokoro local TTS unavailable, trying next provider:', kokoroError);
            }
        }
        
        // Try Star News narrate endpoint first (queues TTS/Whisper-style narration)
        if ((provider === 'starnews' || provider === 'auto') && settings.starNewsApiBaseUrl) {
            try {
                const narrateUrl = `${settings.starNewsApiBaseUrl}/api/v1/extra.php?resource=narrate`;
                const response = await fetch(narrateUrl, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        text: text,
                        mode: options.voice || 'alloy', // Use voice as mode (history/weather/alerts)
                        random_voice: true, // Star News API supports random voice selection
                    }),
                });
                
                if (response.ok) {
                    const data = await response.json();
                    // If the API returns audio directly
                    if (data.audio_url) {
                        const audioResponse = await fetch(data.audio_url);
                        if (audioResponse.ok) {
                            return await audioResponse.blob();
                        }
                    }
                    // If it returns task ID, we'd need to poll (for now, fall through to OpenAI)
                    if (data.task_id) {
                        console.log('[WhisperTTS] TTS queued, task ID:', data.task_id);
                        // TODO: Implement polling for task completion
                    }
                }
            } catch (starNewsError) {
                console.warn('[WhisperTTS] Star News narrate endpoint failed, falling back to OpenAI:', starNewsError);
            }
        }

        if (provider !== 'openai' && provider !== 'auto') {
            return null;
        }
        
        // Fallback to direct OpenAI API
        const apiKey = settings.openaiApiKey;
        if (!apiKey) {
            console.warn('[WhisperTTS] No OpenAI API key configured');
            return null;
        }

        const response = await fetch('https://api.openai.com/v1/audio/speech', {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${apiKey}`,
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                model: options.model || 'tts-1',
                input: text,
                voice: options.voice || 'alloy',
                speed: options.speed || 1.0,
            }),
        });

        if (!response.ok) {
            const error = await response.text();
            throw new Error(`OpenAI TTS API error: ${response.status} - ${error}`);
        }

        const audioBlob = await response.blob();
        return audioBlob;
    } catch (error) {
        console.error('[WhisperTTS] Error generating TTS:', error);
        return null;
    }
}

// Convert blob to audio element URL
export function blobToAudioURL(blob: Blob): string {
    return URL.createObjectURL(blob);
}

// Generate TTS and return as audio element
export async function generateTTSAudio(
    text: string,
    options: TTSOptions = {}
): Promise<HTMLAudioElement | null> {
    const blob = await generateTTS(text, options);
    if (!blob) return null;

    const url = blobToAudioURL(blob);
    const audio = new Audio(url);
    
    return audio;
}
