import { apiClient } from './apiClient';

interface PlayoutLog {
    id?: number;
    node_id: string;
    hostname: string;
    filename: string;
    duration: number;
    started_at: string;
    ended_at: string;
    synced: boolean;
}

class AnalyticsService {
    private logs: PlayoutLog[] = [];
    private syncInterval: NodeJS.Timeout | null = null;
    private readonly SYNC_INTERVAL_MS = 60 * 60 * 1000; // 1 hour
    private readonly STORAGE_KEY = 'playplay_analytics_logs';

    constructor() {
        this.loadLogs();
        this.startSync();
    }

    private loadLogs() {
        try {
            const stored = localStorage.getItem(this.STORAGE_KEY);
            if (stored) {
                this.logs = JSON.parse(stored);
            }
        } catch (e) {
            console.error('Failed to load analytics logs', e);
        }
    }

    private saveLogs() {
        try {
            localStorage.setItem(this.STORAGE_KEY, JSON.stringify(this.logs));
        } catch (e) {
            console.error('Failed to save analytics logs', e);
        }
    }

    public logPlayout(filename: string, duration: number, startedAt: Date, endedAt: Date) {
        const log: PlayoutLog = {
            node_id: 'LOCAL_NODE', // Should be replaced with actual ID
            hostname: window.location.hostname || 'localhost',
            filename,
            duration,
            started_at: startedAt.toISOString(),
            ended_at: endedAt.toISOString(),
            synced: false
        };
        this.logs.push(log);
        this.saveLogs();
    }

    public async syncLogs() {
        const unsynced = this.logs.filter(l => !l.synced);
        if (unsynced.length === 0) return;

        try {
            // In a real app, we'd batch this properly.
            // For now, we send all unsynced logs.
            // We need to update apiClient to support this, or fetch directly.
            // Let's assume we add logActivity to apiClient.

            // For now, using fetch directly to demonstrate logic, but ideally use apiClient
            // const res = await apiClient.logActivity(unsynced);

            // Placeholder for API call
            console.log('Syncing logs to tracker...', unsynced);

            // Simulate success
            const success = true;

            if (success) {
                this.logs = this.logs.map(l => ({ ...l, synced: true }));
                // Optional: Prune old synced logs
                this.logs = this.logs.filter(l => !l.synced || new Date(l.ended_at).getTime() > Date.now() - 24 * 60 * 60 * 1000);
                this.saveLogs();
            }
        } catch (e) {
            console.error('Failed to sync logs', e);
        }
    }

    private startSync() {
        if (this.syncInterval) clearInterval(this.syncInterval);
        this.syncInterval = setInterval(() => this.syncLogs(), this.SYNC_INTERVAL_MS);
    }

    public stopSync() {
        if (this.syncInterval) clearInterval(this.syncInterval);
    }
}

export const analyticsService = new AnalyticsService();
