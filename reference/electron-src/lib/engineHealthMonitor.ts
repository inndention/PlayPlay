/**
 * ENGINE HEALTH MONITOR
 * 
 * Monitors automation engine health and provides diagnostics.
 * 
 * Features:
 * - Real-time health scoring
 * - Anomaly detection
 * - Performance trending
 * - Alert system for critical issues
 * - Automatic recovery suggestions
 */

export type HealthLevel = 'healthy' | 'degraded' | 'critical' | 'offline';

export type AlertSeverity = 'info' | 'warning' | 'error' | 'critical';

export interface HealthAlert {
    id: string;
    timestamp: number;
    severity: AlertSeverity;
    category: 'timing' | 'playback' | 'media' | 'system' | 'network';
    message: string;
    details?: Record<string, unknown>;
    acknowledged: boolean;
    autoRecoveryAttempted: boolean;
}

export interface HealthMetrics {
    // Timing metrics
    tickRate: number;           // Actual ticks per second
    targetTickRate: number;     // Expected ticks per second
    tickRateDeviation: number;  // % deviation from target
    averageTickJitter: number;  // ms
    maxTickJitter: number;      // ms
    missedTicks: number;        // Total missed
    missedTickRate: number;     // % of ticks missed
    
    // Playback metrics
    driftEvents: number;        // Number of drift corrections
    averageDrift: number;       // ms
    maxDrift: number;           // ms
    gapEvents: number;          // Number of gap detections
    mediaLoadFailures: number;
    
    // System metrics
    memoryUsage?: number;       // MB
    cpuUsage?: number;          // % (if available)
    frameDrops: number;
    
    // Uptime
    engineUptime: number;       // ms
    lastRestartReason?: string;
}

export interface HealthSnapshot {
    timestamp: number;
    level: HealthLevel;
    score: number;              // 0-100
    metrics: HealthMetrics;
    alerts: HealthAlert[];
    recommendations: string[];
}

// Thresholds for health scoring
const THRESHOLDS = {
    // Tick rate deviation
    tickRateDeviation: {
        healthy: 5,      // < 5% deviation
        degraded: 15,    // < 15% deviation  
        critical: 30,    // > 30% deviation
    },
    // Jitter
    jitter: {
        healthy: 10,     // < 10ms
        degraded: 30,    // < 30ms
        critical: 100,   // > 100ms
    },
    // Drift
    drift: {
        healthy: 50,     // < 50ms
        degraded: 200,   // < 200ms
        critical: 1000,  // > 1000ms
    },
    // Gap events (per hour)
    gapsPerHour: {
        healthy: 0,
        degraded: 2,
        critical: 5,
    },
};

/**
 * Engine Health Monitor
 */
export class EngineHealthMonitor {
    private metrics: HealthMetrics;
    private alerts: HealthAlert[] = [];
    private alertCounter = 0;
    private snapshots: HealthSnapshot[] = [];
    private maxSnapshots = 60; // Keep 60 snapshots (1 minute at 1Hz)
    
    // Listeners — use Set so duplicates and stale refs don't accumulate
    private alertListeners: Set<(alert: HealthAlert) => void> = new Set();
    private healthListeners: Set<(snapshot: HealthSnapshot) => void> = new Set();
    
    // Sampling
    private tickSamples: number[] = [];
    private driftSamples: number[] = [];
    private startTime = 0;
    private lastSnapshotTime = 0;
    
    constructor() {
        this.metrics = this.createEmptyMetrics();
        this.startTime = performance.now();
    }
    
    /**
     * Record a tick event
     */
    recordTick(jitter: number, missed: boolean): void {
        this.tickSamples.push(jitter);
        if (this.tickSamples.length > 100) {
            this.tickSamples.shift();
        }
        
        if (jitter > this.metrics.maxTickJitter) {
            this.metrics.maxTickJitter = jitter;
        }
        
        if (missed) {
            this.metrics.missedTicks++;
            this.raiseAlert('warning', 'timing', 'Missed tick detected', { jitter });
        }
        
        // Update average
        this.metrics.averageTickJitter = 
            this.tickSamples.reduce((a, b) => a + b, 0) / this.tickSamples.length;
        
        // Check for critical jitter
        if (jitter > THRESHOLDS.jitter.critical) {
            this.raiseAlert('error', 'timing', `Critical jitter: ${jitter.toFixed(1)}ms`, { jitter });
        }
    }
    
