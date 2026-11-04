/**
 * NDI Engine
 * NDI source discovery and input handling using NDI SDK
 */

import type { NdiSource, NdiConfig } from './types';

export class NdiEngine {
    private api: any;
    private discoveryInterval: NodeJS.Timeout | null = null;
    private sources: Map<string, NdiSource> = new Map();
    private listeners: Set<(sources: NdiSource[]) => void> = new Set();

    constructor(api: any) {
        this.api = api;
    }

    /**
     * Start NDI source discovery
     */
    async startDiscovery(intervalMs: number = 5000): Promise<void> {
        if (!this.api) {
            return;
        }

        // Initial discovery
        await this.discoverSources();

        // Periodic discovery
        if (this.discoveryInterval) {
            clearInterval(this.discoveryInterval);
        }

        this.discoveryInterval = setInterval(async () => {
            await this.discoverSources();
        }, intervalMs);
    }

    /**
     * Stop NDI source discovery
     */
    stopDiscovery(): void {
        if (this.discoveryInterval) {
            clearInterval(this.discoveryInterval);
            this.discoveryInterval = null;
        }
    }

    /**
     * Discover NDI sources on network
     */
    async discoverSources(): Promise<NdiSource[]> {
        if (!this.api) {
            return [];
        }

        try {
            const result = await this.api.ndiDiscoverSources?.();
            if (result?.success && result.sources) {
                const newSources: NdiSource[] = result.sources;
                
                // Update sources map
                newSources.forEach(source => {
                    this.sources.set(source.name, {
                        ...source,
                        lastSeen: Date.now(),
                    });
                });

                // Remove stale sources (not seen in last 30 seconds)
                const now = Date.now();
                for (const [name, source] of this.sources.entries()) {
                    if (source.lastSeen && (now - source.lastSeen) > 30000) {
                        this.sources.delete(name);
                    }
                }

                const sourcesArray = Array.from(this.sources.values());
                this.notifyListeners(sourcesArray);
                return sourcesArray;
            }
        } catch (error) {
            console.error('[NDI Engine] Discovery failed:', error);
        }

        return Array.from(this.sources.values());
    }

    /**
     * Get current discovered sources
     */
    getSources(): NdiSource[] {
        return Array.from(this.sources.values());
    }

    /**
     * Create NDI receiver for a source
     */
    async createReceiver(config: NdiConfig): Promise<{ success: boolean; receiverId?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.ndiCreateReceiver?.(config);
        return result || { success: false, error: 'NDI receiver creation not available' };
    }

    /**
     * Get receiver status
     */
    async getReceiverStatus(receiverId: string): Promise<{ 
        success: boolean; 
        connected?: boolean; 
        resolution?: string; 
        framerate?: number;
        error?: string;
    }> {
        if (!this.api) {
            return { success: false };
        }

        return await this.api.ndiGetReceiverStatus?.(receiverId) || { success: false };
    }

    /**
     * Create NDI sender for output
     */
    async createSender(sourceName: string, options?: {
        groups?: string;
        clockVideo?: boolean;
        clockAudio?: boolean;
    }): Promise<{ success: boolean; senderId?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.ndiCreateSender?.({
            sourceName,
            groups: options?.groups,
            clockVideo: options?.clockVideo ?? true,
            clockAudio: options?.clockAudio ?? true,
        });
        
        return result || { success: false, error: 'NDI sender creation not available' };
    }

    /**
     * Send video frame to NDI sender
     */
    async sendVideoFrame(
        senderId: string,
        frameData: Buffer | ArrayBuffer,
        width: number,
        height: number,
        format: 'RGBA' | 'RGB' | 'UYVY' = 'RGBA'
    ): Promise<{ success: boolean; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        return await this.api.ndiSendVideoFrame?.(senderId, frameData, width, height, format) || 
            { success: false, error: 'NDI video frame sending not available' };
    }

    /**
     * Send audio frame to NDI sender
     */
    async sendAudioFrame(
        senderId: string,
        audioData: Buffer | ArrayBuffer,
        sampleRate: number,
        channels: number,
        samples: number
    ): Promise<{ success: boolean; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        return await this.api.ndiSendAudioFrame?.(senderId, audioData, sampleRate, channels, samples) || 
            { success: false, error: 'NDI audio frame sending not available' };
    }

    /**
     * Get sender status
     */
    async getSenderStatus(senderId: string): Promise<{ 
        success: boolean; 
        connected?: boolean;
        receivers?: number;
        error?: string;
    }> {
        if (!this.api) {
            return { success: false };
        }

        return await this.api.ndiGetSenderStatus?.(senderId) || { success: false };
    }

    /**
     * Destroy NDI sender
     */
    async destroySender(senderId: string): Promise<{ success: boolean; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        return await this.api.ndiDestroySender?.(senderId) || 
            { success: false, error: 'NDI sender destruction not available' };
    }

    /**
     * Subscribe to source updates
     */
    onSourcesUpdate(callback: (sources: NdiSource[]) => void): () => void {
        this.listeners.add(callback);
        return () => this.listeners.delete(callback);
    }

    private notifyListeners(sources: NdiSource[]): void {
        this.listeners.forEach(callback => {
            try {
                callback(sources);
            } catch (error) {
                console.error('[NDI Engine] Listener error:', error);
            }
        });
    }
}




