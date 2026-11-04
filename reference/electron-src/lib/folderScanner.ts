// ============================================
// Folder Scanner
// Scans folders for media files and sorts them
// ============================================

import { MediaItem, FolderSortOrder } from './types';

const isElectron = !!(window as any).electronAPI;

const MEDIA_EXTENSIONS = ['.mp4', '.mov', '.avi', '.mkv', '.m4v', '.mp3', '.wav', '.aac', '.m4a', '.jpg', '.jpeg', '.png', '.gif'];

export interface ScannedFile {
    path: string;
    name: string;
    size: number;
    modified: number;
    extension: string;
}

export async function scanFolder(folderPath: string): Promise<ScannedFile[]> {
    if (!isElectron) {
        console.warn('[FolderScanner] Not in Electron, cannot scan folders');
        return [];
    }

    try {
        const files = await (window as any).electronAPI?.scanFolder?.(folderPath);
        if (!files || !Array.isArray(files)) {
            return [];
        }

        // Filter to media files only
        return files.filter((file: any) => {
            const ext = file.extension?.toLowerCase() || '';
            return MEDIA_EXTENSIONS.includes(ext);
        });
    } catch (error) {
        console.error('[FolderScanner] Error scanning folder:', error);
        return [];
    }
}

export function sortFiles(files: ScannedFile[], order: FolderSortOrder): ScannedFile[] {
    const sorted = [...files];
    
    switch (order) {
        case 'name':
            sorted.sort((a, b) => a.name.localeCompare(b.name));
            break;
        case 'date':
            sorted.sort((a, b) => b.modified - a.modified); // Newest first
            break;
        case 'size':
            sorted.sort((a, b) => b.size - a.size); // Largest first
            break;
        case 'random':
            // Fisher-Yates shuffle
            for (let i = sorted.length - 1; i > 0; i--) {
                const j = Math.floor(Math.random() * (i + 1));
                [sorted[i], sorted[j]] = [sorted[j], sorted[i]];
            }
            break;
    }
    
    return sorted;
}

export async function convertToPlaylistItems(
    files: ScannedFile[],
    folderPath: string
): Promise<MediaItem[]> {
    const items: MediaItem[] = [];
    
    for (const file of files) {
        try {
            // Get file info (duration, etc.) via Electron
            const fileInfo = await (window as any).electronAPI?.getFileInfo?.(file.path);
            
            const mediaItem: MediaItem = {
                id: `folder_${file.path.replace(/[^a-zA-Z0-9]/g, '_')}`,
                title: file.name,
                duration: fileInfo?.duration || 0,
                src: file.path,
                type: file.extension.match(/\.(jpg|jpeg|png|gif)$/i) ? 'image' : 'video',
                filePath: file.path,
                filename: file.name,
            };
            
            items.push(mediaItem);
        } catch (error) {
            console.error(`[FolderScanner] Error processing file ${file.path}:`, error);
        }
    }
    
    return items;
}

