// ============================================
// AI Control Room Controller
// Funny, imperfect, clumsy control room assistant
// ============================================

import { aiPersonalityEngine, AIResponse } from './aiPersonalityEngine';
import { EventEmitter } from 'events';

// Re-export for convenience
export type { AIResponse } from './aiPersonalityEngine';

export interface ShowEvent {
    type: 'media_change' | 'schedule_update' | 'error' | 'milestone' | 'user_action' | 'preroll_preview';
    data: Record<string, unknown>;
    timestamp: number;
}

class AIController extends EventEmitter {
    private isEnabled: boolean = false;
    private eventHistory: ShowEvent[] = [];
    private responseHistory: AIResponse[] = [];
    private autoRespond: boolean = true;
    private minResponseInterval: number = 5000; // 5 seconds
    private lastResponseTime: number = 0;

    /**
     * Enable/disable controller
     */
    public setEnabled(enabled: boolean): void {
        this.isEnabled = enabled;
        this.emit('enabledChanged', enabled);
    }

    public getEnabled(): boolean {
        return this.isEnabled;
    }

    /**
     * Handle show event and generate response
     */
    public async handleEvent(event: ShowEvent): Promise<AIResponse | null> {
        if (!this.isEnabled || !this.autoRespond) {
            return null;
        }

        // Rate limiting
        const now = Date.now();
        if (now - this.lastResponseTime < this.minResponseInterval) {
            return null;
        }

        this.eventHistory.push(event);
        if (this.eventHistory.length > 100) {
            this.eventHistory.shift();
        }

        try {
            const prompt = this.buildPromptFromEvent(event);
            const context = this.buildContext();

            const response = await aiPersonalityEngine.generateResponse(
                'controller',
                prompt,
                { ...context, event }
            );

            this.lastResponseTime = now;
            this.responseHistory.push(response);
            if (this.responseHistory.length > 50) {
                this.responseHistory.shift();
            }

            this.emit('response', response);
            return response;
        } catch (error) {
            console.error('[AIController] Failed to generate response:', error);
            return null;
        }
    }

    /**
     * Manually trigger a response
     */
    public async triggerResponse(prompt?: string): Promise<AIResponse | null> {
        if (!this.isEnabled) {
            return null;
        }

        const context = this.buildContext();
        const userPrompt = prompt || 'What\'s happening in the control room?';

        try {
            const response = await aiPersonalityEngine.generateResponse(
                'controller',
                userPrompt,
                context
            );

            this.responseHistory.push(response);
            this.emit('response', response);
            return response;
        } catch (error) {
            console.error('[AIController] Failed to generate response:', error);
            return null;
        }
    }

    /**
     * Build prompt from event
     */
    private buildPromptFromEvent(event: ShowEvent): string {
        const data = event.data;
        const isFill = data.priority === 'fill' || data.isProbabilistic;
        const eventContext = isFill ? 'probabilistic fill' : 'hard scheduled';

        switch (event.type) {
            case 'media_change':
                return `The current media just changed to: ${data.title || 'unknown'} (${eventContext}). What do you think?`;
            case 'schedule_update':
                return `The schedule was just updated. ${data.itemCount ? `${data.itemCount} items` : 'Something changed'}.`;
            case 'error':
                return `Oh no, an error occurred: ${data.message || 'something went wrong'}. How should we fix this mess?`;
            case 'milestone':
                return `We just hit a milestone: ${data.description || 'something happened'}. Celebrate with me!`;
            case 'user_action':
                return `The user just ${data.action || 'did something'}. Did they mean to do that?`;
            case 'preroll_preview':
                return `Coming up next: ${data.title}. Give us a quick teaser or comment on this upcoming segment.`;
            default:
                return 'Something happened in the control room.';
        }
    }

    /**
     * Build context from event history
     */
    private buildContext(): Record<string, unknown> {
        return {
            recentEvents: this.eventHistory.slice(-5),
            responseCount: this.responseHistory.length,
        };
    }

    /**
     * Get response history
     */
    public getResponseHistory(): AIResponse[] {
        return [...this.responseHistory];
    }

    /**
     * Get event history
     */
    public getEventHistory(): ShowEvent[] {
        return [...this.eventHistory];
    }

    /**
     * Set auto-respond
     */
    public setAutoRespond(enabled: boolean): void {
        this.autoRespond = enabled;
    }

    /**
     * Set minimum response interval
     */
    public setMinResponseInterval(ms: number): void {
        this.minResponseInterval = ms;
    }
}

// Singleton instance
let controllerInstance: AIController | null = null;

export function getAIController(): AIController {
    if (!controllerInstance) {
        controllerInstance = new AIController();
    }
    return controllerInstance;
}

export const aiController = getAIController();