    /**
     * Record a drift event
     */
    recordDrift(drift: number, correctionAction: string): void {
        this.driftSamples.push(Math.abs(drift));
        if (this.driftSamples.length > 50) {
            this.driftSamples.shift();
        }
        
        this.metrics.driftEvents++;
        
        if (Math.abs(drift) > this.metrics.maxDrift) {
            this.metrics.maxDrift = Math.abs(drift);
        }
        
        this.metrics.averageDrift = 
            this.driftSamples.reduce((a, b) => a + b, 0) / this.driftSamples.length;
        
        // Alert on significant drift
        if (Math.abs(drift) > THRESHOLDS.drift.degraded) {
            this.raiseAlert('warning', 'playback', 
                `Drift correction: ${drift.toFixed(0)}ms (${correctionAction})`, 
                { drift, correctionAction });
        }
        
        if (Math.abs(drift) > THRESHOLDS.drift.critical) {
            this.raiseAlert('error', 'playback', 
                `Critical drift: ${drift.toFixed(0)}ms`, 
                { drift, correctionAction });
        }
    }
    
    /**
     * Record a gap event (no content available)
     */
    recordGap(): void {
        this.metrics.gapEvents++;
        this.raiseAlert('critical', 'playback', 'GAP DETECTED - No content available');
    }
    
    /**
     * Record media load failure
     */
    recordMediaFailure(mediaId: string, error: string): void {
        this.metrics.mediaLoadFailures++;
        this.raiseAlert('error', 'media', `Media load failed: ${mediaId}`, { error });
    }
    
    /**
     * Record frame drop
     */
    recordFrameDrop(): void {
        this.metrics.frameDrops++;
    }
    
    /**
     * Update tick rate metrics
     */
    updateTickRate(actual: number, target: number): void {
        this.metrics.tickRate = actual;
        this.metrics.targetTickRate = target;
        this.metrics.tickRateDeviation = 
            Math.abs((actual - target) / target) * 100;
        this.metrics.missedTickRate = 
            this.metrics.missedTicks / Math.max(1, this.tickSamples.length) * 100;
    }
    
    /**
     * Update engine uptime
     */
    updateUptime(uptime: number): void {
        this.metrics.engineUptime = uptime;
    }
    
    /**
     * Get current health snapshot
     */
    getSnapshot(): HealthSnapshot {
        const score = this.calculateHealthScore();
        const level = this.determineHealthLevel(score);
        const recommendations = this.generateRecommendations();
        
        const snapshot: HealthSnapshot = {
            timestamp: performance.now(),
            level,
            score,
            metrics: { ...this.metrics },
            alerts: this.alerts.filter(a => !a.acknowledged).slice(-10),
            recommendations,
        };
        
        // Store snapshot
        this.snapshots.push(snapshot);
        if (this.snapshots.length > this.maxSnapshots) {
            this.snapshots.shift();
        }
        
        // Notify listeners
        this.healthListeners.forEach(listener => listener(snapshot));
        
        return snapshot;
    }
    
    /**
     * Get health trend (last N snapshots)
     */
    getTrend(count = 30): HealthSnapshot[] {
        return this.snapshots.slice(-count);
    }
    
    /**
     * Calculate overall health score (0-100)
     */
    private calculateHealthScore(): number {
        let score = 100;

        // Tick-rate deviation: linear penalty scaled to the degraded/critical band
        if (this.metrics.tickRateDeviation > THRESHOLDS.tickRateDeviation.healthy) {
            // 0 pts lost at healthy boundary, 20 pts lost at critical (30%)
            const band = THRESHOLDS.tickRateDeviation.critical - THRESHOLDS.tickRateDeviation.healthy;
            const excess = this.metrics.tickRateDeviation - THRESHOLDS.tickRateDeviation.healthy;
            score -= Math.min(20, (excess / band) * 20);
        }

        // Jitter: proportional to the degraded→critical window (10ms → 100ms)
        if (this.metrics.averageTickJitter > THRESHOLDS.jitter.healthy) {
            const band = THRESHOLDS.jitter.critical - THRESHOLDS.jitter.healthy;
            const excess = this.metrics.averageTickJitter - THRESHOLDS.jitter.healthy;
            score -= Math.min(20, (excess / band) * 20);
        }

        // Drift: proportional to the degraded→critical window (50ms → 1000ms)
        if (this.metrics.averageDrift > THRESHOLDS.drift.healthy) {
            const band = THRESHOLDS.drift.critical - THRESHOLDS.drift.healthy;
            const excess = this.metrics.averageDrift - THRESHOLDS.drift.healthy;
            score -= Math.min(25, (excess / band) * 25);
        }

        // Gap events: 15 pts per gap, capped at 30 — gaps are broadcast-critical
        score -= Math.min(30, this.metrics.gapEvents * 15);

        // Media failures: 5 pts each, capped at 15
        score -= Math.min(15, this.metrics.mediaLoadFailures * 5);

        // Missed ticks: linear penalty in the 1%–15% band
        if (this.metrics.missedTickRate > 1) {
            score -= Math.min(20, ((this.metrics.missedTickRate - 1) / 14) * 20);
        }

        return Math.max(0, Math.round(score));
    }
    
