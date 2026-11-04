// ============================================
// Centralized Error Handling Service
// Non-intrusive error tally system
// ============================================

import { ErrorEntry } from './types';

class ErrorHandler {
    private errors: Map<string, ErrorEntry> = new Map();
    private listeners: Set<(errors: ErrorEntry[]) => void> = new Set();
    private instanceErrors: Map<string, ErrorEntry[]> = new Map();
    private errorThrottle: Map<string, { count: number; lastTime: number }> = new Map();
    private suppressedErrors: Set<string> = new Set();
    private buzzerEnabled: boolean = true;
    private audioContext: AudioContext | null = null;

    logError(instanceId: string, severity: 'warning' | 'error' | 'critical', message: string, source: string, details?: any): string {
        // Throttle repeated errors - don't log same error multiple times quickly
        const errorKey = `${source}:${message}`;
        const now = Date.now();
        const throttle = this.errorThrottle.get(errorKey);
        
        if (throttle) {
            // If same error within 5 seconds, increment count but don't create new entry
            if (now - throttle.lastTime < 5000) {
                throttle.count++;
                throttle.lastTime = now;
                // Update existing error with count
                const existing = Array.from(this.errors.values()).find(e => 
                    e.source === source && e.message === message && !e.acknowledged
                );
                if (existing) {
                    existing.details = { ...existing.details, repeatCount: throttle.count };
                    this.errors.set(existing.id, existing);
                    this.notifyListeners();
                    return existing.id;
                }
            }
        } else {
            this.errorThrottle.set(errorKey, { count: 1, lastTime: now });
        }

        // Check if error is suppressed
        if (this.suppressedErrors.has(errorKey)) {
            return ''; // Don't log suppressed errors
        }

        const errorId = `err_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        
        const error: ErrorEntry = {
            id: errorId,
            instanceId,
            timestamp: Date.now(),
            severity,
            message,
            source,
            acknowledged: false,
            details: { ...details, repeatCount: throttle?.count || 1 },
        };

        this.errors.set(errorId, error);

        // Group by instance
        if (!this.instanceErrors.has(instanceId)) {
            this.instanceErrors.set(instanceId, []);
        }
        this.instanceErrors.get(instanceId)!.push(error);

        // Play buzzer for critical errors (if enabled)
        if (severity === 'critical' && this.buzzerEnabled) {
            this.playBuzzer();
        }

        // Auto-dismiss low-severity errors after timeout
        if (severity === 'warning') {
            setTimeout(() => {
                this.acknowledgeError(errorId);
            }, 30000); // 30 seconds
        }

        this.notifyListeners();
        return errorId;
    }

    private playBuzzer() {
        try {
            if (typeof window === 'undefined') return;
            const AudioCtx = window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
            if (!AudioCtx) return;

            // Reuse a single AudioContext — creating one per buzz leaks OS audio resources
            if (!this.audioContext || this.audioContext.state === 'closed') {
                this.audioContext = new AudioCtx();
            }
            if (this.audioContext.state === 'suspended') {
                void this.audioContext.resume();
            }

            const ctx = this.audioContext;
            const oscillator = ctx.createOscillator();
            const gainNode = ctx.createGain();

            oscillator.connect(gainNode);
            gainNode.connect(ctx.destination);

            oscillator.frequency.value = 800;
            oscillator.type = 'sine';
            gainNode.gain.setValueAtTime(0.3, ctx.currentTime);
            gainNode.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.3);

            oscillator.start(ctx.currentTime);
            oscillator.stop(ctx.currentTime + 0.3);
        } catch (e) {
            console.error('[ErrorHandler] Failed to play buzzer:', e);
        }
    }

    suppressError(errorKey: string) {
        this.suppressedErrors.add(errorKey);
    }

    setBuzzerEnabled(enabled: boolean) {
        this.buzzerEnabled = enabled;
    }

    acknowledgeError(errorId: string): void {
        const error = this.errors.get(errorId);
        if (error) {
            error.acknowledged = true;
            this.errors.set(errorId, error);
            this.notifyListeners();
        }
    }

    acknowledgeAll(instanceId: string): void {
        const errors = this.instanceErrors.get(instanceId) || [];
        errors.forEach(err => {
            if (!err.acknowledged) {
                err.acknowledged = true;
                this.errors.set(err.id, err);
            }
        });
        this.notifyListeners();
    }

    resetInstance(instanceId: string): void {
        const errors = this.instanceErrors.get(instanceId) || [];
        errors.forEach(err => this.errors.delete(err.id));
        this.instanceErrors.delete(instanceId);
        this.notifyListeners();
    }

    getErrors(instanceId?: string): ErrorEntry[] {
        if (instanceId) {
            return this.instanceErrors.get(instanceId) || [];
        }
        return Array.from(this.errors.values());
    }

    getUnacknowledgedCount(instanceId?: string): number {
        const errors = instanceId 
            ? (this.instanceErrors.get(instanceId) || [])
            : Array.from(this.errors.values());
        return errors.filter(e => !e.acknowledged).length;
    }

    getInstanceIds(): string[] {
        return Array.from(this.instanceErrors.keys());
    }

    onUpdate(callback: (errors: ErrorEntry[]) => void): () => void {
        this.listeners.add(callback);
        return () => this.listeners.delete(callback);
    }

    private notifyListeners(): void {
        const allErrors = Array.from(this.errors.values());
        this.listeners.forEach(callback => {
            try {
                callback(allErrors);
            } catch (e) {
                console.error('[ErrorHandler] Listener error:', e);
            }
        });
    }
}

export const errorHandler = new ErrorHandler();

// Global error logging helper
export function logError(instanceId: string, severity: 'warning' | 'error' | 'critical', message: string, source: string, details?: any): string {
    return errorHandler.logError(instanceId, severity, message, source, details);
}

