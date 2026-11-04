// ============================================
// Unified Settings Manager
// Single source of truth for all application settings
// ============================================

import {
    AutomationSettings,
    ExtendedSettings,
    OBSSession,
} from './types';
import { DEFAULT_SETTINGS } from './storage';
import { DEFAULT_ENGINE_APPROACHES, mergeEngineApproaches, type EngineApproachesMergeInput } from './engineApproaches';

// Connection settings interface
export interface ConnectionSettings {
    obs: { host: string; port: number; password: string };
    caspercg: { host: string; port: number };
    ffplayout: { apiUrl: string; username: string; password: string };
    phpApi: { url: string; apiKey: string; enabled: boolean };
}

// Engine paths interface
export interface EnginePaths {
    ffmpeg?: string;
    ffprobe?: string;
    caspercg?: string;
    gstreamer?: string;
    [key: string]: string | undefined;
}

// Complete application settings
export interface AppSettings extends ExtendedSettings {
    connections: ConnectionSettings;
    enginePaths: EnginePaths;
    hostname?: string;
    instanceMapping?: { siteId?: string; playerId?: string };
    devNotes?: string;
}

// Default connection settings
const DEFAULT_CONNECTIONS: ConnectionSettings = {
    obs: { host: '127.0.0.1', port: 4455, password: '' },
    caspercg: { host: '127.0.0.1', port: 5250 },
    ffplayout: { apiUrl: 'http://127.0.0.1:8787', username: 'admin', password: 'admin' },
    phpApi: { url: 'http://yoursite.com/tracker.php', apiKey: '', enabled: false },
};

// Storage keys
const STORAGE_KEYS = {
    SETTINGS: 'playplay_settings',
    CONNECTIONS: 'connection_settings',
    ENGINE_PATHS: 'engine_paths',
    HOSTNAME: 'hostname',
    INSTANCE_MAPPING: 'instance_mapping',
    DEV_NOTES: 'dev_notes',
} as const;

class SettingsManager {
    private settings: AppSettings | null = null;
    private listeners: Set<(settings: AppSettings) => void> = new Set();

    /**
     * Load all settings from storage
     */
    loadAll(): AppSettings {
        // Load automation settings
        const automationSettings = this.loadAutomationSettings();
        
        // Load connection settings
        const connections = this.loadConnections();
        
        // Load engine paths
        const enginePaths = this.loadEnginePaths();
        
        // Load other settings
        const hostname = this.loadHostname();
        const instanceMapping = this.loadInstanceMapping();
        const devNotes = this.loadDevNotes();

        const settings: AppSettings = {
            ...automationSettings,
            connections,
            enginePaths,
            hostname,
            instanceMapping,
            devNotes,
        };

        this.settings = settings;
        return settings;
    }

    /**
     * Get current settings (load if not cached)
     */
    getSettings(): AppSettings {
        if (!this.settings) {
            return this.loadAll();
        }
        return this.settings;
    }

    /**
     * Save all settings
     */
    saveAll(settings: Partial<AppSettings>): void {
        const current = this.getSettings();
        const updated = { ...current, ...settings };
        this.settings = updated as AppSettings;

        // Save automation settings
        if (settings.alwaysPlayLatestEpisode !== undefined || 
            settings.shuffleAdsInHour !== undefined ||
            settings.fillGapsWithIds !== undefined ||
            settings.obsSessions !== undefined ||
            settings.ddrAutoPreload !== undefined ||
            settings.ddrDefaultVolume !== undefined ||
            settings.engineApproaches !== undefined) {
            this.saveAutomationSettings(updated);
        }

        // Save connection settings
        if (settings.connections) {
            this.saveConnections(settings.connections);
        }

        // Save engine paths
        if (settings.enginePaths) {
            this.saveEnginePaths(settings.enginePaths);
        }

        // Save other settings
        if (settings.hostname !== undefined) {
            this.saveHostname(settings.hostname);
        }

        if (settings.instanceMapping !== undefined) {
            this.saveInstanceMapping(settings.instanceMapping);
        }

        if (settings.devNotes !== undefined) {
            this.saveDevNotes(settings.devNotes);
        }

        // Notify listeners
        this.notifyListeners(updated as AppSettings);
    }

    /**
     * Update specific setting category
     */
    updateAutomationSettings(settings: Partial<AutomationSettings>): void {
        const current = this.getSettings();
        const updated = {
            ...current,
            ...settings,
        };
        this.saveAutomationSettings(updated);
        this.settings = updated;
        this.notifyListeners(updated);
    }

    updateConnections(connections: Partial<ConnectionSettings>): void {
        const current = this.getSettings();
        const updated = {
            ...current,
            connections: {
                ...current.connections,
                ...connections,
            },
        };
        this.saveConnections(updated.connections);
        this.settings = updated;
        this.notifyListeners(updated);
    }

