/**
 * PRECISION TIMER MODULE
 * 
 * High-reliability timing for broadcast automation.
 * 
 * Key features:
 * - Uses performance.now() for monotonic timestamps (immune to system clock changes)
 * - Delta-time accumulation to handle missed ticks
 * - Automatic drift compensation
 * - Watchdog for stall detection
 * - Jitter measurement and logging
 * - Frame-accurate mode using requestAnimationFrame
 * - Self-healing with exponential backoff on errors
 */

export interface TimerConfig {
    /** Target tick rate in Hz (default: 10) */
    targetHz: number;
    /** Maximum allowed jitter before logging warning (ms) */
    jitterWarningThreshold: number;
    /** Maximum accumulated drift before correction (ms) */
    maxAccumulatedDrift: number;
    /** Enable frame-accurate mode using RAF (syncs to display refresh) */
    frameAccurate: boolean;
    /** Watchdog timeout - if no tick for this long, restart (ms) */
    watchdogTimeout: number;
    /** Enable verbose logging */
    verbose: boolean;
}

export const DEFAULT_TIMER_CONFIG: TimerConfig = {
    targetHz: 20, // 20Hz = 50ms intervals (better than 10Hz for smooth playback)
    jitterWarningThreshold: 20, // Warn if tick is >20ms off schedule
    maxAccumulatedDrift: 100, // Reset accumulator if >100ms drift
    frameAccurate: false, // Use setInterval by default, RAF optional
    watchdogTimeout: 1000, // 1s watchdog
    verbose: false,
};

export interface TimerStats {
    totalTicks: number;
    missedTicks: number;
    averageJitter: number;
    maxJitter: number;
    minJitter: number;
    lastTickTime: number;
    uptime: number;
    isHealthy: boolean;
    healthScore: number; // 0-100
}

export interface TimerCallback {
    (delta: number, now: number, stats: TimerStats): void;
}

type TickSource = 'interval' | 'raf' | 'hybrid';

/**
 * High-precision timer with drift compensation and health monitoring
 */
export class PrecisionTimer {
    private config: TimerConfig;
    private callback: TimerCallback | null = null;
    
    // Timing state
    private isRunning = false;
    private startTime = 0;
    private lastTickTime = 0;
    private accumulator = 0;
    private targetInterval: number;
    
    // Handle refs
    private intervalHandle: number | null = null;
    private rafHandle: number | null = null;
    private watchdogHandle: number | null = null;
    
    // Stats
    private tickCount = 0;
    private missedTicks = 0;
    private jitterSamples: number[] = [];
    private maxJitter = 0;
    private minJitter = Infinity;
    
    // Health
    private consecutiveErrors = 0;
    private lastWatchdogPet = 0;
    private isStalled = false;
    
    // Tick source strategy
    private tickSource: TickSource = 'interval';
    
    constructor(config: Partial<TimerConfig> = {}) {
        this.config = { ...DEFAULT_TIMER_CONFIG, ...config };
        this.targetInterval = 1000 / this.config.targetHz;
        
        // Determine tick source
        if (this.config.frameAccurate) {
            this.tickSource = 'raf';
        } else {
            this.tickSource = 'interval';
        }
    }
    
    /**
     * Start the precision timer
     */
    start(callback: TimerCallback): void {
        if (this.isRunning) {
            this.log('warn', 'Timer already running');
            return;
        }
        
        this.callback = callback;
        this.isRunning = true;
        this.startTime = performance.now();
        this.lastTickTime = this.startTime;
        this.lastWatchdogPet = this.startTime;
        this.accumulator = 0;
        this.tickCount = 0;
        this.missedTicks = 0;
        this.jitterSamples = [];
        this.maxJitter = 0;
        this.minJitter = Infinity;
        this.consecutiveErrors = 0;
        this.isStalled = false;
        
        this.log('info', `Starting precision timer @ ${this.config.targetHz}Hz (${this.tickSource} mode)`);
        
        // Start tick source
        this.startTickSource();
        
        // Start watchdog
        this.startWatchdog();
    }
    
    /**
     * Stop the timer
     */
    stop(): void {
        if (!this.isRunning) return;
        
        this.isRunning = false;
        this.callback = null;
        
        // Stop tick source
        if (this.intervalHandle !== null) {
            clearInterval(this.intervalHandle);
            this.intervalHandle = null;
        }
        if (this.rafHandle !== null) {
            cancelAnimationFrame(this.rafHandle);
            this.rafHandle = null;
        }
        if (this.watchdogHandle !== null) {
            clearInterval(this.watchdogHandle);
            this.watchdogHandle = null;
        }
        
        this.log('info', 'Timer stopped', { stats: this.getStats() });
    }
    
