// ============================================
// Drive Change Detection Service
// ============================================

import { DriveWatchConfig } from './types';

class DriveWatcher {
    private watchers: Map<string, any> = new Map();
    private configs: Map<string, DriveWatchConfig> = new Map();

    async watchPath(config: DriveWatchConfig, path: string): Promise<void> {
        const api = (window as any).electronAPI;
        if (api?.watchDirectory) {
            const watcher = await api.watchDirectory(path, (event: string, filePath: string) => {
                this.handleChange(event, filePath, path);
            });
            this.watchers.set(path, watcher);
            this.configs.set(path, config);
        }
    }

    private handleChange(event: string, filePath: string, watchedPath: string): void {
        const config = this.configs.get(watchedPath);
        if (!config) return;

        // Check include/exclude patterns
        if (config.excludePatterns) {
            for (const pattern of config.excludePatterns) {
                if (filePath.includes(pattern)) return;
            }
        }

        // Emit change event
        window.dispatchEvent(new CustomEvent('file-changed', {
            detail: { event, filePath, watchedPath }
        }));
    }

    unwatchPath(path: string): void {
        const watcher = this.watchers.get(path);
        if (watcher) {
            const api = (window as any).electronAPI;
            if (api?.unwatchDirectory) {
                api.unwatchDirectory(watcher);
            }
            this.watchers.delete(path);
            this.configs.delete(path);
        }
    }
}

export const driveWatcher = new DriveWatcher();




