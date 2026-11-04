// ============================================
// Statistical Multiplexing Controller
// Dynamically allocates bitrate across multiple channels
// to efficiently use ATSC 19.3 Mbps bandwidth
// ============================================

export interface StatMuxConfig {
    enabled: boolean;
    totalBitrate: number; // 19392658 for ATSC
    minChannelBitrate: number; // Minimum per channel (e.g., 1 Mbps)
    maxChannelBitrate: number; // Maximum per channel (e.g., 8 Mbps)
    updateInterval: number; // How often to recalculate (ms)
    complexityWindow: number; // Time window for complexity analysis (seconds)
    priorityWeights?: Map<number, number>; // Channel ID -> priority weight (1.0 = normal)
}

export interface ChannelComplexity {
    channelId: number;
    currentBitrate: number;
    targetBitrate: number;
    complexity: number; // 0-1, higher = more complex
    frameType: 'I' | 'P' | 'B'; // Current frame type
    motionLevel: number; // 0-1, higher = more motion
    lastUpdate: number;
}

export interface StatMuxAllocation {
    channelId: number;
    allocatedBitrate: number;
    videoBitrate: number;
    audioBitrate: number;
    reason: string;
}

export class StatMuxController {
    private config: StatMuxConfig;
    private channelComplexities: Map<number, ChannelComplexity> = new Map();
    private allocations: Map<number, StatMuxAllocation> = new Map();
    private updateTimer: ReturnType<typeof setInterval> | null = null;
    private onAllocationChange: ((allocations: StatMuxAllocation[]) => void) | null = null;

    constructor(config: StatMuxConfig) {
        this.config = config;
    }

    /**
     * Initialize channel for statmux
     */
    public initializeChannel(channelId: number, initialBitrate: number): void {
        this.channelComplexities.set(channelId, {
            channelId,
            currentBitrate: initialBitrate,
            targetBitrate: initialBitrate,
            complexity: 0.5, // Start at medium complexity
            frameType: 'P',
            motionLevel: 0.5,
            lastUpdate: Date.now(),
        });
    }

    /**
     * Update channel complexity metrics
     */
    public updateChannelComplexity(
        channelId: number,
        metrics: {
            complexity?: number;
            frameType?: 'I' | 'P' | 'B';
            motionLevel?: number;
            currentBitrate?: number;
        }
    ): void {
        const channel = this.channelComplexities.get(channelId);
        if (!channel) {
            this.initializeChannel(channelId, this.config.totalBitrate / 4); // Default to equal share
            return;
        }

        if (metrics.complexity !== undefined) {
            channel.complexity = Math.max(0, Math.min(1, metrics.complexity));
        }
        if (metrics.frameType !== undefined) {
            channel.frameType = metrics.frameType;
        }
        if (metrics.motionLevel !== undefined) {
            channel.motionLevel = Math.max(0, Math.min(1, metrics.motionLevel));
        }
        if (metrics.currentBitrate !== undefined) {
            channel.currentBitrate = metrics.currentBitrate;
        }
        channel.lastUpdate = Date.now();

        this.channelComplexities.set(channelId, channel);
    }

