// ============================================
// OBS Branch Output Plugin Integration
// Controls Branch Output filters for multi-channel ATSC output
// ============================================

import { OBSWebSocketClient } from './obsWebSocket';
import { generateId } from './utils';

export interface BranchOutputFilterConfig {
    filterName: string;
    sourceName: string;
    sceneName?: string;
    
    // Output settings
    outputTarget: string; // File path or RTMP/SRT URL
    videoBitrate: number; // kbps
    audioBitrate: number; // kbps
    resolution?: string; // e.g., "1920x1080"
    framerate?: number;
    
    // Encoder settings
    videoEncoder?: string; // e.g., "x264", "h264_nvenc", "mpeg2video"
    audioEncoder?: string; // e.g., "aac", "ac3"
    
    // ATSC specific
    enableATSC?: boolean; // Use AC-3 audio
    containerFormat?: string; // "mpegts" for ATSC
    
    // Audio source
    audioSource?: 'filter' | 'any_source' | 'track1' | 'track2' | 'track3' | 'track4' | 'track5' | 'track6' | 'none';
    
    enabled: boolean;
}

export interface BranchOutputStatus {
    filterName: string;
    sourceName: string;
    active: boolean;
    streaming: boolean;
    recording: boolean;
    bitrate?: number;
    frameRate?: number;
    errors?: string[];
}

export class OBSBranchOutputManager {
    private client: OBSWebSocketClient;
    private filters: Map<string, BranchOutputFilterConfig> = new Map();
    private statuses: Map<string, BranchOutputStatus> = new Map();

    constructor(client: OBSWebSocketClient) {
        this.client = client;
    }

    /**
     * Add or update a Branch Output filter on a source
     */
    async addBranchOutputFilter(config: BranchOutputFilterConfig): Promise<{ success: boolean; error?: string }> {
        try {
            if (!this.client.isConnected) {
                return { success: false, error: 'OBS not connected' };
            }

            // Check if filter already exists
            const sourceFilters = await this.client.getSourceFilterList(config.sourceName);
            const existingFilter = sourceFilters.filters?.find(f => f.filterName === config.filterName);

            if (existingFilter) {
                // Update existing filter
                await this.updateBranchOutputFilter(config);
            } else {
                // Create new filter
                await this.client.call('CreateSourceFilter', {
                    sourceName: config.sourceName,
                    filterName: config.filterName,
                    filterKind: 'branch-output-mod', // Plugin filter ID
                    filterSettings: this.buildFilterSettings(config),
                });
            }

            this.filters.set(config.filterName, config);
            return { success: true };
        } catch (error: any) {
            return { success: false, error: error.message || 'Failed to add Branch Output filter' };
        }
    }

    /**
     * Update Branch Output filter settings
     */
    async updateBranchOutputFilter(config: BranchOutputFilterConfig): Promise<{ success: boolean; error?: string }> {
        try {
            if (!this.client.isConnected) {
                return { success: false, error: 'OBS not connected' };
            }

            await this.client.call('SetSourceFilterSettings', {
                sourceName: config.sourceName,
                filterName: config.filterName,
                filterSettings: this.buildFilterSettings(config),
            });

            this.filters.set(config.filterName, config);
            return { success: true };
        } catch (error: any) {
            return { success: false, error: error.message || 'Failed to update Branch Output filter' };
        }
    }

    /**
     * Enable/disable Branch Output filter (via eye icon)
     */
    async setBranchOutputEnabled(filterName: string, sourceName: string, enabled: boolean): Promise<{ success: boolean; error?: string }> {
        try {
            if (!this.client.isConnected) {
                return { success: false, error: 'OBS not connected' };
            }

            await this.client.call('SetSourceFilterEnabled', {
                sourceName: sourceName,
                filterName: filterName,
                filterEnabled: enabled,
            });

            const config = this.filters.get(filterName);
            if (config) {
                config.enabled = enabled;
                this.filters.set(filterName, config);
            }

            return { success: true };
        } catch (error: any) {
            return { success: false, error: error.message || 'Failed to set Branch Output enabled' };
        }
    }

