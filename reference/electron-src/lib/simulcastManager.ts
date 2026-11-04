// ============================================
// Simulcast Manager
// Handles low-res AV1 preview streams and quality switching
// ============================================

export type SimulcastQuality = 'preview' | 'fast' | 'heavy';
export type SimulcastMode = 'preview' | 'master' | 'receiver';

export interface SimulcastConfig {
    sessionId: string;
    hostname: string;
    mode: SimulcastMode;
    quality: SimulcastQuality;
    // Preview: 16:9 80kbps 150px wide AV1
    // Fast: 720p 30fps 800kbps (HW volatile)
    // Heavy: 720p 2Mbps H.264 (HW safe)
}

export interface SimulcastStream {
    sessionId: string;
    hostname: string;
    streamUrl: string;
    quality: SimulcastQuality;
    isLocalMaster: boolean;
    width: number;
    height: number;
    bitrate: number;
    codec: 'av1' | 'h264';
    hardwareAccel: 'none' | 'nvenc' | 'amd' | 'auto';
}

class SimulcastManager {
    private streams: Map<string, SimulcastStream> = new Map();
    private localConfig: SimulcastConfig | null = null;
    private hardwareEncoder: 'nvenc' | 'amd' | 'none' = 'none';

    constructor() {
        this.detectHardwareEncoder();
    }

    private async detectHardwareEncoder(): Promise<void> {
        // Check for NVENC
        try {
            const api = (window as any).electronAPI;
            if (api?.checkHardwareEncoder) {
                const result = await api.checkHardwareEncoder();
                if (result.nvenc) {
                    this.hardwareEncoder = 'nvenc';
                    return;
                }
                if (result.amd) {
                    this.hardwareEncoder = 'amd';
                    return;
                }
            }
        } catch (e) {
            console.warn('[Simulcast] Failed to detect hardware encoder:', e);
        }
        this.hardwareEncoder = 'none';
    }

    async startPreviewStream(sessionId: string, hostname: string): Promise<string> {
        // Start 16:9 80kbps 150px wide AV1 preview stream
        const streamUrl = await this.createPreviewStream(sessionId, hostname);
        
        const stream: SimulcastStream = {
            sessionId,
            hostname,
            streamUrl,
            quality: 'preview',
            isLocalMaster: false,
            width: 150,
            height: 84, // 16:9 aspect
            bitrate: 80000, // 80kbps
            codec: 'av1',
            hardwareAccel: 'none',
        };

        this.streams.set(sessionId, stream);
        return streamUrl;
    }

    async switchToFast(sessionId: string): Promise<string> {
        // Switch to 720p 30fps 800kbps (HW volatile)
        const stream = this.streams.get(sessionId);
        if (!stream) {
            throw new Error(`Stream not found for session ${sessionId}`);
        }

        const streamUrl = await this.createFastStream(sessionId, stream.hostname);
        
        const updated: SimulcastStream = {
            ...stream,
            streamUrl,
            quality: 'fast',
            isLocalMaster: sessionId === this.localConfig?.sessionId,
            width: 1280,
            height: 720,
            bitrate: 800000, // 800kbps
            codec: 'av1',
            hardwareAccel: this.hardwareEncoder === 'none' ? 'none' : 'auto',
        };

        this.streams.set(sessionId, updated);
        return streamUrl;
    }

    async switchToHeavy(sessionId: string): Promise<string> {
        // Switch to 720p 2Mbps H.264 (HW safe)
        const stream = this.streams.get(sessionId);
        if (!stream) {
            throw new Error(`Stream not found for session ${sessionId}`);
        }

        const streamUrl = await this.createHeavyStream(sessionId, stream.hostname);
        
        const updated: SimulcastStream = {
            ...stream,
            streamUrl,
            quality: 'heavy',
            isLocalMaster: sessionId === this.localConfig?.sessionId,
            width: 1280,
            height: 720,
            bitrate: 2000000, // 2Mbps
            codec: 'h264',
            hardwareAccel: this.hardwareEncoder,
        };

        this.streams.set(sessionId, updated);
        return streamUrl;
    }

    private async createPreviewStream(sessionId: string, hostname: string): Promise<string> {
        // Create AV1 preview stream via FFmpeg/encoder
        const api = (window as any).electronAPI;
        if (api?.createSimulcastStream) {
            const result = await api.createSimulcastStream({
                sessionId,
                hostname,
                quality: 'preview',
                width: 150,
                height: 84,
                bitrate: 80000,
                codec: 'av1',
            });
            return result.streamUrl;
        }
        // Fallback: return placeholder URL
        return `simulcast://${sessionId}/preview`;
    }

    private async createFastStream(sessionId: string, hostname: string): Promise<string> {
        const api = (window as any).electronAPI;
        if (api?.createSimulcastStream) {
            const result = await api.createSimulcastStream({
                sessionId,
                hostname,
                quality: 'fast',
                width: 1280,
                height: 720,
                framerate: 30,
                bitrate: 800000,
                codec: 'av1',
                hardwareAccel: this.hardwareEncoder === 'none' ? 'none' : 'auto',
            });
            return result.streamUrl;
        }
        return `simulcast://${sessionId}/fast`;
    }

    private async createHeavyStream(sessionId: string, hostname: string): Promise<string> {
        const api = (window as any).electronAPI;
        if (api?.createSimulcastStream) {
            const result = await api.createSimulcastStream({
                sessionId,
                hostname,
                quality: 'heavy',
                width: 1280,
                height: 720,
                framerate: 30,
                bitrate: 2000000,
                codec: 'h264',
                hardwareAccel: this.hardwareEncoder,
            });
            return result.streamUrl;
        }
        return `simulcast://${sessionId}/heavy`;
    }

    getStream(sessionId: string): SimulcastStream | undefined {
        return this.streams.get(sessionId);
    }

    getAllStreams(): SimulcastStream[] {
        return Array.from(this.streams.values());
    }

    setLocalConfig(config: SimulcastConfig): void {
        this.localConfig = config;
    }

    getLocalConfig(): SimulcastConfig | null {
        return this.localConfig;
    }
}

export const simulcastManager = new SimulcastManager();