    /**
     * Get current timer statistics
     */
    getStats(): TimerStats {
        const now = performance.now();
        const avgJitter = this.jitterSamples.length > 0
            ? this.jitterSamples.reduce((a, b) => a + b, 0) / this.jitterSamples.length
            : 0;
        
        // Health score: 100 = perfect, decreases with missed ticks and jitter
        const missRate = this.tickCount > 0 ? this.missedTicks / this.tickCount : 0;
        const jitterPenalty = Math.min(avgJitter / this.targetInterval, 1);
        const healthScore = Math.max(0, Math.round(100 * (1 - missRate * 0.5 - jitterPenalty * 0.5)));
        
        return {
            totalTicks: this.tickCount,
            missedTicks: this.missedTicks,
            averageJitter: avgJitter,
            maxJitter: this.maxJitter === 0 ? 0 : this.maxJitter,
            minJitter: this.minJitter === Infinity ? 0 : this.minJitter,
            lastTickTime: this.lastTickTime,
            uptime: now - this.startTime,
            isHealthy: !this.isStalled && this.consecutiveErrors < 3 && healthScore > 50,
            healthScore,
        };
    }
    
    /**
     * Get monotonic timestamp (ms since timer start)
     */
    now(): number {
        return performance.now() - this.startTime;
    }
    
    /**
     * Get wall clock timestamp (but using monotonic reference)
     */
    wallClock(): number {
        return performance.now();
    }
    
    /**
     * Force immediate tick (for testing/manual override)
     */
    forceTick(): void {
        if (this.isRunning && this.callback) {
            const now = performance.now();
            const delta = now - this.lastTickTime;
            this.lastTickTime = now;
            this.tickCount++;
            this.callback(delta, now, this.getStats());
        }
    }
    
    // ========================================
    // PRIVATE
    // ========================================
    
    private startTickSource(): void {
        switch (this.tickSource) {
            case 'raf':
                this.startRAFLoop();
                break;
            case 'interval':
            default:
                this.startIntervalLoop();
                break;
        }
    }
    
    /**
     * Interval-based tick source with drift compensation
     */
    private startIntervalLoop(): void {
        // Use a slightly faster interval to compensate for JS timing delays
        const compensatedInterval = Math.max(1, this.targetInterval - 1);
        
        this.intervalHandle = window.setInterval(() => {
            this.processTick();
        }, compensatedInterval);
    }
    
    /**
     * RAF-based tick source for frame-accurate timing
     */
    private startRAFLoop(): void {
        const rafCallback = () => {
            if (!this.isRunning) return;
            
            this.processTick();
            this.rafHandle = requestAnimationFrame(rafCallback);
        };
        
        this.rafHandle = requestAnimationFrame(rafCallback);
    }
    
    /**
     * Core tick processing with delta accumulation
     */
    private processTick(): void {
        const now = performance.now();
        const delta = now - this.lastTickTime;
        
        // Measure jitter (how far off from expected interval)
        const expectedDelta = this.targetInterval;
        const jitter = Math.abs(delta - expectedDelta);
        
        this.recordJitter(jitter);
        
        // Warn on excessive jitter
        if (jitter > this.config.jitterWarningThreshold) {
            this.log('warn', `High jitter detected: ${jitter.toFixed(1)}ms (expected ${expectedDelta}ms)`);
        }
        
        // Pet the watchdog immediately on entry — before the callback — so a slow
        // callback doesn't trigger a false stall detection.
        this.lastWatchdogPet = now;
        this.accumulator += delta;
        this.lastTickTime = now;
        
        // Check for missed ticks (if accumulator > 2x interval, we missed ticks)
        const ticksToProcess = Math.floor(this.accumulator / this.targetInterval);
        if (ticksToProcess > 1) {
            const missed = ticksToProcess - 1;
            this.missedTicks += missed;
            this.log('warn', `Missed ${missed} tick(s), processing catch-up`);
        }
        
        // Cap accumulator when it exceeds the drift limit — but keep one full interval's
        // worth so the next tick fires immediately rather than discarding all budget.
        if (this.accumulator > this.config.maxAccumulatedDrift) {
            this.log('warn', `Accumulated drift too high (${this.accumulator.toFixed(1)}ms), capping`);
            this.accumulator = this.targetInterval;
        }
        
        // Process tick(s)
        try {
            while (this.accumulator >= this.targetInterval) {
                this.tickCount++;
                this.accumulator -= this.targetInterval;

                if (this.callback) {
                    this.callback(this.targetInterval, now, this.getStats());
                }
            }
            // Reset error streak only after a fully clean tick pass
            this.consecutiveErrors = 0;
        } catch (error) {
            this.consecutiveErrors++;
            this.log('error', `Tick callback error: ${(error as Error).message}`);

            if (this.consecutiveErrors >= 3) {
                this.log('error', `Too many consecutive errors, entering recovery mode`);
                this.enterRecoveryMode();
            }
        }
    }
    
    /**
     * Record jitter sample (rolling window)
     */
    private recordJitter(jitter: number): void {
        this.jitterSamples.push(jitter);
        if (this.jitterSamples.length > 100) {
            this.jitterSamples.shift();
        }
        
        this.maxJitter = Math.max(this.maxJitter, jitter);
        this.minJitter = Math.min(this.minJitter, jitter);
    }
    
