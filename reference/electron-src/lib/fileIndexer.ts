// ============================================
// Hash-Based File Indexing Service
// Comprehensive file indexing with deduplication
// ============================================

import { FileIndexEntry } from './types';
import { loadFileIndex, saveFileIndex } from './storage';
import { extractMediaMetadata, MediaMetadata } from './mediaMetadata';

class FileIndexer {
    private index: Map<string, FileIndexEntry> = new Map();
    private hashCache: Map<string, string> = new Map(); // filePath -> hash cache

    async initialize(): Promise<void> {
        const stored = await loadFileIndex();
        Object.entries(stored).forEach(([hash, entry]) => {
            this.index.set(hash, entry);
            if (entry.filePath) {
                this.hashCache.set(entry.filePath, hash);
            }
        });
    }

    /**
     * Calculate SHA-256 hash of a file
     */
    async calculateFileHash(filePath: string): Promise<string> {
        // Check cache first
        const cached = this.hashCache.get(filePath);
        if (cached) {
            return cached;
        }

        try {
            const api = (window as any).electronAPI;
            if (api?.calculateFileHash) {
                const hash = await api.calculateFileHash(filePath);
                this.hashCache.set(filePath, hash);
                return hash;
            }
            
            // Fallback: use file path + size + modified time as hash
            // This is not cryptographically secure but works for deduplication
            const stats = await api?.getFileStats?.(filePath);
            if (stats) {
                const hash = `${filePath}_${stats.size}_${stats.mtime}`;
                this.hashCache.set(filePath, hash);
                return hash;
            }
            
            // Last resort: use file path
            return filePath;
        } catch (error) {
            console.error('[FileIndexer] Failed to calculate hash:', error);
            return filePath; // Fallback to file path
        }
    }

    /**
     * Index a file with comprehensive metadata
     */
    async indexFile(
        filePath: string, 
        options?: {
            hash?: string;
            size?: number;
            metadata?: MediaMetadata;
            siteId?: string;
        }
    ): Promise<string> {
        // Calculate hash if not provided
        const hash = options?.hash || await this.calculateFileHash(filePath);
        
        // Get file stats if size not provided
        let size = options?.size;
        if (!size) {
            try {
                const api = (window as any).electronAPI;
                if (api?.getFileStats) {
                    const stats = await api.getFileStats(filePath);
                    size = stats.size;
                }
            } catch (error) {
                console.error('[FileIndexer] Failed to get file stats:', error);
            }
        }

        // Extract metadata if not provided
        let metadata = options?.metadata;
        if (!metadata && filePath.match(/\.(mp4|avi|mov|mkv|webm|mp3|wav|flac)$/i)) {
            const extracted = await extractMediaMetadata(filePath);
            metadata = extracted || undefined;
        }

        const entry: FileIndexEntry = {
            hash,
            filePath,
            size: size || 0,
            modifiedTime: Date.now(),
            siteLocations: options?.siteId ? [options.siteId] : [],
            metadata: metadata ? {
                duration: metadata.duration,
                codec: metadata.codec?.video || metadata.codec?.audio,
                resolution: metadata.resolution ? `${metadata.resolution.width}x${metadata.resolution.height}` : undefined,
            } : undefined,
        };

        // Check if file with same hash already exists (deduplication)
        const existing = this.index.get(hash);
        if (existing) {
            // Update existing entry
            if (options?.siteId && !existing.siteLocations.includes(options.siteId)) {
                existing.siteLocations.push(options.siteId);
            }
            existing.filePath = filePath; // Update path (might be different location)
            existing.modifiedTime = Date.now();
            if (metadata) {
                existing.metadata = {
                    ...existing.metadata,
                    ...entry.metadata,
                };
            }
            this.index.set(hash, existing);
        } else {
            this.index.set(hash, entry);
        }

        this.hashCache.set(filePath, hash);
        await this.persist();
        
        return hash;
    }

    /**
     * Find duplicate files (same hash, different paths)
     */
    findDuplicates(): Map<string, FileIndexEntry[]> {
        const duplicates = new Map<string, FileIndexEntry[]>();
        const hashToEntries = new Map<string, FileIndexEntry[]>();

        // Group entries by hash
        this.index.forEach((entry) => {
            if (!hashToEntries.has(entry.hash)) {
                hashToEntries.set(entry.hash, []);
            }
            hashToEntries.get(entry.hash)!.push(entry);
        });

        // Find hashes with multiple entries
        hashToEntries.forEach((entries, hash) => {
            if (entries.length > 1) {
                duplicates.set(hash, entries);
            }
        });

        return duplicates;
    }

    /**
     * Get all files at a specific site
     */
    getFilesBySite(siteId: string): FileIndexEntry[] {
        const result: FileIndexEntry[] = [];
        this.index.forEach((entry) => {
            if (entry.siteLocations.includes(siteId)) {
                result.push(entry);
            }
        });
        return result;
    }

    getByHash(hash: string): FileIndexEntry | undefined {
        return this.index.get(hash);
    }

    getByFilePath(filePath: string): FileIndexEntry | undefined {
        const hash = this.hashCache.get(filePath);
        if (hash) {
            return this.index.get(hash);
        }
        return undefined;
    }

    async addSiteLocation(hash: string, siteId: string): Promise<void> {
        const entry = this.index.get(hash);
        if (entry) {
            if (!entry.siteLocations.includes(siteId)) {
                entry.siteLocations.push(siteId);
                this.index.set(hash, entry);
                await this.persist();
            }
        }
    }

    /**
     * Remove a file from the index
     */
    async removeFile(filePath: string): Promise<void> {
        const hash = this.hashCache.get(filePath);
        if (hash) {
            this.index.delete(hash);
            this.hashCache.delete(filePath);
            await this.persist();
        }
    }

    /**
     * Reindex all files (useful after metadata extraction improvements)
     */
    async reindexAll(onProgress?: (completed: number, total: number) => void): Promise<void> {
        const entries = Array.from(this.index.values());
        let completed = 0;

        for (const entry of entries) {
            if (entry.filePath) {
                try {
                    // Re-extract metadata
                    const metadata = await extractMediaMetadata(entry.filePath);
                    if (metadata) {
                        entry.metadata = {
                            duration: metadata.duration,
                            codec: metadata.codec?.video || metadata.codec?.audio,
                            resolution: metadata.resolution ? `${metadata.resolution.width}x${metadata.resolution.height}` : undefined,
                        };
                        this.index.set(entry.hash, entry);
                    }
                } catch (error) {
                    console.error(`[FileIndexer] Failed to reindex ${entry.filePath}:`, error);
                }
            }
            
            completed++;
            onProgress?.(completed, entries.length);
        }

        await this.persist();
    }

    private async persist(): Promise<void> {
        const indexObj: Record<string, FileIndexEntry> = {};
        this.index.forEach((entry, hash) => {
            indexObj[hash] = entry;
        });
        await saveFileIndex(indexObj);
    }
}

export const fileIndexer = new FileIndexer();




