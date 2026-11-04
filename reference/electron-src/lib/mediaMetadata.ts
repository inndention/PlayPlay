// ============================================
// Media Metadata Extraction Service
// Comprehensive metadata extraction
// ============================================

import { MediaItem } from './types';

export interface MediaMetadata {
    duration: number;
    codec?: {
        video?: string;
        audio?: string;
    };
    resolution?: {
        width: number;
        height: number;
    };
    framerate?: number;
    bitrate?: number;
    audioChannels?: number;
    audioSampleRate?: number;
    fileSize?: number;
    format?: string;
    tags?: Record<string, string>;
}

/**
 * Extract comprehensive metadata from a media file
 * Uses Electron API if available, otherwise returns basic info
 */
export async function extractMediaMetadata(filePath: string): Promise<MediaMetadata | null> {
    try {
        // Try Electron API first
        const electronAPI = (window as any).electronAPI;
        if (electronAPI?.probeMedia) {
            const info = await electronAPI.probeMedia(filePath);
            
            if (!info || !info.format) {
                return null;
            }

            const format = info.format;
            const streams = info.streams || [];
            
            // Find video and audio streams
            const videoStream = streams.find((s: any) => s.codec_type === 'video');
            const audioStream = streams.find((s: any) => s.codec_type === 'audio');

            const metadata: MediaMetadata = {
                duration: parseFloat(format.duration || '0'),
                fileSize: parseInt(format.size || '0', 10),
                format: format.format_name,
                bitrate: parseInt(format.bit_rate || '0', 10),
                tags: format.tags,
            };

            if (videoStream) {
                metadata.codec = metadata.codec || {};
                metadata.codec.video = videoStream.codec_name;
                metadata.resolution = {
                    width: videoStream.width || 0,
                    height: videoStream.height || 0,
                };
                
                // Parse framerate
                if (videoStream.r_frame_rate) {
                    const [num, den] = videoStream.r_frame_rate.split('/').map(Number);
                    metadata.framerate = den ? num / den : num;
                }
            }

            if (audioStream) {
                metadata.codec = metadata.codec || {};
                metadata.codec.audio = audioStream.codec_name;
                metadata.audioChannels = audioStream.channels;
                metadata.audioSampleRate = audioStream.sample_rate;
            }

            return metadata;
        }

        // Fallback: try to get basic metadata from HTML5 video element
        return new Promise((resolve) => {
            const video = document.createElement('video');
            video.preload = 'metadata';
            
            video.onloadedmetadata = () => {
                resolve({
                    duration: video.duration || 0,
                    resolution: {
                        width: video.videoWidth || 0,
                        height: video.videoHeight || 0,
                    },
                });
                video.src = '';
            };
            
            video.onerror = () => {
                resolve(null);
                video.src = '';
            };
            
            // Try to load from file path
            if (filePath.startsWith('file://')) {
                video.src = filePath;
            } else if (filePath.match(/^[A-Za-z]:/)) {
                // Windows path
                video.src = `file:///${filePath.replace(/\\/g, '/')}`;
            } else {
                video.src = `file://${filePath}`;
            }
        });
    } catch (error) {
        console.error('[MediaMetadata] Failed to extract metadata:', error);
        return null;
    }
}

/**
 * Update a MediaItem with extracted metadata
 */
export async function enrichMediaItem(item: MediaItem): Promise<MediaItem> {
    if (!item.filePath && !item.src) {
        return item;
    }

    const filePath = item.filePath || item.src;
    const metadata = await extractMediaMetadata(filePath);

    if (!metadata) {
        return item;
    }

    return {
        ...item,
        duration: metadata.duration || item.duration,
        // Add metadata as notes if not set
        notes: item.notes || (metadata.resolution 
            ? `${metadata.resolution.width}x${metadata.resolution.height}` 
            : undefined),
    };
}

/**
 * Batch extract metadata for multiple items
 */
export async function batchExtractMetadata(
    filePaths: string[]
): Promise<Map<string, MediaMetadata | null>> {
    const results = new Map<string, MediaMetadata | null>();
    
    // Process in batches to avoid overwhelming the system
    const batchSize = 4;
    for (let i = 0; i < filePaths.length; i += batchSize) {
        const batch = filePaths.slice(i, i + batchSize);
        const batchResults = await Promise.all(
            batch.map(async (path) => ({
                path,
                metadata: await extractMediaMetadata(path),
            }))
        );
        
        for (const { path, metadata } of batchResults) {
            results.set(path, metadata);
        }
    }
    
    return results;
}
