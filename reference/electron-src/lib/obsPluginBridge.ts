import { EventEmitter } from 'events';

export interface OBSPluginCommand {
    type: string;
    payload: Record<string, unknown>;
}

export interface OBSPluginState {
    scenes: string[];
    currentScene: string;
    sources: Record<string, unknown>;
    recording: boolean;
    streaming: boolean;
}

class OBSPluginBridge extends EventEmitter {
    private isConnected: boolean = false;
    private pluginState: OBSPluginState | null = null;

    async connect(): Promise<boolean> {
        // Connect to OBS plugin via IPC/WebSocket
        if (typeof window !== 'undefined' && (window as any).electronAPI?.obsPluginConnect) {
            const result = await (window as any).electronAPI.obsPluginConnect();
            this.isConnected = result.success;
            return this.isConnected;
        }
        return false;
    }

    async sendCommand(command: OBSPluginCommand): Promise<boolean> {
        if (!this.isConnected) return false;
        
        if (typeof window !== 'undefined' && (window as any).electronAPI?.obsPluginCommand) {
            const result = await (window as any).electronAPI.obsPluginCommand(command);
            return result.success;
        }
        return false;
    }

    getState(): OBSPluginState | null {
        return this.pluginState;
    }
}

export const obsPluginBridge = new OBSPluginBridge();

