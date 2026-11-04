/**
 * TSDUCK Integration - Enhanced 2026 Professional Broadcast Suite
 * Advanced wrapper for TSDUCK statmux, packet injection, and transport stream analysis
 * 
 * Features:
 * - Statistical multiplexing with intelligent bitrate allocation
 * - ATSC 1.0/3.0 PSIP table generation
 * - Real-time transport stream analysis
 * - Compliance validation against broadcast standards
 * - Low-latency packet injection
 * - Multi-program muxing
 */

import type { TSDUCKConfig, MuxChannelConfig } from './types';

// Extended types for professional broadcast operations
export interface TSDUCKStatMuxChannel {
    channelId: number;
    inputUrl: string; // UDP URL or file path
    minBitrate?: number;
    maxBitrate?: number;
    priority?: number;
    // Enhanced professional features
    encodingProfile?: 'broadcast' | 'cable' | 'iptv' | 'ott';
    colorSpace?: 'bt709' | 'bt2020' | 'smpte170m';
    audioMode?: 'stereo' | 'surround51' | 'surround71' | 'atmos';
    closedCaptions?: boolean;
    cea608?: boolean;
    cea708?: boolean;
}

export interface TransportStreamAnalysis {
    pid: number;
    type: 'video' | 'audio' | 'data' | 'psi' | 'null';
    codec?: string;
    bitrate: number;
    packetCount: number;
    errors: number;
    continuityErrors: number;
    pcrJitter?: number;
    description?: string;
}

export interface StreamHealth {
    overallScore: number; // 0-100
    bitrateStability: number;
    pcrAccuracy: number;
    continuityErrorRate: number;
    bufferHealth: number;
    recommendations: string[];
}

export interface PSIPTableStatus {
    table: string;
    version: number;
    lastUpdate: number;
    nextUpdate: number;
    status: 'ok' | 'stale' | 'missing' | 'error';
}

export class TSDUCKIntegration {
    private api: any;
    private runningProcesses: Map<string, any> = new Map();

    constructor(api: any) {
        this.api = api;
    }

    /**
     * Start statistical multiplexing
     */
    async startStatMux(
        channels: TSDUCKStatMuxChannel[],
        outputUrl: string,
        totalBitrate: number
    ): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckStartStatMux?.({
            channels,
            outputUrl,
            totalBitrate,
        });

        if (result?.success && result.key) {
            this.runningProcesses.set(result.key, { type: 'statmux', channels, outputUrl });
        }

