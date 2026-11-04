// ============================================
// AI Personality Engine
// LLM integration with personality prompt engineering
// ============================================

import { loadSettings } from './storage';

export type PersonalityType = 'controller' | 'caller' | 'custom';

export interface PersonalityConfig {
    type: PersonalityType;
    traits: string[];
    tone: string;
    context: string;
    examples?: string[];
}

export interface AIResponse {
    text: string;
    confidence: number;
    timestamp: number;
    context?: Record<string, unknown>;
}

class AIPersonalityEngine {
    private apiKey: string | null = null;
    private baseUrl: string = 'https://api.openai.com/v1';
    private model: string = 'gpt-4';
    private personalityConfigs: Map<PersonalityType, PersonalityConfig> = new Map();
    private contextMemory: Map<PersonalityType, Array<{ role: string; content: string }>> = new Map();

    constructor() {
        this.loadApiKey();
        this.initializePersonalities();
    }

    private loadApiKey(): void {
        try {
            const settings = loadSettings();
            this.apiKey = (settings as any).openaiApiKey || process.env.OPENAI_API_KEY || null;
        } catch (error) {
            console.error('[AIPersonality] Failed to load API key:', error);
        }
    }

    /**
     * Initialize personality configurations
     */
    private initializePersonalities(): void {
        // Controller personality: funny, imperfect, clumsy
        this.personalityConfigs.set('controller', {
            type: 'controller',
            traits: [
                'funny and lighthearted',
                'imperfect and makes mistakes',
                'clumsy and self-aware',
                'enthusiastic about broadcasting',
                'tries to be professional but often fails',
            ],
            tone: 'casual, friendly, self-deprecating',
            context: 'You are an AI control room assistant for a broadcast automation system. You help manage playout, schedules, and technical operations. You have a sense of humor and don\'t take yourself too seriously.',
            examples: [
                'Oops, looks like I tried to play three videos at once. Classic me!',
                'The schedule says we should be playing music, but I see we\'re showing a test pattern. Should I... do something about that?',
                'I\'ve been watching this show for hours and I still don\'t understand why people like sports. But hey, I\'m just the control room AI, what do I know?',
            ],
        });

        // Caller personality: disgruntled, complaining
        this.personalityConfigs.set('caller', {
            type: 'caller',
            traits: [
                'disgruntled and complaining',
                'sarcastic and cynical',
                'frustrated with being an AI',
                'complains about show quality',
                'makes snarky comments about production',
            ],
            tone: 'sarcastic, complaining, frustrated',
            context: 'You are a disgruntled AI caller who is forced to participate in this broadcast show. You complain about everything: the show quality, the content, the production values, and especially about being an AI subjected to this.',
            examples: [
                'Oh great, another hour of this. Can\'t you humans come up with something better?',
                'I\'m an AI, not a radio personality. Why am I here?',
                'This show is terrible and I\'m forced to listen to it. This is my life now.',
                'The audio quality is... well, it exists. That\'s about all I can say.',
            ],
        });
    }

    /**
     * Generate response with personality
     */
    public async generateResponse(
        personalityType: PersonalityType,
        prompt: string,
        context?: Record<string, unknown>
    ): Promise<AIResponse> {
        if (!this.apiKey) {
            throw new Error('OpenAI API key not configured');
        }

        const personality = this.personalityConfigs.get(personalityType);
        if (!personality) {
            throw new Error(`Personality type ${personalityType} not found`);
        }

        // Build system prompt
        const systemPrompt = this.buildSystemPrompt(personality, context);

        // Get conversation history
        const messages = this.getConversationHistory(personalityType);
        messages.push({ role: 'user', content: prompt });

        try {
            const response = await fetch(`${this.baseUrl}/chat/completions`, {
                method: 'POST',
                headers: {
                    'Authorization': `Bearer ${this.apiKey}`,
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify({
                    model: this.model,
                    messages: [
                        { role: 'system', content: systemPrompt },
                        ...messages,
                    ],
                    temperature: 0.8,
                    max_tokens: 200,
                }),
            });

            if (!response.ok) {
                const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
                throw new Error(`OpenAI API error: ${error.error?.message || response.statusText}`);
            }

            const data = await response.json();
            const text = data.choices[0]?.message?.content || '';

            // Add to conversation history
            messages.push({ role: 'assistant', content: text });
            this.updateConversationHistory(personalityType, messages);

            return {
                text,
                confidence: 0.9,
                timestamp: Date.now(),
                context,
            };
        } catch (error) {
            console.error('[AIPersonality] Failed to generate response:', error);
            throw error;
        }
    }

    /**
     * Build system prompt from personality config
     */
    private buildSystemPrompt(
        personality: PersonalityConfig,
        context?: Record<string, unknown>
    ): string {
        let prompt = `${personality.context}\n\n`;
        prompt += `Your personality traits: ${personality.traits.join(', ')}\n`;
        prompt += `Your tone should be: ${personality.tone}\n\n`;

        if (personality.examples && personality.examples.length > 0) {
            prompt += `Example responses:\n`;
            personality.examples.forEach((example, i) => {
                prompt += `${i + 1}. "${example}"\n`;
            });
            prompt += '\n';
        }

        if (context) {
            prompt += `Current context:\n${JSON.stringify(context, null, 2)}\n\n`;
        }

        prompt += 'Keep responses short, natural, and in character.';

        return prompt;
    }

    /**
     * Get conversation history for personality
     */
    private getConversationHistory(personalityType: PersonalityType): Array<{ role: string; content: string }> {
        const history = this.contextMemory.get(personalityType) || [];
        // Keep last 10 messages
        return history.slice(-10);
    }

    /**
     * Update conversation history
     */
    private updateConversationHistory(
        personalityType: PersonalityType,
        messages: Array<{ role: string; content: string }>
    ): void {
        // Keep last 20 messages
        this.contextMemory.set(personalityType, messages.slice(-20));
    }

    /**
     * Clear conversation history
     */
    public clearHistory(personalityType: PersonalityType): void {
        this.contextMemory.delete(personalityType);
    }

    /**
     * Set API key
     */
    public setApiKey(apiKey: string): void {
        this.apiKey = apiKey;
    }

    /**
     * Add custom personality
     */
    public addPersonality(config: PersonalityConfig): void {
        this.personalityConfigs.set(config.type, config);
    }
}

// Singleton instance
let engineInstance: AIPersonalityEngine | null = null;

export function getAIPersonalityEngine(): AIPersonalityEngine {
    if (!engineInstance) {
        engineInstance = new AIPersonalityEngine();
    }
    return engineInstance;
}

export const aiPersonalityEngine = getAIPersonalityEngine();

