/**
 * DekTec SDI Engine
 * Direct SDI output control using dtbridge.dll
 */

import type { DekTecConfig, SdiVideoFormat } from './types';

export interface SdiVideoMode {
    format: SdiVideoFormat;
    width: number;
    height: number;
    framerate: number;
    interlaced: boolean;
    standard: 'smpte292m' | 'smpte259m';
}

export const SDI_VIDEO_MODES: Record<SdiVideoFormat, SdiVideoMode> = {
    '720p59.94': { format: '720p59.94', width: 1280, height: 720, framerate: 59.94, interlaced: false, standard: 'smpte292m' },
    '720p60': { format: '720p60', width: 1280, height: 720, framerate: 60, interlaced: false, standard: 'smpte292m' },
    '720p50': { format: '720p50', width: 1280, height: 720, framerate: 50, interlaced: false, standard: 'smpte292m' },
    '720p25': { format: '720p25', width: 1280, height: 720, framerate: 25, interlaced: false, standard: 'smpte292m' },
    '1080i59.94': { format: '1080i59.94', width: 1920, height: 1080, framerate: 29.97, interlaced: true, standard: 'smpte292m' },
    '1080i60': { format: '1080i60', width: 1920, height: 1080, framerate: 30, interlaced: true, standard: 'smpte292m' },
    '1080i50': { format: '1080i50', width: 1920, height: 1080, framerate: 25, interlaced: true, standard: 'smpte292m' },
    '1080p29.97': { format: '1080p29.97', width: 1920, height: 1080, framerate: 29.97, interlaced: false, standard: 'smpte292m' },
    '1080p30': { format: '1080p30', width: 1920, height: 1080, framerate: 30, interlaced: false, standard: 'smpte292m' },
    '1080p25': { format: '1080p25', width: 1920, height: 1080, framerate: 25, interlaced: false, standard: 'smpte292m' },
    '1080p23.98': { format: '1080p23.98', width: 1920, height: 1080, framerate: 23.98, interlaced: false, standard: 'smpte292m' },
    '1080p24': { format: '1080p24', width: 1920, height: 1080, framerate: 24, interlaced: false, standard: 'smpte292m' },
};

export class DekTecSdiEngine {
    private api: any;

    constructor(api: any) {
        this.api = api;
    }

    /**
     * Get SDI video mode details
     */
    getSdiVideoMode(format: SdiVideoFormat): SdiVideoMode {
        return SDI_VIDEO_MODES[format];
    }

    /**
     * Start SDI output using dtbridge.dll
     */
    async startSdiOutput(config: DekTecConfig & { sdiVideoFormat: SdiVideoFormat }): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.dektecStartSdi?.(config);
        return result || { success: false, error: 'SDI output not available' };
    }

    /**
     * Stop SDI output
     */
    async stopSdiOutput(key: string): Promise<{ success: boolean }> {
        if (!this.api) {
            return { success: false };
        }

        return await this.api.dektecStopSdi?.(key) || { success: false };
    }

    /**
     * Write video frame to SDI output
     */
    async writeSdiFrame(key: string, frameData: Buffer): Promise<{ success: boolean }> {
        if (!this.api) {
            return { success: false };
        }

        return await this.api.dektecWriteSdiFrame?.(key, frameData) || { success: false };
    }
}




