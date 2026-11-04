// ============================================
// File Sharing Portal
// Guest file access portal logic
// ============================================

import { MediaItem } from './types';
import { dbFetchMedia } from './mediaDbClient';

export interface GuestAccessConfig {
    enabled: boolean;
    publicFolders: string[];
    allowUpload: boolean;
    allowDownload: boolean;
    maxUploadSize: number; // bytes
    requireAuth: boolean;
    authToken?: string;
}

const DEFAULT_CONFIG: GuestAccessConfig = {
    enabled: true,
    publicFolders: ['/public', '/shared'],
    allowUpload: true,
    allowDownload: true,
    maxUploadSize: 100 * 1024 * 1024, // 100MB
    requireAuth: false,
};

class FileSharingPortal {
    private config: GuestAccessConfig = DEFAULT_CONFIG;

    /**
     * Check if a file is accessible to guests
     */
    isFilePublic(filePath: string): boolean {
        if (!this.config.enabled) return false;
        
        // Check if file is in a public folder
        return this.config.publicFolders.some(folder => 
            filePath.startsWith(folder) || filePath.includes(folder)
        );
    }

    /**
     * Get public media items
     */
    async getPublicMedia(): Promise<MediaItem[]> {
        if (!this.config.enabled) return [];
        
        try {
            const allMedia = await dbFetchMedia();
            return allMedia.filter(item => {
                if (!item.filePath) return false;
                return this.isFilePublic(item.filePath);
            });
        } catch (error) {
            console.error('[FileSharingPortal] Failed to get public media:', error);
            return [];
        }
    }

    /**
     * Check if upload is allowed
     */
    canUpload(fileSize: number): { allowed: boolean; error?: string } {
        if (!this.config.enabled) {
            return { allowed: false, error: 'File sharing portal is disabled' };
        }
        
        if (!this.config.allowUpload) {
            return { allowed: false, error: 'Uploads are not allowed' };
        }
        
        if (fileSize > this.config.maxUploadSize) {
            return { allowed: false, error: `File size exceeds maximum (${this.config.maxUploadSize / 1024 / 1024}MB)` };
        }
        
        return { allowed: true };
    }

    /**
     * Check if download is allowed
     */
    canDownload(): boolean {
        return this.config.enabled && this.config.allowDownload;
    }

    /**
     * Validate guest access token
     */
    validateToken(token?: string): boolean {
        if (!this.config.requireAuth) return true;
        return token === this.config.authToken;
    }

    /**
     * Update configuration
     */
    updateConfig(config: Partial<GuestAccessConfig>): void {
        this.config = { ...this.config, ...config };
    }

    /**
     * Get current configuration
     */
    getConfig(): GuestAccessConfig {
        return { ...this.config };
    }
}

export const fileSharingPortal = new FileSharingPortal();