    /**
     * Remove Branch Output filter
     */
    async removeBranchOutputFilter(filterName: string, sourceName: string): Promise<{ success: boolean; error?: string }> {
        try {
            if (!this.client.isConnected) {
                return { success: false, error: 'OBS not connected' };
            }

            await this.client.call('RemoveSourceFilter', {
                sourceName: sourceName,
                filterName: filterName,
            });

            this.filters.delete(filterName);
            this.statuses.delete(filterName);
            return { success: true };
        } catch (error: any) {
            return { success: false, error: error.message || 'Failed to remove Branch Output filter' };
        }
    }

    /**
     * Get Branch Output filter status
     */
    async getBranchOutputStatus(filterName: string, sourceName: string): Promise<BranchOutputStatus | null> {
        try {
            if (!this.client.isConnected) {
                return null;
            }

            const filterInfo = await this.client.call('GetSourceFilter', {
                sourceName: sourceName,
                filterName: filterName,
            }) as { filterEnabled?: boolean } | null;

            if (!filterInfo) {
                return null;
            }

            const config = this.filters.get(filterName);
            const status: BranchOutputStatus = {
                filterName,
                sourceName,
                active: filterInfo.filterEnabled ?? false,
                streaming: false, // Would need plugin-specific API to get this
                recording: false, // Would need plugin-specific API to get this
            };

            this.statuses.set(filterName, status);
            return status;
        } catch (error) {
            console.error('[BranchOutput] Failed to get status:', error);
            return null;
        }
    }

    /**
     * Get all Branch Output filters
     */
    getAllFilters(): BranchOutputFilterConfig[] {
        return Array.from(this.filters.values());
    }

    /**
     * Get all Branch Output statuses
     */
    getAllStatuses(): BranchOutputStatus[] {
        return Array.from(this.statuses.values());
    }

    /**
     * Build filter settings object for Branch Output plugin
     */
    private buildFilterSettings(config: BranchOutputFilterConfig): any {
        const settings: any = {
            output_target: config.outputTarget,
            video_bitrate: config.videoBitrate,
            audio_bitrate: config.audioBitrate,
        };

        if (config.resolution) {
            settings.resolution = config.resolution;
        }

        if (config.framerate) {
            settings.framerate = config.framerate;
        }

        if (config.videoEncoder) {
            settings.video_encoder = config.videoEncoder;
        }

        if (config.audioEncoder) {
            settings.audio_encoder = config.audioEncoder;
        }

        if (config.enableATSC) {
            settings.enable_atsc = true;
            settings.audio_encoder = 'ac3';
            settings.container_format = 'mpegts';
        }

        if (config.containerFormat) {
            settings.container_format = config.containerFormat;
        }

        if (config.audioSource) {
            settings.audio_source = config.audioSource;
        }

        return settings;
    }

    /**
     * Setup multiple Branch Output filters for ATSC multi-channel output
     */
    async setupATSCChannels(
        channels: Array<{
            channelId: number;
            sourceName: string;
            sceneName?: string;
            outputPath: string; // File path for MPEG-TS output
            videoBitrate: number;
            audioBitrate: number;
            resolution: string;
            framerate: number;
        }>
    ): Promise<{ success: boolean; errors?: string[] }> {
        const errors: string[] = [];

        for (const channel of channels) {
            const filterName = `ATSC_Channel_${channel.channelId}`;
            
            const config: BranchOutputFilterConfig = {
                filterName,
                sourceName: channel.sourceName,
                sceneName: channel.sceneName,
                outputTarget: channel.outputPath,
                videoBitrate: channel.videoBitrate,
                audioBitrate: channel.audioBitrate,
                resolution: channel.resolution,
                framerate: channel.framerate,
                videoEncoder: 'libx264', // H.264 for YOLO mode
                audioEncoder: 'ac3',
                enableATSC: true,
                containerFormat: 'mpegts',
                audioSource: 'filter',
                enabled: true,
            };

            const result = await this.addBranchOutputFilter(config);
            if (!result.success) {
                errors.push(`Channel ${channel.channelId}: ${result.error}`);
            }
        }

        return {
            success: errors.length === 0,
            errors: errors.length > 0 ? errors : undefined,
        };
    }
}

