/**
 * ATSC Mux Engine
 * Core logic for 4-channel ATSC statistical multiplexer
 * NOTE: External FFmpeg/TSDuck engines have been removed.
 * This module provides stub implementation for type compatibility.
 */

import type { ATSCMuxConfig, MuxChannelConfig, MuxInputSourceType } from './types';

export interface MuxChannelStatus {
    channelId: number;
    enabled: boolean;
    inputConnected: boolean;
    encodingActive: boolean;
    outputActive: boolean;
    currentBitrate?: number;
    frameRate?: number;
    errors?: string[];
}

export class ATSCMuxEngine {
    private config: ATSCMuxConfig | null = null;
    private channelStatuses: Map<number, MuxChannelStatus> = new Map();

    constructor(_api?: any) {
        // External engines removed
    }

    /**
     * Configure the mux system
     */
    setConfig(config: ATSCMuxConfig): void {
        this.config = config;
        this.initializeChannelStatuses();
    }

    /**
     * Start the mux system
     * NOTE: External engine support removed - returns error
     */
    async start(): Promise<{ success: boolean; error?: string }> {
        return { 
            success: false, 
            error: 'ATSC Mux requires external FFmpeg/TSDuck which have been removed. Use external mux setup.' 
        };
    }

    /**
     * Stop the mux system
     */
    async stop(): Promise<void> {
        this.channelStatuses.forEach((status, channelId) => {
            this.updateChannelStatus(channelId, {
                encodingActive: false,
                outputActive: false,
            });
        });
    }

    /**
     * Get status of all channels
     */
    getChannelStatuses(): MuxChannelStatus[] {
        return Array.from(this.channelStatuses.values());
    }

    /**
     * Get status of a specific channel
     */
    getChannelStatus(channelId: number): MuxChannelStatus | undefined {
        return this.channelStatuses.get(channelId);
    }

    /**
     * Initialize channel statuses based on config
     */
    private initializeChannelStatuses(): void {
        this.channelStatuses.clear();
        if (!this.config) return;

        for (const channel of this.config.channels) {
            this.channelStatuses.set(channel.channelId, {
                channelId: channel.channelId,
                enabled: channel.enabled,
                inputConnected: false,
                encodingActive: false,
                outputActive: false,
                errors: [],
            });
        }
    }

    /**
     * Update channel status
     */
    private updateChannelStatus(channelId: number, updates: Partial<MuxChannelStatus>): void {
        const current = this.channelStatuses.get(channelId);
        if (current) {
            this.channelStatuses.set(channelId, { ...current, ...updates });
        }
    }
}

// Singleton instance
let muxEngineInstance: ATSCMuxEngine | null = null;

export function getATSCMuxEngine(api?: any): ATSCMuxEngine {
    if (!muxEngineInstance) {
        muxEngineInstance = new ATSCMuxEngine(api);
    }
    return muxEngineInstance;
}
