/**
 * Auto-Publish Service
 * Automatically publishes recordings to YouTube/Facebook when recording ends
 */

import { publishRecordingToYouTube } from './youtube';
import { FacebookLiveSession } from './facebookLiveProducer';

export interface AutoPublishConfig {
    enabled: boolean;
    platforms: {
        youtube: boolean;
        facebook: boolean;
    };
    youtubeChannelId?: string;
    youtubePlaylistId?: string;
    facebookPageId?: string;
    defaultTitle?: string;
    defaultDescription?: string;
    defaultTags?: string[];
}

export interface RecordingMetadata {
    id: string;
    filePath: string;
    title: string;
    description?: string;
    tags?: string[];
    duration: number;
    startTime: Date;
    endTime: Date;
    sessionId?: string;
    sessionName?: string;
}

export class AutoPublishService {
    private config: AutoPublishConfig;
    private onPublishCallback?: (recording: RecordingMetadata, platform: 'youtube' | 'facebook', result: any) => void;
    private onErrorCallback?: (recording: RecordingMetadata, platform: 'youtube' | 'facebook', error: Error) => void;

    constructor(config?: Partial<AutoPublishConfig>) {
        this.config = {
            enabled: true,
            platforms: {
                youtube: false,
                facebook: false,
            },
            defaultTitle: 'Live Recording',
            defaultDescription: 'Automatically published recording',
            defaultTags: [],
            ...config,
        };
    }

    /**
     * Handle recording completion and auto-publish
     */
    async handleRecordingComplete(recording: RecordingMetadata): Promise<void> {
        if (!this.config.enabled) {
            console.log('[AutoPublish] Auto-publish disabled');
            return;
        }

        const publishPromises: Promise<any>[] = [];

        // Publish to YouTube
        if (this.config.platforms.youtube) {
            publishPromises.push(
                this.publishToYouTube(recording).catch(err => {
                    this.onErrorCallback?.(recording, 'youtube', err);
                    throw err;
                })
            );
        }

        // Publish to Facebook
        if (this.config.platforms.facebook) {
            publishPromises.push(
                this.publishToFacebook(recording).catch(err => {
                    this.onErrorCallback?.(recording, 'facebook', err);
                    throw err;
                })
            );
        }

        // Wait for all publishes to complete
        try {
            const results = await Promise.allSettled(publishPromises);
            results.forEach((result, index) => {
                const platform = index === 0 && this.config.platforms.youtube ? 'youtube' : 'facebook';
                if (result.status === 'fulfilled') {
                    this.onPublishCallback?.(recording, platform, result.value);
                } else {
                    this.onErrorCallback?.(recording, platform, result.reason);
                }
            });
        } catch (err) {
            console.error('[AutoPublish] Failed to publish recording:', err);
        }
    }

    /**
     * Publish recording to YouTube
     */
    private async publishToYouTube(recording: RecordingMetadata): Promise<any> {
        try {
            const title = recording.title || this.config.defaultTitle || 'Live Recording';
            const description = recording.description || this.config.defaultDescription || '';
            const tags = recording.tags || this.config.defaultTags || [];

            // Add session info to description if available
            let fullDescription = description;
            if (recording.sessionName) {
                fullDescription += `\n\nSession: ${recording.sessionName}`;
            }
            if (recording.sessionId) {
                fullDescription += `\nSession ID: ${recording.sessionId}`;
            }
            fullDescription += `\nRecorded: ${recording.startTime.toLocaleString()}`;

            const result = await publishRecordingToYouTube(
                recording.filePath,
                title,
                fullDescription,
                tags
            );

            // If playlist ID is configured, add to playlist
            if (this.config.youtubePlaylistId && result.id) {
                // This would require the scheduleYouTubeVideoInPlaylist function
                // For now, just log
                console.log('[AutoPublish] Video published to YouTube:', result.id);
            }

            return result;
        } catch (err) {
            console.error('[AutoPublish] Failed to publish to YouTube:', err);
            throw err;
        }
    }

    /**
     * Publish recording to Facebook
     */
    private async publishToFacebook(recording: RecordingMetadata): Promise<any> {
        try {
            // Facebook doesn't have a direct video upload API like YouTube
            // In a real implementation, this would:
            // 1. Upload video file to Facebook Graph API
            // 2. Create a post with the video
            // 3. Or publish to a Facebook Page

            const title = recording.title || this.config.defaultTitle || 'Live Recording';
            const description = recording.description || this.config.defaultDescription || '';

            // For now, return a mock result
            // In production, you would use Facebook Graph API to upload video
            console.log('[AutoPublish] Would publish to Facebook:', {
                title,
                description,
                filePath: recording.filePath,
            });

            return {
                id: `fb_${Date.now()}`,
                success: true,
                message: 'Video would be published to Facebook (implementation needed)',
            };
        } catch (err) {
            console.error('[AutoPublish] Failed to publish to Facebook:', err);
            throw err;
        }
    }

    /**
     * Update configuration
     */
    updateConfig(config: Partial<AutoPublishConfig>): void {
        this.config = { ...this.config, ...config };
    }

    /**
     * Get current configuration
     */
    getConfig(): AutoPublishConfig {
        return { ...this.config };
    }

    /**
     * Set callback for successful publishes
     */
    onPublish(callback: (recording: RecordingMetadata, platform: 'youtube' | 'facebook', result: any) => void): void {
        this.onPublishCallback = callback;
    }

    /**
     * Set callback for publish errors
     */
    onError(callback: (recording: RecordingMetadata, platform: 'youtube' | 'facebook', error: Error) => void): void {
        this.onErrorCallback = callback;
    }
}

// Singleton instance
let autoPublishInstance: AutoPublishService | null = null;

export function getAutoPublishService(): AutoPublishService {
    if (!autoPublishInstance) {
        autoPublishInstance = new AutoPublishService();
    }
    return autoPublishInstance;
}