    /**
     * Determine health level from score
     */
    private determineHealthLevel(score: number): HealthLevel {
        if (score >= 80) return 'healthy';
        if (score >= 50) return 'degraded';
        if (score >= 20) return 'critical';
        return 'offline';
    }
    
    /**
     * Generate recommendations based on current state
     */
    private generateRecommendations(): string[] {
        const recommendations: string[] = [];
        
        if (this.metrics.averageTickJitter > THRESHOLDS.jitter.degraded) {
            recommendations.push('High jitter detected. Consider closing other applications or reducing system load.');
        }
        
        if (this.metrics.tickRateDeviation > THRESHOLDS.tickRateDeviation.degraded) {
            recommendations.push('Tick rate unstable. The browser may be throttling background tabs.');
        }
        
        if (this.metrics.averageDrift > THRESHOLDS.drift.degraded) {
            recommendations.push('Playback drift detected. Check media file integrity and network stability.');
        }
        
        if (this.metrics.gapEvents > 0) {
            recommendations.push('Gaps detected in playback. Ensure schedule has sufficient content and fill media is configured.');
        }
        
        if (this.metrics.mediaLoadFailures > 3) {
            recommendations.push('Multiple media load failures. Verify media paths and file accessibility.');
        }
        
        if (this.metrics.missedTickRate > 5) {
            recommendations.push('High missed tick rate. Engine may be overloaded. Consider reducing tick frequency.');
        }
        
        return recommendations;
    }
    
    /**
     * Raise an alert
     */
    raiseAlert(
        severity: AlertSeverity,
        category: HealthAlert['category'],
        message: string,
        details?: Record<string, unknown>
    ): void {
        const alert: HealthAlert = {
            id: `alert_${++this.alertCounter}`,
            timestamp: performance.now(),
            severity,
            category,
            message,
            details,
            acknowledged: false,
            autoRecoveryAttempted: false,
        };
        
        this.alerts.push(alert);
        if (this.alerts.length > 100) {
            this.alerts.shift();
        }
        
        // Log to console
        const prefix = `[HealthMonitor:${severity}]`;
        switch (severity) {
            case 'info':
                console.info(prefix, message, details || '');
                break;
            case 'warning':
                console.warn(prefix, message, details || '');
                break;
            case 'error':
            case 'critical':
                console.error(prefix, message, details || '');
                break;
        }
        
        // Notify listeners
        this.alertListeners.forEach(listener => listener(alert));
    }
    
    /**
     * Acknowledge an alert
     */
    acknowledgeAlert(alertId: string): void {
        const alert = this.alerts.find(a => a.id === alertId);
        if (alert) {
            alert.acknowledged = true;
        }
    }
    
    /**
     * Acknowledge all alerts
     */
    acknowledgeAllAlerts(): void {
        this.alerts.forEach(a => a.acknowledged = true);
    }
    
    /**
     * Clear all alerts
     */
    clearAlerts(): void {
        this.alerts = [];
    }
    
    /**
     * Subscribe to alerts
     */
    onAlert(callback: (alert: HealthAlert) => void): () => void {
        this.alertListeners.add(callback);
        return () => this.alertListeners.delete(callback);
    }

    onHealthUpdate(callback: (snapshot: HealthSnapshot) => void): () => void {
        this.healthListeners.add(callback);
        return () => this.healthListeners.delete(callback);
    }
    
    /**
     * Reset all metrics
     */
    reset(): void {
        this.metrics = this.createEmptyMetrics();
        this.alerts = [];
        this.snapshots = [];
        this.tickSamples = [];
        this.driftSamples = [];
        this.startTime = performance.now();
    }
    
    /**
     * Create empty metrics object
     */
    private createEmptyMetrics(): HealthMetrics {
        return {
            tickRate: 0,
            targetTickRate: 10,
            tickRateDeviation: 0,
            averageTickJitter: 0,
            maxTickJitter: 0,
            missedTicks: 0,
            missedTickRate: 0,
            driftEvents: 0,
            averageDrift: 0,
            maxDrift: 0,
            gapEvents: 0,
            mediaLoadFailures: 0,
            frameDrops: 0,
            engineUptime: 0,
        };
    }
}

// ========================================
// SINGLETON
// ========================================

let monitorInstance: EngineHealthMonitor | null = null;

export function getHealthMonitor(): EngineHealthMonitor {
    if (!monitorInstance) {
        monitorInstance = new EngineHealthMonitor();
    }
    return monitorInstance;
}

export function resetHealthMonitor(): void {
    if (monitorInstance) {
        monitorInstance.reset();
    }
}


