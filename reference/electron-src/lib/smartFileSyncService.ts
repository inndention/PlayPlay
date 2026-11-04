/**
 * Smart File Sync Service
 * Intelligently resolves media sources with fallback to livestream
 * Supports delay compensation for synced playback
 */

import { MediaItem } from './types';
import { getWebRTCSessionManager, WebRTCSession } from './webrtcSessionManager';

export type MediaSourceType = 'local' | 'p2p-stream' | 'live-stream' | 'remote-file';

export interface MediaSource {
    type: MediaSourceType;
    path?: string; // For local files
    peerId?: string; // For P2P streams
    sessionId?: string; // For live streams
    mediaId: string;
    delay?: number; // Delay compensation in milliseconds
    estimatedLatency?: number; // Estimated network latency
}

export interface FileAvailability {
    mediaId: string;
    availableLocally: boolean;
    availableOnPeers: string[]; // Peer IDs that have the file
    estimatedDownloadTime?: number; // seconds
    fileSize?: number; // bytes
}

export class SmartFileSyncService {
    private localMediaCache: Map<string, MediaItem> = new Map();
    private peerFileIndex: Map<string, Set<string>> = new Map(); // peerId -> Set<mediaId>
    private downloadQueue: Map<string, Promise<MediaSource>> = new Map();
    private delayCompensation: Map<string, number> = new Map(); // mediaId -> delay in ms

    /**
     * Register local media file
     */
    registerLocalMedia(media: MediaItem): void {
        this.localMediaCache.set(media.id, media);
    }

    /**
     * Register peer file availability
     */
    registerPeerFiles(peerId: string, mediaIds: string[]): void {
        this.peerFileIndex.set(peerId, new Set(mediaIds));
    }

    /**
     * Resolve media source with smart fallback
     */
    async resolveMediaSource(
        mediaId: string,
        sessionId?: string,
        preferLocal: boolean = true
    ): Promise<MediaSource> {
        // Check if already downloading
        const existingDownload = this.downloadQueue.get(mediaId);
        if (existingDownload) {
            return existingDownload;
        }

        // 1. Check local availability
        if (preferLocal) {
            const localMedia = this.localMediaCache.get(mediaId);
            if (localMedia && localMedia.filePath) {
                return {
                    type: 'local',
                    path: localMedia.filePath,
                    mediaId,
                };
            }
        }

        // 2. Check peer nodes
        const peerWithFile = this.findPeerWithFile(mediaId);
        if (peerWithFile) {
            const downloadPromise = this.initiateP2PDownload(mediaId, peerWithFile);
            this.downloadQueue.set(mediaId, downloadPromise);
            
            // Clean up from queue when done
            downloadPromise.finally(() => {
                this.downloadQueue.delete(mediaId);
            });

            return downloadPromise;
        }

        // 3. Fallback to live stream
        if (sessionId) {
            const session = getWebRTCSessionManager().getSession(sessionId);
            if (session && session.remoteStream) {
                const delay = this.calculateDelayCompensation(mediaId, sessionId);
                return {
                    type: 'live-stream',
                    sessionId,
                    mediaId,
                    delay,
                    estimatedLatency: this.estimateLatency(sessionId),
                };
            }
        }

        // 4. If no session, try to find any active session
        const allSessions = getWebRTCSessionManager().getAllSessions();
        const activeSession = allSessions.find(s => s.status === 'connected' && s.remoteStream);
        if (activeSession) {
            const delay = this.calculateDelayCompensation(mediaId, activeSession.id);
            return {
                type: 'live-stream',
                sessionId: activeSession.id,
                mediaId,
                delay,
                estimatedLatency: this.estimateLatency(activeSession.id),
            };
        }

        throw new Error(`Media ${mediaId} not available locally, on peers, or via live stream`);
    }

    /**
     * Find peer that has the file
     */
    private findPeerWithFile(mediaId: string): string | null {
        for (const [peerId, mediaSet] of this.peerFileIndex.entries()) {
            if (mediaSet.has(mediaId)) {
                return peerId;
            }
        }
        return null;
    }

