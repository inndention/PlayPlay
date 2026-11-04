// ============================================
// OpenAI API Integration
// For AI-assisted content generation
// ============================================

export interface OpenAIResponse {
    success: boolean;
    content?: string;
    error?: string;
}

/**
 * Generate speech audio (Whisper/tts-1) as a data URL for quick promo spots.
 */
export async function generateSpeech(
    text: string,
    apiKey: string,
    voice: 'alloy' | 'verse' | 'coral' | 'mono' = 'alloy',
    format: 'mp3' | 'wav' | 'flac' = 'mp3'
): Promise<{ success: boolean; audioDataUrl?: string; error?: string }> {
    if (!apiKey || !apiKey.trim()) {
        return { success: false, error: 'OpenAI API key not configured' };
    }
    if (!text.trim()) {
        return { success: false, error: 'Text is empty' };
    }
    try {
        const res = await fetch('https://api.openai.com/v1/audio/speech', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${apiKey.trim()}`,
            },
            body: JSON.stringify({
                model: 'gpt-4o-mini-tts',
                voice,
                input: text,
                format,
            }),
        });
        if (!res.ok) {
            const err = await res.json().catch(() => ({}));
            return { success: false, error: err.error?.message || `TTS failed: ${res.status}` };
        }
        const buffer = await res.arrayBuffer();
        const base64 = Buffer.from(buffer).toString('base64');
        const audioDataUrl = `data:audio/${format};base64,${base64}`;
        return { success: true, audioDataUrl };
    } catch (error) {
        return { success: false, error: error instanceof Error ? error.message : 'Unknown error' };
    }
}

/**
 * Generate Facebook story content using OpenAI
 * @param prompt - User prompt or context
 * @param apiKey - OpenAI API key
 * @param context - Additional context (e.g., current program, time of day)
 */
export async function generateStoryContent(
    prompt: string,
    apiKey: string,
    context?: {
        currentProgram?: string;
        timeOfDay?: string;
        stationName?: string;
    }
): Promise<OpenAIResponse> {
    if (!apiKey || !apiKey.trim()) {
        return { success: false, error: 'OpenAI API key not configured' };
    }

    try {
        // Build context-aware prompt
        let fullPrompt = prompt;
        if (context) {
            const contextParts: string[] = [];
            if (context.currentProgram) {
                contextParts.push(`Current program: ${context.currentProgram}`);
            }
            if (context.timeOfDay) {
                contextParts.push(`Time: ${context.timeOfDay}`);
            }
            if (context.stationName) {
                contextParts.push(`Station: ${context.stationName}`);
            }
            if (contextParts.length > 0) {
                fullPrompt = `${contextParts.join('. ')}. ${prompt}`;
            }
        }

        // Add instructions for Facebook story format
        const systemPrompt = `You are a social media content creator for a TV/radio station. 
Generate engaging, concise Facebook story content (max 220 characters).
Make it attention-grabbing, relevant, and appropriate for the context provided.
Keep it professional but friendly.`;

        const response = await fetch('https://api.openai.com/v1/chat/completions', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${apiKey.trim()}`,
            },
            body: JSON.stringify({
                model: 'gpt-4o-mini', // Use cheaper model for stories
                messages: [
                    { role: 'system', content: systemPrompt },
                    { role: 'user', content: fullPrompt },
                ],
                max_tokens: 150,
                temperature: 0.8,
            }),
        });

        if (!response.ok) {
            const errorData = await response.json().catch(() => ({}));
            return {
                success: false,
                error: errorData.error?.message || `OpenAI API error: ${response.status} ${response.statusText}`,
            };
        }

        const data = await response.json();
        const content = data.choices?.[0]?.message?.content?.trim();

        if (!content) {
            return { success: false, error: 'No content generated' };
        }

        return { success: true, content };
    } catch (error) {
        console.error('[OpenAI] Error generating story:', error);
        return {
            success: false,
            error: error instanceof Error ? error.message : 'Unknown error',
        };
    }
}

/**
 * Generate multiple story variations
 */
export async function generateStoryVariations(
    prompt: string,
    apiKey: string,
    count: number = 3,
    context?: {
        currentProgram?: string;
        timeOfDay?: string;
        stationName?: string;
    }
): Promise<OpenAIResponse & { variations?: string[] }> {
    if (!apiKey || !apiKey.trim()) {
        return { success: false, error: 'OpenAI API key not configured' };
    }

    try {
        const variations: string[] = [];
        
        // Generate multiple variations in parallel
        const promises = Array.from({ length: count }, (_, i) => 
            generateStoryContent(`${prompt} (variation ${i + 1})`, apiKey, context)
        );

        const results = await Promise.all(promises);
        
        for (const result of results) {
            if (result.success && result.content) {
                variations.push(result.content);
            }
        }

        if (variations.length === 0) {
            return { success: false, error: 'Failed to generate any variations' };
        }

        return { success: true, variations };
    } catch (error) {
        console.error('[OpenAI] Error generating variations:', error);
        return {
            success: false,
            error: error instanceof Error ? error.message : 'Unknown error',
        };
    }
}