    updateEnginePaths(enginePaths: Partial<EnginePaths>): void {
        const current = this.getSettings();
        const updated = {
            ...current,
            enginePaths: {
                ...current.enginePaths,
                ...enginePaths,
            },
        };
        this.saveEnginePaths(updated.enginePaths);
        this.settings = updated;
        this.notifyListeners(updated);
    }

    // Private methods for loading/saving individual settings

    private loadAutomationSettings(): ExtendedSettings {
        try {
            const stored = localStorage.getItem(STORAGE_KEYS.SETTINGS);
            if (stored) {
                const parsed = JSON.parse(stored);
                let engineApproaches = mergeEngineApproaches(parsed.engineApproaches);
                if (!parsed.engineApproaches) {
                    try {
                        const c = localStorage.getItem('playout-use-caspar-engine');
                        if (c != null) engineApproaches = { ...engineApproaches, casparTransport: c === '1' };
                        const ap = localStorage.getItem('playplay_autopush');
                        if (ap != null) engineApproaches = { ...engineApproaches, casparHourAutoPush: ap !== 'false' };
                    } catch {
                        /* ignore */
                    }
                }
                return {
                    ...DEFAULT_SETTINGS,
                    ...parsed,
                    obsSessions: parsed.obsSessions || [],
                    ddrAutoPreload: parsed.ddrAutoPreload ?? true,
                    ddrDefaultVolume: parsed.ddrDefaultVolume ?? 100,
                    socialAccounts: parsed.socialAccounts || [],
                    socialDefaultTemplate: parsed.socialDefaultTemplate || '',
                    liveInputEnabled: parsed.liveInputEnabled ?? false,
                    engineApproaches,
                };
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to load automation settings:', e);
        }
        return {
            ...DEFAULT_SETTINGS,
            obsSessions: [],
            ddrAutoPreload: true,
            ddrDefaultVolume: 100,
            socialAccounts: [],
            socialDefaultTemplate: '',
            liveInputEnabled: false,
            engineApproaches: { ...DEFAULT_ENGINE_APPROACHES },
        };
    }

    private safeLocalStorageSet(key: string, value: string, label: string): void {
        try {
            localStorage.setItem(key, value);
        } catch (e) {
            if (e instanceof DOMException && e.name === 'QuotaExceededError') {
                console.error(`[SettingsManager] localStorage quota exceeded saving ${label} — consider clearing old data`);
            } else {
                console.error(`[SettingsManager] Failed to save ${label}:`, e);
            }
        }
    }

    private saveAutomationSettings(settings: ExtendedSettings): void {
        let existing: Record<string, unknown> = {};
        try {
            const raw = localStorage.getItem(STORAGE_KEYS.SETTINGS);
            if (raw) existing = JSON.parse(raw);
        } catch {
            /* keep existing empty */
        }
        const toSave = {
            ...existing,
            alwaysPlayLatestEpisode: settings.alwaysPlayLatestEpisode,
            shuffleAdsInHour: settings.shuffleAdsInHour,
            fillGapsWithIds: settings.fillGapsWithIds,
            obsSessions: settings.obsSessions || [],
            ddrAutoPreload: settings.ddrAutoPreload ?? true,
            ddrDefaultVolume: settings.ddrDefaultVolume ?? 100,
            engineApproaches: mergeEngineApproaches(
                (settings.engineApproaches ?? existing.engineApproaches) as EngineApproachesMergeInput | undefined
            ),
        };
        this.safeLocalStorageSet(STORAGE_KEYS.SETTINGS, JSON.stringify(toSave), 'automation settings');
        this.syncRamdiskTempStubFile(settings);
    }

    /** Main reads this before `app.whenReady` to set Electron `temp` on a RAM disk; restart required after toggles. */
    private syncRamdiskTempStubFile(settings: ExtendedSettings): void {
        try {
            const api = typeof window !== 'undefined' ? (window as unknown as { electronAPI?: { writePlayplayRamdiskTempStub?: (p: unknown) => Promise<unknown> } }).electronAPI : undefined;
            if (!api?.writePlayplayRamdiskTempStub) return;
            const ea = mergeEngineApproaches(settings.engineApproaches);
            const r = ea.ramdisk;
            void api.writePlayplayRamdiskTempStub({
                enableElectronTempOnRamdisk: !!r?.enableElectronTempOnRamdisk,
                root: r?.root || 'B:/',
                electronTempFolder: r?.electronTempFolder || 'PlayPlay-electron-temp',
            });
        } catch {
            /* non-Electron or API missing */
        }
    }

    private loadConnections(): ConnectionSettings {
        try {
            const stored = localStorage.getItem(STORAGE_KEYS.CONNECTIONS);
            if (stored) {
                // Deep-merge stored values into defaults so new fields are never missing
                const parsed = JSON.parse(stored);
                const merged: ConnectionSettings = {
                    obs:       { ...DEFAULT_CONNECTIONS.obs,       ...parsed.obs },
                    caspercg:  { ...DEFAULT_CONNECTIONS.caspercg,  ...parsed.caspercg },
                    ffplayout: { ...DEFAULT_CONNECTIONS.ffplayout, ...parsed.ffplayout },
                    phpApi:    { ...DEFAULT_CONNECTIONS.phpApi,    ...parsed.phpApi },
                };
                // Warn when credentials are missing — don't silently back-fill admin/admin
                if (!merged.ffplayout.username || !merged.ffplayout.password) {
                    console.warn('[SettingsManager] ffplayout credentials are not configured');
                }
                return merged;
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to load connections:', e);
        }
        return { ...DEFAULT_CONNECTIONS };
    }

    private saveConnections(connections: ConnectionSettings): void {
        this.safeLocalStorageSet(STORAGE_KEYS.CONNECTIONS, JSON.stringify(connections), 'connections');
    }

    private loadEnginePaths(): EnginePaths {
        try {
            const stored = localStorage.getItem(STORAGE_KEYS.ENGINE_PATHS);
            if (stored) {
                return JSON.parse(stored);
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to load engine paths:', e);
        }
        return {};
    }

    private saveEnginePaths(enginePaths: EnginePaths): void {
        this.safeLocalStorageSet(STORAGE_KEYS.ENGINE_PATHS, JSON.stringify(enginePaths), 'engine paths');
    }

    private loadHostname(): string | undefined {
        try {
            return localStorage.getItem(STORAGE_KEYS.HOSTNAME) || undefined;
        } catch (e) {
            console.error('[SettingsManager] Failed to load hostname:', e);
        }
        return undefined;
    }

    private saveHostname(hostname: string | undefined): void {
        try {
            if (hostname) {
                this.safeLocalStorageSet(STORAGE_KEYS.HOSTNAME, hostname, 'hostname');
            } else {
                localStorage.removeItem(STORAGE_KEYS.HOSTNAME);
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to remove hostname:', e);
        }
    }

    private loadInstanceMapping(): { siteId?: string; playerId?: string } | undefined {
        try {
            const stored = localStorage.getItem(STORAGE_KEYS.INSTANCE_MAPPING);
            if (stored) {
                return JSON.parse(stored);
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to load instance mapping:', e);
        }
        return undefined;
    }

    private saveInstanceMapping(mapping: { siteId?: string; playerId?: string } | undefined): void {
        try {
            if (mapping) {
                this.safeLocalStorageSet(STORAGE_KEYS.INSTANCE_MAPPING, JSON.stringify(mapping), 'instance mapping');
            } else {
                localStorage.removeItem(STORAGE_KEYS.INSTANCE_MAPPING);
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to remove instance mapping:', e);
        }
    }

    private loadDevNotes(): string | undefined {
        try {
            return localStorage.getItem(STORAGE_KEYS.DEV_NOTES) || undefined;
        } catch (e) {
            console.error('[SettingsManager] Failed to load dev notes:', e);
        }
        return undefined;
    }

    private saveDevNotes(notes: string | undefined): void {
        try {
            if (notes) {
                this.safeLocalStorageSet(STORAGE_KEYS.DEV_NOTES, notes, 'dev notes');
            } else {
                localStorage.removeItem(STORAGE_KEYS.DEV_NOTES);
            }
        } catch (e) {
            console.error('[SettingsManager] Failed to remove dev notes:', e);
        }
    }

    /**
     * Subscribe to settings changes
     */
    onSettingsChange(callback: (settings: AppSettings) => void): () => void {
        this.listeners.add(callback);
        return () => this.listeners.delete(callback);
    }

    private notifyListeners(settings: AppSettings): void {
        this.listeners.forEach(callback => {
            try {
                callback(settings);
            } catch (e) {
                console.error('[SettingsManager] Listener error:', e);
            }
        });
    }

    /**
     * Reset to defaults
     */
    reset(): void {
        this.settings = null;
        localStorage.removeItem(STORAGE_KEYS.SETTINGS);
        localStorage.removeItem(STORAGE_KEYS.CONNECTIONS);
        localStorage.removeItem(STORAGE_KEYS.ENGINE_PATHS);
        localStorage.removeItem(STORAGE_KEYS.HOSTNAME);
        localStorage.removeItem(STORAGE_KEYS.INSTANCE_MAPPING);
        localStorage.removeItem(STORAGE_KEYS.DEV_NOTES);
        const defaults = this.loadAll();
        this.notifyListeners(defaults);
    }
}

// Export singleton instance
export const settingsManager = new SettingsManager();

// Export convenience functions for backward compatibility
export function loadSettings(): AutomationSettings {
    return settingsManager.getSettings();
}

export function saveSettings(settings: Partial<AutomationSettings>): void {
    settingsManager.updateAutomationSettings(settings);
}

export function loadConnectionSettings(): ConnectionSettings {
    return settingsManager.getSettings().connections;
}

export function saveConnectionSettings(connections: ConnectionSettings): void {
    settingsManager.updateConnections(connections);
}

export function loadEnginePaths(): EnginePaths {
    return settingsManager.getSettings().enginePaths;
}

export function saveEnginePaths(enginePaths: EnginePaths): void {
    settingsManager.updateEnginePaths(enginePaths);
}