    /**
     * Initiate P2P download
     */
    private async initiateP2PDownload(mediaId: string, peerId: string): Promise<MediaSource> {
        // In a real implementation, this would:
        // 1. Establish WebRTC data channel with peer
        // 2. Request file transfer
        // 3. Stream file chunks
        // 4. Save to local cache
        // 5. Return local path

        // For now, return P2P stream source
        return {
            type: 'p2p-stream',
            peerId,
            mediaId,
            estimatedLatency: 100, // Mock latency
        };
    }

    /**
     * Calculate delay compensation for synced playback
     */
    private calculateDelayCompensation(mediaId: string, sessionId: string): number {
        // Check if we have a cached delay value
        const cached = this.delayCompensation.get(`${mediaId}_${sessionId}`);
        if (cached !== undefined) {
            return cached;
        }

        // Estimate delay based on:
        // - Network latency
        // - Processing time
        // - Buffer time
        const session = getWebRTCSessionManager().getSession(sessionId);
        if (!session) return 0;

        const latency = this.estimateLatency(sessionId);
        const processingDelay = 200; // Estimated processing delay
        const bufferDelay = 500; // Safety buffer

        const totalDelay = latency + processingDelay + bufferDelay;
        this.delayCompensation.set(`${mediaId}_${sessionId}`, totalDelay);
        return totalDelay;
    }

    /**
     * Estimate network latency for a session
     */
    private estimateLatency(sessionId: string): number {
        const session = getWebRTCSessionManager().getSession(sessionId);
        if (!session) return 0;

        // In a real implementation, this would measure RTT
        // For now, return a reasonable estimate based on connection state
        if (session.status === 'connected') {
            return 100; // 100ms estimated latency
        }
        return 500; // Higher latency if not fully connected
    }

    /**
     * Check file availability across all sources
     */
    async checkFileAvailability(mediaId: string): Promise<FileAvailability> {
        const availableLocally = this.localMediaCache.has(mediaId);
        const availableOnPeers: string[] = [];

        for (const [peerId, mediaSet] of this.peerFileIndex.entries()) {
            if (mediaSet.has(mediaId)) {
                availableOnPeers.push(peerId);
            }
        }

        // Estimate download time (mock calculation)
        const estimatedDownloadTime = availableOnPeers.length > 0
            ? 30 // 30 seconds estimate
            : undefined;

        return {
            mediaId,
            availableLocally,
            availableOnPeers,
            estimatedDownloadTime,
        };
    }

    /**
     * Download file for segment editing/scrubbing
     */
    async downloadFileForEditing(mediaId: string, peerId?: string): Promise<string> {
        // If peerId specified, download from that peer
        // Otherwise, find any available peer
        const targetPeer = peerId || this.findPeerWithFile(mediaId);
        if (!targetPeer) {
            throw new Error(`File ${mediaId} not available on any peer`);
        }

        // Initiate download
        const source = await this.initiateP2PDownload(mediaId, targetPeer);
        
        // Wait for download to complete and return local path
        // In real implementation, this would wait for file to be saved
        if (source.type === 'local' && source.path) {
            return source.path;
        }

        // If still P2P, we'd need to wait for transfer completion
        // For now, throw error indicating download in progress
        throw new Error(`Download in progress for ${mediaId}`);
    }

    /**
     * Get delay compensation for a media item
     */
    getDelayCompensation(mediaId: string, sessionId: string): number {
        return this.calculateDelayCompensation(mediaId, sessionId);
    }

    /**
     * Clear delay compensation cache
     */
    clearDelayCache(): void {
        this.delayCompensation.clear();
    }

    /**
     * Update delay compensation based on actual playback sync
     */
    updateDelayCompensation(mediaId: string, sessionId: string, actualDelay: number): void {
        this.delayCompensation.set(`${mediaId}_${sessionId}`, actualDelay);
    }
}

// Singleton instance
let fileSyncInstance: SmartFileSyncService | null = null;

export function getSmartFileSyncService(): SmartFileSyncService {
    if (!fileSyncInstance) {
        fileSyncInstance = new SmartFileSyncService();
    }
    return fileSyncInstance;
}
