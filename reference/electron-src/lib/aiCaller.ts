// ============================================
// Disgruntled AI Caller
// AI caller that complains about the show
// ============================================

import { aiPersonalityEngine, AIResponse } from './aiPersonalityEngine';
import { EventEmitter } from 'events';

// Re-export AIResponse for convenience
export type { AIResponse } from './aiPersonalityEngine';

export interface CallEvent {
    type: 'call_start' | 'call_end' | 'interruption' | 'complaint';
    data: Record<string, unknown>;
    timestamp: number;
}

class AICaller extends EventEmitter {
    private isEnabled: boolean = false;
    private isOnAir: boolean = false;
    private callHistory: CallEvent[] = [];
    private responseHistory: AIResponse[] = [];
    private intensity: number = 0.7; // 0.0 to 1.0

    /**
     * Enable/disable caller
     */
    public setEnabled(enabled: boolean): void {
        this.isEnabled = enabled;
        this.emit('enabledChanged', enabled);
    }

    public getEnabled(): boolean {
        return this.isEnabled;
    }

    /**
     * Set complaint intensity
     */
    public setIntensity(intensity: number): void {
        this.intensity = Math.max(0, Math.min(1, intensity));
    }

    public getIntensity(): number {
        return this.intensity;
    }

    /**
     * Start a call (interrupt the show)
     */
    public async startCall(context?: Record<string, unknown>): Promise<AIResponse | null> {
        if (!this.isEnabled) {
            return null;
        }

        this.isOnAir = true;
        const event: CallEvent = {
            type: 'call_start',
            data: context || {},
            timestamp: Date.now(),
        };

        this.callHistory.push(event);

        try {
            const prompt = this.buildCallPrompt('start', context);
            const response = await aiPersonalityEngine.generateResponse(
                'caller',
                prompt,
                { ...context, intensity: this.intensity, isOnAir: true }
            );

            this.responseHistory.push(response);
            this.emit('callStarted', response);
            return response;
        } catch (error) {
            console.error('[AICaller] Failed to start call:', error);
            return null;
        }
    }

    /**
     * End current call
     */
    public async endCall(): Promise<void> {
        if (!this.isOnAir) return;

        this.isOnAir = false;
        const event: CallEvent = {
            type: 'call_end',
            data: {},
            timestamp: Date.now(),
        };

        this.callHistory.push(event);
        this.emit('callEnded');
    }

    /**
     * Generate a complaint about the show
     */
    public async generateComplaint(context?: Record<string, unknown>): Promise<AIResponse | null> {
        if (!this.isEnabled) {
            return null;
        }

        const event: CallEvent = {
            type: 'complaint',
            data: context || {},
            timestamp: Date.now(),
        };

        this.callHistory.push(event);

        try {
            const prompt = this.buildCallPrompt('complaint', context);
            const response = await aiPersonalityEngine.generateResponse(
                'caller',
                prompt,
                { ...context, intensity: this.intensity }
            );

            this.responseHistory.push(response);
            this.emit('complaint', response);
            return response;
        } catch (error) {
            console.error('[AICaller] Failed to generate complaint:', error);
            return null;
        }
    }

    /**
     * Interrupt show with a call
     */
    public async interruptShow(context?: Record<string, unknown>): Promise<AIResponse | null> {
        if (!this.isEnabled || this.isOnAir) {
            return null;
        }

        const event: CallEvent = {
            type: 'interruption',
            data: context || {},
            timestamp: Date.now(),
        };

        this.callHistory.push(event);

        // Start call automatically
        return this.startCall({ ...context, isInterruption: true });
    }

    /**
     * Build prompt for call
     */
    private buildCallPrompt(type: 'start' | 'complaint', context?: Record<string, unknown>): string {
        const intensityText = this.intensity > 0.8 ? 'very' : this.intensity > 0.5 ? 'moderately' : 'mildly';

        switch (type) {
            case 'start':
                return `You're calling into this show. You're ${intensityText} disgruntled about being an AI forced to participate. Start your call with a complaint.`;
            case 'complaint':
                return `Complain about the show. You're ${intensityText} frustrated. What's wrong with this broadcast?`;
            default:
                return 'Say something disgruntled about this show.';
        }
    }

    /**
     * Get call history
     */
    public getCallHistory(): CallEvent[] {
        return [...this.callHistory];
    }

    /**
     * Get response history
     */
    public getResponseHistory(): AIResponse[] {
        return [...this.responseHistory];
    }

    /**
     * Check if currently on air
     */
    public getIsOnAir(): boolean {
        return this.isOnAir;
    }
}

// Singleton instance
let callerInstance: AICaller | null = null;

export function getAICaller(): AICaller {
    if (!callerInstance) {
        callerInstance = new AICaller();
    }
    return callerInstance;
}

export const aiCaller = getAICaller();