    /**
     * Watchdog to detect engine stalls
     */
    private startWatchdog(): void {
        this.watchdogHandle = window.setInterval(() => {
            const now = performance.now();
            const timeSinceLastTick = now - this.lastWatchdogPet;
            
            if (timeSinceLastTick > this.config.watchdogTimeout) {
                this.log('error', `WATCHDOG: Timer stalled for ${timeSinceLastTick.toFixed(0)}ms`);
                this.isStalled = true;
                this.restartTickSource();
            } else {
                this.isStalled = false;
            }
        }, this.config.watchdogTimeout / 2);
    }
    
    /**
     * Restart tick source after stall
     */
    private restartTickSource(): void {
        this.log('warn', 'Restarting tick source...');
        
        // Stop current
        if (this.intervalHandle !== null) {
            clearInterval(this.intervalHandle);
            this.intervalHandle = null;
        }
        if (this.rafHandle !== null) {
            cancelAnimationFrame(this.rafHandle);
            this.rafHandle = null;
        }
        
        // Reset timing state
        this.lastTickTime = performance.now();
        this.lastWatchdogPet = this.lastTickTime;
        this.accumulator = 0;
        
        // Restart
        this.startTickSource();
    }
    
    /**
     * Enter recovery mode after repeated errors
     */
    private enterRecoveryMode(): void {
        if (this.intervalHandle !== null) {
            clearInterval(this.intervalHandle);
            this.intervalHandle = null;
        }

        let recoveryTicks = 0;
        // Run at half speed; exit after 5 consecutive clean ticks
        this.intervalHandle = window.setInterval(() => {
            this.processTick();
            if (this.consecutiveErrors === 0) {
                recoveryTicks++;
                if (recoveryTicks >= 5) {
                    this.log('info', 'Exiting recovery mode — engine stable');
                    this.restartTickSource();
                }
            } else {
                recoveryTicks = 0;
            }
        }, this.targetInterval * 2);
    }
    
    /**
     * Logging helper
     */
    private log(level: 'debug' | 'info' | 'warn' | 'error', message: string, data?: unknown): void {
        if (level === 'debug' && !this.config.verbose) return;
        
        const prefix = '[PrecisionTimer]';
        switch (level) {
            case 'debug':
                console.debug(prefix, message, data || '');
                break;
            case 'info':
                console.info(prefix, message, data || '');
                break;
            case 'warn':
                console.warn(prefix, message, data || '');
                break;
            case 'error':
                console.error(prefix, message, data || '');
                break;
        }
    }
}

// ========================================
// UTILITY FUNCTIONS
// ========================================

/**
 * Convert performance.now() to wall clock Date
 * Note: This is still affected by system clock, but timing intervals are not
 */
export function perfToDate(perfTime: number): Date {
    const offset = Date.now() - performance.now();
    return new Date(perfTime + offset);
}

/**
 * Get high-resolution timestamp
 */
export function hrtime(): number {
    return performance.now();
}

/**
 * Sleep using requestAnimationFrame (more accurate than setTimeout for short durations)
 */
export function preciseSleep(ms: number): Promise<void> {
    return new Promise(resolve => {
        const start = performance.now();
        
        const check = () => {
            if (performance.now() - start >= ms) {
                resolve();
            } else {
                requestAnimationFrame(check);
            }
        };
        
        if (ms <= 0) {
            resolve();
        } else if (ms < 16) {
            // For very short sleeps, use RAF
            requestAnimationFrame(check);
        } else {
            // For longer sleeps, combine setTimeout + RAF
            setTimeout(() => {
                requestAnimationFrame(check);
            }, ms - 16);
        }
    });
}

/**
 * Measure execution time of a function
 */
export function measureTime<T>(fn: () => T): { result: T; duration: number } {
    const start = performance.now();
    const result = fn();
    const duration = performance.now() - start;
    return { result, duration };
}

/**
 * Create a debounced version of a function with precise timing
 */
export function preciseDebounce<T extends (...args: unknown[]) => void>(
    fn: T,
    wait: number
): (...args: Parameters<T>) => void {
    let timeoutId: number | null = null;
    let lastCall = 0;
    
    return (...args: Parameters<T>) => {
        const now = performance.now();
        const remaining = wait - (now - lastCall);
        
        if (timeoutId !== null) {
            clearTimeout(timeoutId);
        }
        
        if (remaining <= 0) {
            lastCall = now;
            fn(...args);
        } else {
            timeoutId = window.setTimeout(() => {
                lastCall = performance.now();
                fn(...args);
                timeoutId = null;
            }, remaining);
        }
    };
}

// ========================================
// SINGLETON INSTANCE
// ========================================

let timerInstance: PrecisionTimer | null = null;

export function getTimer(config?: Partial<TimerConfig>): PrecisionTimer {
    if (!timerInstance) {
        timerInstance = new PrecisionTimer(config);
    }
    return timerInstance;
}

export function resetTimer(): void {
    if (timerInstance) {
        timerInstance.stop();
        timerInstance = null;
    }
}


