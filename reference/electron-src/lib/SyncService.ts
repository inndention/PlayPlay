import WebTorrent from 'webtorrent';
import fs from 'fs-extra';
import path from 'path';
import { EventEmitter } from 'events';

export interface SyncStatus {
    downloadSpeed: number;
    uploadSpeed: number;
    progress: number;
    ratio: number;
    numPeers: number;
    files: {
        name: string;
        progress: number;
        downloaded: number;
        length: number;
        numPeers: number;
        state: 'seeding' | 'downloading' | 'paused' | 'done';
    }[];
}

export class SyncService extends EventEmitter {
    private client: WebTorrent.Instance;
    private seedDir: string;
    private trackerUrls: string[];

    constructor(seedDir: string, trackerUrls: string[] = []) {
        super();
        this.seedDir = seedDir;
        this.trackerUrls = trackerUrls;

        // Initialize WebTorrent client
        this.client = new WebTorrent();

        this.client.on('error', (err) => {
            console.error('[SyncService] Client Error:', err);
            this.emit('error', err);
        });

        // Monitor overall progress
        setInterval(() => {
            this.emit('status', this.getStatus());
        }, 1000);
    }

    public getStatus(): SyncStatus {
        return {
            downloadSpeed: this.client.downloadSpeed,
            uploadSpeed: this.client.uploadSpeed,
            progress: this.client.progress,
            ratio: this.client.ratio,
            numPeers: this.client.torrents.reduce((acc, t) => acc + t.numPeers, 0),
            files: this.client.torrents.map(t => ({
                name: t.name,
                progress: t.progress,
                downloaded: t.downloaded,
                length: t.length,
                numPeers: t.numPeers,
                state: t.done ? 'seeding' : 'downloading' // Simplified state
            }))
        };
    }

    public async seedFile(filePath: string): Promise<string> {
        return new Promise((resolve, reject) => {
            // Check if already seeding
            const existing = this.client.torrents.find(t => t.path === path.dirname(filePath) && t.name === path.basename(filePath));
            if (existing) {
                resolve(existing.magnetURI);
                return;
            }

            this.client.seed(filePath, {
                path: path.dirname(filePath),
                announce: this.trackerUrls
            }, (torrent) => {
                console.log('[SyncService] Seeding:', torrent.name);
                console.log('[SyncService] Magnet:', torrent.magnetURI);
                resolve(torrent.magnetURI);
            });
        });
    }

    public async addMagnet(magnetURI: string): Promise<void> {
        return new Promise((resolve, reject) => {
            // Check if already added
            const existing = this.client.get(magnetURI);
            if (existing !== null && existing !== undefined) {
                resolve();
                return;
            }

            this.client.add(magnetURI, {
                path: this.seedDir, // Download to the seed directory
                announce: this.trackerUrls
            }, (torrent) => {
                console.log('[SyncService] Downloading:', torrent.name);

                torrent.on('done', () => {
                    console.log('[SyncService] Finished:', torrent.name);
                    this.emit('file-done', torrent.name);
                });

                resolve();
            });
        });
    }

    public removeTorrent(magnetURI: string) {
        this.client.remove(magnetURI);
    }

    public destroy() {
        this.client.destroy();
    }
}