    /**
     * Calculate optimal bitrate allocation across all channels
     */
    public calculateAllocations(enabledChannelIds: number[]): StatMuxAllocation[] {
        if (!this.config.enabled || enabledChannelIds.length === 0) {
            // Return equal allocation if statmux disabled
            const equalShare = this.config.totalBitrate / enabledChannelIds.length;
            return enabledChannelIds.map(id => ({
                channelId: id,
                allocatedBitrate: equalShare,
                videoBitrate: Math.round(equalShare * 0.9), // 90% video, 10% audio overhead
                audioBitrate: Math.round(equalShare * 0.1),
                reason: 'Equal allocation (statmux disabled)',
            }));
        }

        // Calculate complexity scores for each channel
        const channelScores = new Map<number, number>();
        const now = Date.now();

        for (const channelId of enabledChannelIds) {
            const channel = this.channelComplexities.get(channelId);
            if (!channel) {
                this.initializeChannel(channelId, this.config.totalBitrate / enabledChannelIds.length);
                continue;
            }

            // Calculate score based on complexity, motion, and frame type
            let score = channel.complexity * 0.4; // Base complexity
            score += channel.motionLevel * 0.3; // Motion adds complexity
            score += channel.frameType === 'I' ? 0.2 : channel.frameType === 'P' ? 0.1 : 0.05; // I-frames need more bitrate

            // Apply priority weight if configured
            const priority = this.config.priorityWeights?.get(channelId) || 1.0;
            score *= priority;

            // Penalize stale data
            const age = (now - channel.lastUpdate) / 1000;
            if (age > this.config.complexityWindow) {
                score *= 0.5; // Reduce score for stale data
            }

            channelScores.set(channelId, score);
        }

        // Normalize scores to sum to 1.0
        const totalScore = Array.from(channelScores.values()).reduce((sum, s) => sum + s, 0);
        if (totalScore === 0) {
            // Fallback to equal allocation
            const equalShare = this.config.totalBitrate / enabledChannelIds.length;
            return enabledChannelIds.map(id => ({
                channelId: id,
                allocatedBitrate: equalShare,
                videoBitrate: Math.round(equalShare * 0.9),
                audioBitrate: Math.round(equalShare * 0.1),
                reason: 'Equal allocation (no complexity data)',
            }));
        }

        // Allocate bitrate proportionally to scores
        const allocations: StatMuxAllocation[] = [];
        const reservedForAudio = enabledChannelIds.length * 192000; // 192 kbps per channel for AC-3
        const availableForVideo = this.config.totalBitrate - reservedForAudio;

        for (const channelId of enabledChannelIds) {
            const score = channelScores.get(channelId) || 0;
            const normalizedScore = score / totalScore;
            
            // Allocate video bitrate
            let videoBitrate = Math.round(availableForVideo * normalizedScore);
            
            // Apply min/max constraints
            videoBitrate = Math.max(this.config.minChannelBitrate, Math.min(this.config.maxChannelBitrate, videoBitrate));
            
            const audioBitrate = 192000; // Fixed AC-3 bitrate
            const totalAllocated = videoBitrate + audioBitrate;

            const channel = this.channelComplexities.get(channelId);
            const reason = channel
                ? `Complexity: ${(channel.complexity * 100).toFixed(1)}%, Motion: ${(channel.motionLevel * 100).toFixed(1)}%, Frame: ${channel.frameType}`
                : 'Default allocation';

            allocations.push({
                channelId,
                allocatedBitrate: totalAllocated,
                videoBitrate,
                audioBitrate,
                reason,
            });
        }

        // Ensure total doesn't exceed limit (redistribute if needed)
        const totalAllocated = allocations.reduce((sum, a) => sum + a.allocatedBitrate, 0);
        if (totalAllocated > this.config.totalBitrate) {
            const scale = this.config.totalBitrate / totalAllocated;
            for (const alloc of allocations) {
                alloc.videoBitrate = Math.round(alloc.videoBitrate * scale);
                alloc.allocatedBitrate = alloc.videoBitrate + alloc.audioBitrate;
            }
        }

        // Store allocations
        for (const alloc of allocations) {
            this.allocations.set(alloc.channelId, alloc);
        }

        return allocations;
    }

    /**
     * Get current allocation for a channel
     */
    public getAllocation(channelId: number): StatMuxAllocation | null {
        return this.allocations.get(channelId) || null;
    }

    /**
     * Get all current allocations
     */
    public getAllAllocations(): StatMuxAllocation[] {
        return Array.from(this.allocations.values());
    }

    /**
     * Start automatic statmux updates
     */
    public startAutoUpdate(
        enabledChannelIds: number[],
        onUpdate: (allocations: StatMuxAllocation[]) => void
    ): void {
        this.onAllocationChange = onUpdate;

        if (this.updateTimer) {
            clearInterval(this.updateTimer);
        }

        // Calculate initial allocation
        const initial = this.calculateAllocations(enabledChannelIds);
        onUpdate(initial);

        // Update periodically
        this.updateTimer = setInterval(() => {
            const allocations = this.calculateAllocations(enabledChannelIds);
            onUpdate(allocations);
        }, this.config.updateInterval);
    }

    /**
     * Stop automatic updates
     */
    public stopAutoUpdate(): void {
        if (this.updateTimer) {
            clearInterval(this.updateTimer);
            this.updateTimer = null;
        }
        this.onAllocationChange = null;
    }

    /**
     * Update configuration
     */
    public updateConfig(config: Partial<StatMuxConfig>): void {
        this.config = { ...this.config, ...config };
    }

    /**
     * Get current configuration
     */
    public getConfig(): StatMuxConfig {
        return { ...this.config };
    }
}