        return result || { success: false, error: 'TSDUCK statmux not available' };
    }

    /**
     * Start packet injection (PSI/SI tables and ATSC PSIP)
     */
    async startPacketInjection(
        inputUrl: string,
        outputUrl: string,
        config: {
            // Basic PSI/SI
            pat?: boolean;
            pmt?: boolean;
            sdt?: boolean;
            eit?: boolean;
            networkName?: string;
            serviceName?: string;
            serviceId?: number;
            programNumber?: number;
            pmtPid?: number;
            videoPid?: number;
            audioPid?: number;
            // ATSC PSIP
            psip?: {
                enabled: boolean;
                mgt?: any;
                vct?: {
                    enabled: boolean;
                    transportStreamId: number;
                    channels: Array<{
                        shortName: string;
                        majorChannelNumber: number;
                        minorChannelNumber: number;
                        modulationMode: number;
                        carrierFrequency: number;
                        channelTSID: number;
                        programNumber: number;
                        serviceType: number;
                        sourceId: number;
                    }>;
                };
                eit?: {
                    enabled: boolean;
                    events: Array<{
                        eventId: number;
                        startTime: number;
                        lengthInSeconds: number;
                        title: string;
                        description?: string;
                    }>;
                };
                stt?: {
                    enabled: boolean;
                };
            };
            // ATSC 3.0 VTC
            atsc3?: {
                enabled: boolean;
                vtc?: {
                    channelFrequency: number;
                    channelNumber: number;
                    subChannelNumber: number;
                    serviceId: number;
                    plpId: number;
                };
            };
            // Low jitter / constant bitrate
            lowJitter?: boolean;
            constantBitrate?: number; // bps
            customScript?: string;
        }
    ): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckStartPacketInjection?.({
            inputUrl,
            outputUrl,
            config,
        });

        if (result?.success && result.key) {
            this.runningProcesses.set(result.key, { type: 'injection', inputUrl, outputUrl });
        }

        return result || { success: false, error: 'TSDUCK packet injection not available' };
    }

    /**
     * Validate transport stream compliance
     */
    async validateCompliance(
        inputUrl: string,
        atscVersion: '1.0' | '3.0' = '1.0'
    ): Promise<{ 
        success: boolean; 
        compliant?: boolean; 
        errors?: string[]; 
        warnings?: string[];
        error?: string;
    }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckValidateCompliance?.(inputUrl, atscVersion);
        return result || { success: false, error: 'TSDUCK validation not available' };
    }

    /**
     * Stop TSDUCK process
     */
    async stop(key: string): Promise<{ success: boolean }> {
        if (!this.api) {
            return { success: false };
        }

        const result = await this.api.tsduckStop?.(key);
        if (result?.success) {
            this.runningProcesses.delete(key);
        }

        return result || { success: false };
    }

    /**
     * Get running processes
     */
    getRunningProcesses(): string[] {
        return Array.from(this.runningProcesses.keys());
    }

    /**
     * Analyze transport stream in real-time
     */
    async analyzeStream(
        inputUrl: string
    ): Promise<{ 
        success: boolean; 
        analysis?: TransportStreamAnalysis[]; 
        health?: StreamHealth;
        error?: string;
    }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckAnalyzeStream?.(inputUrl);
        return result || { success: false, error: 'TSDUCK analysis not available' };
    }

    /**
     * Get PSIP table status for monitoring
     */
    async getPSIPStatus(
        inputUrl: string
    ): Promise<{
        success: boolean;
        tables?: PSIPTableStatus[];
        error?: string;
    }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckGetPSIPStatus?.(inputUrl);
        return result || { success: false, error: 'TSDUCK PSIP status not available' };
    }

    /**
     * Start continuous stream monitoring
     */
    async startStreamMonitor(
        inputUrl: string,
        config: {
            analyzeInterval?: number; // ms
            healthThresholds?: {
                minBitrate?: number;
                maxPcrJitter?: number;
                maxContinuityErrors?: number;
            };
            alertCallback?: string; // Webhook URL for alerts
        }
    ): Promise<{ success: boolean; monitorKey?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckStartMonitor?.({
            inputUrl,
            config,
        });

        if (result?.success && result.monitorKey) {
            this.runningProcesses.set(result.monitorKey, { type: 'monitor', inputUrl });
        }

        return result || { success: false, error: 'TSDUCK monitor not available' };
    }

    /**
     * Generate SCTE-35 splice commands for ad insertion
     */
    async injectSCTE35(
        inputUrl: string,
        outputUrl: string,
        spliceConfig: {
            type: 'splice_insert' | 'splice_null' | 'time_signal';
            eventId: number;
            duration?: number; // frames
            autoReturn?: boolean;
            breakDuration?: number; // frames
            uniqueProgramId?: number;
        }
    ): Promise<{ success: boolean; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckInjectSCTE35?.({
            inputUrl,
            outputUrl,
            spliceConfig,
        });

        return result || { success: false, error: 'TSDUCK SCTE-35 injection not available' };
    }

    /**
     * Generate EAS (Emergency Alert System) messages
     */
    async injectEAS(
        inputUrl: string,
        outputUrl: string,
        easConfig: {
            eventCode: string; // e.g., 'TOR' for Tornado Warning
            originatorCode: string; // e.g., 'WXR' for National Weather Service
            locationCodes: string[];
            duration: number; // minutes
            alertText?: string;
            audioUrl?: string;
        }
    ): Promise<{ success: boolean; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckInjectEAS?.({
            inputUrl,
            outputUrl,
            easConfig,
        });

        return result || { success: false, error: 'TSDUCK EAS injection not available' };
    }

    /**
     * Start multi-program muxing with intelligent bitrate allocation
     */
    async startIntelligentMux(
        channels: TSDUCKStatMuxChannel[],
        outputUrl: string,
        config: {
            totalBitrate: number;
            algorithm: 'cbr' | 'vbr' | 'statistical';
            minChannelBitrate?: number;
            maxChannelBitrate?: number;
            complexityAnalysis?: boolean;
            sceneChangeDetection?: boolean;
            bufferModel?: 'leaky' | 'cbr';
        }
    ): Promise<{ success: boolean; key?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckStartIntelligentMux?.({
            channels,
            outputUrl,
            config,
        });

        if (result?.success && result.key) {
            this.runningProcesses.set(result.key, { type: 'intelligent_mux', channels, outputUrl });
        }

        return result || { success: false, error: 'TSDUCK intelligent mux not available' };
    }

    /**
     * Get real-time mux statistics
     */
    async getMuxStats(key: string): Promise<{
        success: boolean;
        stats?: {
            totalBitrate: number;
            channelBitrates: Record<number, number>;
            bufferFillLevel: number;
            dropCount: number;
            overflowCount: number;
            uptime: number;
        };
        error?: string;
    }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckGetMuxStats?.(key);
        return result || { success: false, error: 'TSDUCK mux stats not available' };
    }

    /**
     * Export stream segment for QC
     */
    async exportSegment(
        inputUrl: string,
        outputPath: string,
        config: {
            duration: number; // seconds
            startTime?: number; // offset from current
            includeMetadata?: boolean;
            format: 'ts' | 'mp4' | 'm2ts';
        }
    ): Promise<{ success: boolean; filePath?: string; error?: string }> {
        if (!this.api) {
            return { success: false, error: 'Not in Electron environment' };
        }

        const result = await this.api.tsduckExportSegment?.({
            inputUrl,
            outputPath,
            config,
        });

        return result || { success: false, error: 'TSDUCK segment export not available' };
    }
}

