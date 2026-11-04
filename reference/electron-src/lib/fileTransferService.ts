// ============================================
// File Transfer Service
// Handles drop-in files and automatic torrent transfer
// ============================================

import { SiteLocation } from './types';

export interface FileTransferStatus {
    fileId: string;
    fileName: string;
    targetSiteId: string;
    status: 'pending' | 'checking' | 'downloading' | 'uploading' | 'complete' | 'error' | 'paused';
    progress: number; // 0-100
    bytesTransferred: number;
    totalBytes: number;
    speed?: number; // bytes per second
    error?: string;
    resumeToken?: string; // For resuming transfers
}

class FileTransferService {
    private transfers: Map<string, FileTransferStatus> = new Map();
    private listeners: Set<(status: FileTransferStatus) => void> = new Set();

    async checkFileExists(siteId: string, filePath: string): Promise<boolean> {
        // Check if file exists at target site
        // This would query the tracker API or local file system
        try {
            const api = (window as any).electronAPI;
            if (api?.checkFileExists) {
                return await api.checkFileExists(siteId, filePath);
            }
            // Fallback: assume file doesn't exist if we can't check
            return false;
        } catch (e) {
            console.error('[FileTransfer] Failed to check file existence:', e);
            return false;
        }
    }

    async initiateTransfer(
        siteId: string, 
        filePath: string, 
        fileName: string,
        options?: {
            resumeToken?: string;
            totalBytes?: number;
        }
    ): Promise<string> {
        const fileId = options?.resumeToken || `${siteId}_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        
        const transfer: FileTransferStatus = {
            fileId,
            fileName,
            targetSiteId: siteId,
            status: 'checking',
            progress: 0,
            bytesTransferred: 0,
            totalBytes: options?.totalBytes || 0,
            resumeToken: options?.resumeToken,
        };

        this.transfers.set(fileId, transfer);
        this.notifyListeners(transfer);

        // Check if file exists
        const exists = await this.checkFileExists(siteId, filePath);
        
        if (exists) {
            transfer.status = 'complete';
            transfer.progress = 100;
            transfer.bytesTransferred = transfer.totalBytes;
            this.transfers.set(fileId, transfer);
            this.notifyListeners(transfer);
            return fileId;
        }

        // File doesn't exist, start transfer
        transfer.status = 'downloading';
        this.transfers.set(fileId, transfer);
        this.notifyListeners(transfer);

        try {
            const api = (window as any).electronAPI;
            if (api?.startFileTransfer) {
                // Use new unified file transfer API
                const result = await api.startFileTransfer({
                    siteId,
                    filePath,
                    fileName,
                    resumeToken: options?.resumeToken,
                });
                
                if (result.success) {
                    // Start progress tracking
                    this.trackProgress(fileId, result.transferId);
                } else {
                    throw new Error(result.error || 'Transfer failed');
                }
            } else if (api?.startTorrentTransfer) {
                await api.startTorrentTransfer(siteId, filePath);
                this.simulateProgress(fileId);
            } else {
                // Fallback: use tracker API
                await this.startTorrentViaTracker(siteId, filePath);
                this.simulateProgress(fileId);
            }
        } catch (e) {
            transfer.status = 'error';
            transfer.error = e instanceof Error ? e.message : 'Transfer failed';
            this.transfers.set(fileId, transfer);
            this.notifyListeners(transfer);
        }

        return fileId;
    }

    /**
     * Pause a transfer
     */
    pauseTransfer(fileId: string): void {
        const transfer = this.transfers.get(fileId);
        if (transfer && (transfer.status === 'downloading' || transfer.status === 'uploading')) {
            transfer.status = 'paused';
            this.transfers.set(fileId, transfer);
            this.notifyListeners(transfer);
            
            // Notify backend
            const api = (window as any).electronAPI;
            if (api?.pauseFileTransfer) {
                api.pauseFileTransfer(fileId).catch(console.error);
            }
        }
    }

    /**
     * Resume a paused transfer
     */
    async resumeTransfer(fileId: string): Promise<void> {
        const transfer = this.transfers.get(fileId);
        if (transfer && transfer.status === 'paused') {
            transfer.status = transfer.targetSiteId ? 'downloading' : 'uploading';
            this.transfers.set(fileId, transfer);
            this.notifyListeners(transfer);
            
            // Resume via backend
            const api = (window as any).electronAPI;
            if (api?.resumeFileTransfer) {
                await api.resumeFileTransfer(fileId);
            }
        }
    }

    /**
     * Track progress for a transfer
     */
    private trackProgress(fileId: string, transferId: string): void {
        const api = (window as any).electronAPI;
        if (!api?.getTransferProgress) return;

        const interval = setInterval(async () => {
            const transfer = this.transfers.get(fileId);
            if (!transfer || transfer.status === 'complete' || transfer.status === 'error') {
                clearInterval(interval);
                return;
            }

            try {
                const progress = await api.getTransferProgress(transferId);
                if (progress) {
                    transfer.progress = progress.progress || 0;
                    transfer.bytesTransferred = progress.bytesTransferred || 0;
                    transfer.totalBytes = progress.totalBytes || transfer.totalBytes;
                    transfer.speed = progress.speed;
                    
                    if (progress.status) {
                        transfer.status = progress.status;
                    }
                    
                    this.transfers.set(fileId, transfer);
                    this.notifyListeners(transfer);
                }
            } catch (e) {
                console.error('[FileTransfer] Failed to get progress:', e);
            }
        }, 500);
    }

    private async startTorrentViaTracker(siteId: string, filePath: string): Promise<void> {
        // Request torrent transfer via tracker API
        // This would call the tracker.php endpoint to initiate transfer
        try {
            const response = await fetch(`/api/v1/swarm/transfer`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    targetNodeId: siteId,
                    filePath,
                }),
            });
            
            if (!response.ok) {
                throw new Error(`Transfer initiation failed: ${response.statusText}`);
            }
        } catch (e) {
            console.error('[FileTransfer] Failed to start torrent transfer:', e);
            throw e;
        }
    }

    private simulateProgress(fileId: string): void {
        const transfer = this.transfers.get(fileId);
        if (!transfer || transfer.status !== 'downloading') return;

        let progress = 0;
        const interval = setInterval(() => {
            progress += Math.random() * 10;
            if (progress >= 100) {
                progress = 100;
                transfer.status = 'complete';
                clearInterval(interval);
            }
            transfer.progress = progress;
            this.transfers.set(fileId, transfer);
            this.notifyListeners(transfer);
        }, 500);
    }

    getTransferStatus(fileId: string): FileTransferStatus | undefined {
        return this.transfers.get(fileId);
    }

    getAllTransfers(): FileTransferStatus[] {
        return Array.from(this.transfers.values());
    }

    onTransferUpdate(callback: (status: FileTransferStatus) => void): () => void {
        this.listeners.add(callback);
        return () => this.listeners.delete(callback);
    }

    private notifyListeners(status: FileTransferStatus): void {
        this.listeners.forEach(callback => {
            try {
                callback(status);
            } catch (e) {
                console.error('[FileTransfer] Listener error:', e);
            }
        });
    }
}

export const fileTransferService = new FileTransferService();




