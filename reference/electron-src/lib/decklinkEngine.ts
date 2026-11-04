/**
 * DeckLink Engine
 * Blackmagic DeckLink device enumeration and control
 */

import type { DeckLinkDevice, DeckLinkConfig } from './types';

export class DeckLinkEngine {
    private api: any;
    private devices: DeckLinkDevice[] = [];

    constructor(api: any) {
        this.api = api;
    }

    /**
     * Enumerate DeckLink devices
     */
    async enumerateDevices(): Promise<DeckLinkDevice[]> {
        if (!this.api) {
            return [];
        }

        try {
            const result = await this.api.decklinkEnumerate?.();
            if (result?.success && result.devices) {
                this.devices = result.devices;
                return this.devices;
            }
        } catch (error) {
            console.error('[DeckLink Engine] Enumeration failed:', error);
        }

        return this.devices;
    }

    /**
     * Get available video modes for a device
     */
    async getVideoModes(deviceIndex: number): Promise<string[]> {
        if (!this.api) {
            return [];
        }

        try {
            const result = await this.api.decklinkGetVideoModes?.(deviceIndex);
            return result?.modes || [];
        } catch (error) {
            console.error('[DeckLink Engine] Get video modes failed:', error);
            return [];
        }
    }

    /**
     * Start DeckLink input
     */
    async startInput(config: DeckLinkConfig): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.decklinkStartInput?.(config);
        return result || { success: false, error: 'DeckLink input not available' };
    }

    /**
     * Start DeckLink output
     */
    async startOutput(config: DeckLinkConfig): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.decklinkStartOutput?.(config);
        return result || { success: false, error: 'DeckLink output not available' };
    }

    /**
     * Stop DeckLink input/output
     */
    async stop(key: string): Promise<{ success: boolean }> {
        if (!this.api) {
            return { success: false };
        }

        return await this.api.decklinkStop?.(key) || { success: false };
    }
}




