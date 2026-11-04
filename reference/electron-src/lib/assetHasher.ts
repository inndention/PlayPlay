// ============================================
// Asset Hasher
// Generates SHA256 hashes for media files
// ============================================

import { MediaItem } from './types';

const isElectron = !!(window as any).electronAPI;

export interface AssetHash {
    hash: string;
    mediaId: string;
    metadata: {
        title: string;
        duration: number;
        type: string;
        filePath?: string;
        filename?: string;
    };
}

// Generate hash for a file (Electron only)
export async function generateFileHash(filePath: string): Promise<string> {
    if (!isElectron) {
        console.warn('[AssetHasher] Not in Electron, cannot generate hash');
        return '';
    }

    try {
        const hash = await (window as any).electronAPI?.generateFileHash?.(filePath);
        return hash || '';
    } catch (error) {
        console.error('[AssetHasher] Error generating hash:', error);
        return '';
    }
}

// Generate hash for a media item
export async function hashMediaItem(item: MediaItem): Promise<string> {
    if (item.filePath) {
        return generateFileHash(item.filePath);
    }
    
    // Fallback: generate hash from item properties
    const data = `${item.id}-${item.title}-${item.duration}-${item.src}`;
    return hashString(data);
}

// Hash a string (SHA256)
async function hashString(data: string): Promise<string> {
    if (typeof crypto !== 'undefined' && crypto.subtle) {
        const encoder = new TextEncoder();
        const dataBuffer = encoder.encode(data);
        const hashBuffer = await crypto.subtle.digest('SHA-256', dataBuffer);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
    
    // Fallback for environments without crypto.subtle
    return btoa(data).replace(/[^a-zA-Z0-9]/g, '').substring(0, 64);
}

// Create asset hash entry
export async function createAssetHash(item: MediaItem): Promise<AssetHash> {
    const hash = await hashMediaItem(item);
    
    return {
        hash,
        mediaId: item.id,
        metadata: {
            title: item.title,
            duration: item.duration,
            type: item.type,
            filePath: item.filePath,
            filename: item.filename,
        },
    };
}

// Report asset to sales endpoint
export async function reportAsset(hash: AssetHash, endpointUrl: string): Promise<boolean> {
    try {
        const response = await fetch(`${endpointUrl}/api/assets`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify({
                hash: hash.hash,
                metadata: hash.metadata,
            }),
        });
        
        return response.ok;
    } catch (error) {
        console.error('[AssetHasher] Error reporting asset:', error);
        return false;
    }
}

