// ============================================
// Tracker API Client
// Communicates with remote PHP endpoint: https://getstarnews.com/pp/tracker.php
// ============================================

import { SiteState } from './types';
import { loadConnectionSettings } from './storage';

interface TrackerConfig {
    url: string;
    apiKey: string;
    enabled: boolean;
}

class TrackerApi {
    private config: TrackerConfig = {
        url: 'https://getstarnews.com/pp/tracker.php',
        apiKey: '',
        enabled: false,
    };
    private isConnected: boolean = false;
    private connectionStateListeners: Set<(connected: boolean) => void> = new Set();
    private healthCheckInterval: NodeJS.Timeout | null = null;

    constructor() {
        this.loadConfig();
        this.loadConnectionState();
        this.startHealthCheck();
    }

    private loadConfig() {
        const settings = loadConnectionSettings();
        if (settings?.phpApi) {
            this.config = {
                url: settings.phpApi.url || this.config.url,
                apiKey: settings.phpApi.apiKey || '',
                enabled: settings.phpApi.enabled || false,
            };
        }
    }

    private loadConnectionState() {
        try {
            const stored = localStorage.getItem('tracker_connection_state');
            if (stored) {
                const state = JSON.parse(stored);
                this.isConnected = state.connected || false;
            }
        } catch (e) {
            console.error('[TrackerAPI] Failed to load connection state:', e);
        }
    }

    private saveConnectionState() {
        try {
            localStorage.setItem('tracker_connection_state', JSON.stringify({
                connected: this.isConnected,
                timestamp: Date.now(),
            }));
        } catch (e) {
            console.error('[TrackerAPI] Failed to save connection state:', e);
        }
    }

    private notifyConnectionStateChange() {
        this.connectionStateListeners.forEach(listener => listener(this.isConnected));
    }

    onConnectionStateChange(listener: (connected: boolean) => void): () => void {
        this.connectionStateListeners.add(listener);
        return () => {
            this.connectionStateListeners.delete(listener);
        };
    }

    private startHealthCheck() {
        if (this.healthCheckInterval) {
            clearInterval(this.healthCheckInterval);
        }
        
        this.healthCheckInterval = setInterval(async () => {
            if (this.config.enabled && this.isConnected) {
                await this.checkHealth();
            }
        }, 10000) as any; // Check every 10 seconds
    }

    private async checkHealth(): Promise<boolean> {
        try {
            const result = await this.request('ping');
            if (result && (result.status === 'ok' || result.message)) {
                if (!this.isConnected) {
                    this.isConnected = true;
                    this.saveConnectionState();
                    this.notifyConnectionStateChange();
                }
                return true;
            }
        } catch (e) {
            if (this.isConnected) {
                this.isConnected = false;
                this.saveConnectionState();
                this.notifyConnectionStateChange();
            }
            return false;
        }
        return false;
    }

    async testConnection(): Promise<{ success: boolean; message: string }> {
        this.loadConfig();
        if (!this.config.enabled || !this.config.url) {
            return { success: false, message: 'Tracker API is not enabled' };
        }

        try {
            const headers: Record<string, string> = { 'Content-Type': 'application/json' };
            if (this.config.apiKey) {
                headers['X-API-Key'] = this.config.apiKey;
            }
            const res = await fetch(`${this.config.url}?action=ping`, {
                method: 'GET',
                headers,
            });
            
            if (res.ok) {
                const data = await res.json().catch(() => ({}));
                // After successful test, establish connection
                this.isConnected = true;
                this.saveConnectionState();
                this.notifyConnectionStateChange();
                return { success: true, message: data.message || 'PHP API OK' };
            } else {
                this.isConnected = false;
                this.saveConnectionState();
                this.notifyConnectionStateChange();
                return { success: false, message: `HTTP ${res.status}` };
            }
        } catch (e: any) {
            this.isConnected = false;
            this.saveConnectionState();
            this.notifyConnectionStateChange();
            return { success: false, message: e.message || 'Connection failed' };
        }
    }

    getConnectionState(): boolean {
        return this.isConnected;
    }

    async connect(): Promise<boolean> {
        const testResult = await this.testConnection();
        return testResult.success;
    }

    disconnect() {
        this.isConnected = false;
        this.saveConnectionState();
        this.notifyConnectionStateChange();
    }

    isEnabled(): boolean {
        this.loadConfig(); // Reload in case settings changed
        return this.config.enabled && !!this.config.url;
    }

    private async request(action: string, data?: any): Promise<any> {
        if (!this.isEnabled()) {
            throw new Error('Tracker API is not enabled');
        }

        const url = new URL(this.config.url);
        url.searchParams.set('action', action);

        const headers: Record<string, string> = {
            'Content-Type': 'application/json',
        };

        if (this.config.apiKey) {
            headers['X-API-Key'] = this.config.apiKey;
        }

        try {
            const response = await fetch(url.toString(), {
                method: data ? 'POST' : 'GET',
                headers,
                body: data ? JSON.stringify(data) : undefined,
            });

            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }

            return await response.json();
        } catch (err) {
            console.error(`[TrackerAPI] ${action} failed:`, err);
            throw err;
        }
    }

    async getSiteStates(): Promise<Record<string, SiteState> | null> {
        try {
            const result = await this.request('get_site_states');
            return result.states || null;
        } catch (err) {
            return null;
        }
    }

    async updateSiteState(locationId: string, state: SiteState): Promise<boolean> {
        try {
            await this.request('update_site_state', {
                location_id: locationId,
                state,
            });
            return true;
        } catch (err) {
            return false;
        }
    }

    async broadcastState(state: any): Promise<boolean> {
        try {
            await this.request('broadcast_state', state);
            return true;
        } catch (err) {
            return false;
        }
    }

    async getSwarmState(): Promise<any> {
        try {
            return await this.request('get_swarm_state');
        } catch (err) {
            return null;
        }
    }

    async registerInstance(hostname: string, instanceInfo: any): Promise<boolean> {
        try {
            await this.request('register_instance', {
                hostname,
                ...instanceInfo,
            });
            return true;
        } catch (err) {
            console.error('[TrackerAPI] Failed to register instance:', err);
            return false;
        }
    }

    async getRegisteredInstances(): Promise<any[]> {
        try {
            const result = await this.request('get_instances');
            return result.instances || [];
        } catch (err) {
            return [];
        }
    }

    async updateInstanceMapping(hostname: string, siteId: string, playerId?: string): Promise<boolean> {
        try {
            await this.request('update_instance_mapping', {
                hostname,
                siteId,
                playerId,
            });
            return true;
        } catch (err) {
            return false;
        }
    }

    async getAsPlayedReports(clientId: string | null, startDate: string, endDate: string): Promise<any> {
        return this.request('get_as_played_reports', {
            clientId,
            startDate,
            endDate,
        });
    }
}

export const trackerApi = new TrackerApi();

