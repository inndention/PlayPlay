// ============================================
// ATSC Multi-Channel Output System
// 4-6 channel ATSC playout with H.264 encoding
// Uses OBS Branch Output plugin + TSDuck for multi-port output
// ============================================

import { TSDUCKIntegration } from './tsduckIntegration';
import { OBSBranchOutputManager } from './obsBranchOutput';
import { OBSWebSocketClient } from './obsWebSocket';
import { StatMuxController, StatMuxConfig, StatMuxAllocation } from './statmuxController';
import { generateId } from './utils';

export interface ATSCChannelConfig {
    channelId: number;
    name: string;
    majorChannel: number;
    minorChannel: number;
    sourceId: number;
    programNumber: number;
    
    // Input source
    inputSource: {
        type: 'udp' | 'srt' | 'file' | 'playplay' | 'ndi' | 'decklink';
        url?: string;
        filePath?: string;
        instanceId?: string;
        ndiSourceName?: string;
        deckLinkIndex?: number;
    };
    
    // H.264 encoding (YOLO mode - not strictly ATSC 1.0 compliant but we're going for it)
    encoding: {
        videoBitrate: number; // bps
        audioBitrate: number; // bps (AC-3)
        resolution: '1920x1080' | '1280x720' | '720x480';
        framerate: number; // 29.97, 30, 59.94, 60
        profile: 'baseline' | 'main' | 'high';
        level: string; // e.g., '4.0', '4.1', '4.2'
        gopSize: number;
        bFrames: number;
        keyframeInterval: number;
    };
    
    // PIDs for this channel
    pids: {
        video: number;
        audio: number;
        pcr: number;
        pmt: number;
    };
    
    enabled: boolean;
}

export interface ATSCMultiChannelConfig {
    transportStreamId: number;
    totalBitrate: number; // ATSC standard: 19392658 bps
    channels: ATSCChannelConfig[];
    
    // PSIP configuration
    psip: {
        networkName: string;
        shortName: string;
        modulationMode: number; // 0x04 = 8-VSB
        carrierFrequency: number;
    };
    
    // Output configuration - single ASI output for statmux
    output: {
        type: 'dektec_asi' | 'file' | 'udp';
        dektecDevice?: number;
        dektecPort?: number; // Single port for statmux output
        filePath?: string;
        udpUrl?: string;
    };
    
    // Statistical multiplexing configuration
    statmux?: {
        enabled: boolean;
        minChannelBitrate: number; // Minimum per channel (e.g., 1000000 = 1 Mbps)
        maxChannelBitrate: number; // Maximum per channel (e.g., 8000000 = 8 Mbps)
        updateInterval: number; // How often to recalculate (ms, e.g., 5000 = 5 seconds)
        complexityWindow: number; // Time window for complexity analysis (seconds)
        priorityWeights?: Record<number, number>; // Channel ID -> priority weight (1.0 = normal)
    };
    
    // Legacy: Use branch plugin for multi-output (deprecated, use single output for statmux)
    useBranchPlugin?: boolean;
}

export interface ChannelEncodingStatus {
    channelId: number;
    encoding: boolean;
    bitrate: number;
    frameRate: number;
    errors: string[];
}

export interface MultiChannelStatus {
    muxActive: boolean;
    channels: ChannelEncodingStatus[];
    outputs: Array<{
        outputId: string;
        type: string;
        active: boolean;
        bitrate?: number;
    }>;
    psipInjected: boolean;
}

class ATSCMultiChannelOutput {
    private config: ATSCMultiChannelConfig | null = null;
    private tsduck: TSDUCKIntegration;
    private obsBranchOutput: OBSBranchOutputManager | null = null;
    private obsClient: OBSWebSocketClient | null = null;
    private statMuxController: StatMuxController | null = null;
    private encodingProcesses: Map<number, string> = new Map(); // channelId -> process key
    private muxProcessKey: string | null = null;
    private branchProcessKeys: Map<string, string> = new Map(); // outputId -> process key
    private status: MultiChannelStatus;
    private currentAllocations: Map<number, StatMuxAllocation> = new Map();

    constructor(api: any, obsClient?: OBSWebSocketClient) {
        this.tsduck = new TSDUCKIntegration(api);
        this.obsClient = obsClient || null;
        if (this.obsClient) {
            this.obsBranchOutput = new OBSBranchOutputManager(this.obsClient);
        }
        this.status = {
            muxActive: false,
            channels: [],
            outputs: [],
            psipInjected: false,
        };
    }

    /**
     * Set OBS client for Branch Output integration
     */
    public setOBSClient(client: OBSWebSocketClient): void {
        this.obsClient = client;
        this.obsBranchOutput = new OBSBranchOutputManager(client);
    }

    /**
     * Configure multi-channel ATSC output
     */
    public setConfig(config: ATSCMultiChannelConfig): void {
        this.config = config;
        this.initializeStatus();
        
        // Initialize statmux controller if enabled
        if (config.statmux?.enabled) {
            const statMuxConfig: StatMuxConfig = {
                enabled: true,
                totalBitrate: config.totalBitrate,
                minChannelBitrate: config.statmux.minChannelBitrate || 1000000, // 1 Mbps default
                maxChannelBitrate: config.statmux.maxChannelBitrate || 8000000, // 8 Mbps default
                updateInterval: config.statmux.updateInterval || 5000, // 5 seconds default
                complexityWindow: config.statmux.complexityWindow || 10, // 10 seconds default
                priorityWeights: config.statmux.priorityWeights
                    ? new Map(Object.entries(config.statmux.priorityWeights).map(([k, v]) => [parseInt(k), v]))
                    : undefined,
            };
            this.statMuxController = new StatMuxController(statMuxConfig);
            
            // Initialize channels in statmux
            for (const channel of config.channels) {
                if (channel.enabled) {
                    this.statMuxController.initializeChannel(
                        channel.channelId,
                        channel.encoding.videoBitrate
                    );
                }
            }
        } else {
            this.statMuxController = null;
        }
    }

    /**
     * Start multi-channel ATSC playout
     */
    public async start(): Promise<{ success: boolean; error?: string }> {
        if (!this.config) {
            return { success: false, error: 'Not configured' };
        }

        try {
            // Step 1: Calculate initial statmux allocations if enabled
            if (this.statMuxController && this.config.statmux?.enabled) {
                const enabledChannels = this.config.channels.filter(ch => ch.enabled).map(ch => ch.channelId);
                const allocations = this.statMuxController.calculateAllocations(enabledChannels);
                
                // Start statmux auto-updates
                this.statMuxController.startAutoUpdate(enabledChannels, (newAllocations) => {
                    this.handleStatMuxUpdate(newAllocations);
                });
                
                // Store initial allocations
                for (const alloc of allocations) {
                    this.currentAllocations.set(alloc.channelId, alloc);
                }
            }

            // Step 2: Start H.264 encoding for each channel (with statmux bitrates if enabled)
            for (const channel of this.config.channels) {
                if (channel.enabled) {
                    // Use statmux allocation if available, otherwise use channel's configured bitrate
                    const allocation = this.currentAllocations.get(channel.channelId);
                    if (allocation) {
                        // Temporarily override channel bitrate with statmux allocation
                        const originalBitrate = channel.encoding.videoBitrate;
                        channel.encoding.videoBitrate = allocation.videoBitrate;
                        await this.startChannelEncoding(channel);
                        channel.encoding.videoBitrate = originalBitrate; // Restore
                    } else {
                        await this.startChannelEncoding(channel);
                    }
                }
            }

            // Step 3: Build MPTS with PSIP injection and single ASI output
            await this.startMuxWithPSIP();

            this.status.muxActive = true;
            return { success: true };
        } catch (error: any) {
            return { success: false, error: error.message || 'Failed to start multi-channel output' };
        }
    }

    /**
     * Handle statmux allocation updates
     */
    private async handleStatMuxUpdate(allocations: StatMuxAllocation[]): Promise<void> {
        console.log('[StatMux] New allocations:', allocations);
        
        // Update stored allocations
        for (const alloc of allocations) {
            this.currentAllocations.set(alloc.channelId, alloc);
        }

        // Update encoding bitrates for active channels
        // Note: This requires restarting encoding processes with new bitrates
        // For now, we log the update. Full dynamic bitrate adjustment would require
        // IPC communication to update FFmpeg/OBS encoding parameters in real-time.
        for (const alloc of allocations) {
            const channel = this.config?.channels.find(ch => ch.channelId === alloc.channelId);
            if (channel && channel.enabled) {
                console.log(`[StatMux] Channel ${alloc.channelId}: ${(alloc.videoBitrate / 1000000).toFixed(2)} Mbps video, ${(alloc.audioBitrate / 1000).toFixed(0)} kbps audio`);
                // TODO: Implement dynamic bitrate adjustment via IPC
            }
        }
    }

    /**
     * Start H.264 encoding for a channel
     * Uses OBS Branch Output plugin if available, otherwise falls back to FFmpeg
     */
    private async startChannelEncoding(channel: ATSCChannelConfig): Promise<void> {
        // If OBS Branch Output is available, use it
        if (this.obsBranchOutput && this.obsClient?.isConnected) {
            // Use Branch Output plugin to encode from OBS source
            const outputPath = `C:\\temp\\atsc_channel_${channel.channelId}.ts`;
            
            // Determine source name from input config
            let sourceName = 'Main View'; // Default
            if (channel.inputSource.type === 'playplay' && channel.inputSource.instanceId) {
                sourceName = `PlayPlay_${channel.inputSource.instanceId}`;
            } else if (channel.inputSource.type === 'ndi' && channel.inputSource.ndiSourceName) {
                sourceName = channel.inputSource.ndiSourceName;
            }

            const filterConfig = {
                filterName: `ATSC_Channel_${channel.channelId}`,
                sourceName: sourceName,
                outputTarget: outputPath,
                videoBitrate: Math.floor(channel.encoding.videoBitrate / 1000), // Convert to kbps
                audioBitrate: Math.floor(channel.encoding.audioBitrate / 1000),
                resolution: channel.encoding.resolution,
                framerate: channel.encoding.framerate,
                videoEncoder: 'libx264', // H.264
                audioEncoder: 'ac3',
                enableATSC: true,
                containerFormat: 'mpegts',
                audioSource: 'filter' as const,
                enabled: true,
            };

            const result = await this.obsBranchOutput.addBranchOutputFilter(filterConfig);
            if (result.success) {
                // Enable the filter
                await this.obsBranchOutput.setBranchOutputEnabled(
                    filterConfig.filterName,
                    sourceName,
                    true
                );
                this.encodingProcesses.set(channel.channelId, `obs_branch_${channel.channelId}`);
                this.updateChannelStatus(channel.channelId, { encoding: true });
                return;
            } else {
                console.warn(`[ATSC] Branch Output failed for channel ${channel.channelId}, falling back to FFmpeg:`, result.error);
            }
        }

        // Fallback to FFmpeg encoding
        const inputUrl = this.getInputUrl(channel);
        if (!inputUrl) {
            throw new Error(`Channel ${channel.channelId}: Invalid input source`);
        }

        const outputUrl = `udp://127.0.0.1:${5000 + channel.channelId}?pkt_size=1316`;
        
        if (typeof window !== 'undefined' && (window as any).electronAPI?.atscStartChannelEncoding) {
            const result = await (window as any).electronAPI.atscStartChannelEncoding({
                channelId: channel.channelId,
                inputUrl,
                outputUrl,
                encoding: channel.encoding,
                pids: channel.pids,
            });

            if (result.success && result.processKey) {
                this.encodingProcesses.set(channel.channelId, result.processKey);
                this.updateChannelStatus(channel.channelId, { encoding: true });
            } else {
                throw new Error(`Channel ${channel.channelId}: Encoding failed - ${result.error}`);
            }
        } else {
            throw new Error('ATSC encoding API not available');
        }
    }

    /**
     * Start multiplexing with PSIP injection
     */
    private async startMuxWithPSIP(): Promise<void> {
        if (!this.config) return;

        // Build PSIP XML for all channels
        const psipXml = this.generatePSIPXML();

        // Create temporary XML file
        const xmlPath = await this.savePSIPXML(psipXml);

        // Build TSDuck command for multiplexing with single ASI output
        // Input: null (start from scratch)
        // Inject PSIP tables
        // Mux all channel streams
        // Regulate bitrate to ATSC 19.3 Mbps
        // Output to single DekTec ASI port

        // Determine output URL - single ASI output for statmux
        let outputUrl: string;
        if (this.config.output.type === 'dektec_asi') {
            outputUrl = `dektec://${this.config.output.dektecDevice || 0}/${this.config.output.dektecPort || 0}`;
        } else if (this.config.output.type === 'file') {
            outputUrl = this.config.output.filePath || 'output.ts';
        } else if (this.config.output.type === 'udp') {
            outputUrl = this.config.output.udpUrl || 'udp://127.0.0.1:5000';
        } else {
            outputUrl = 'output.ts'; // Default to file
        }

        // Start TSDuck mux process via IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI?.atscStartMux) {
            const result = await (window as any).electronAPI.atscStartMux({
                config: this.config,
                psipXmlPath: xmlPath,
                outputUrl: outputUrl,
                singleASIOutput: true, // Single ASI output for statmux
                statmuxEnabled: this.config.statmux?.enabled || false,
            });

            if (result.success && result.processKey) {
                this.muxProcessKey = result.processKey;
                this.status.psipInjected = true;
            } else {
                throw new Error(`Mux failed: ${result.error}`);
            }
        } else {
            throw new Error('ATSC mux API not available');
        }
    }

    /**
     * Start branch outputs to multiple DekTec ports
     * Note: This method is deprecated - single ASI output is now used for statmux
     */
    private async startBranchOutputs(): Promise<void> {
        // Legacy method - no longer used for statmux (single ASI output)
        // Kept for backward compatibility
        if (!this.config) return;
        
        // Single output is handled in startMuxWithPSIP
        if (this.config.output) {
            this.status.outputs = [{
                outputId: generateId(),
                type: this.config.output.type,
                active: true,
            }];
        }
    }

    /**
     * Generate PSIP XML for all channels
     */
    private generatePSIPXML(): string {
        if (!this.config) return '';

        let xml = '<?xml version="1.0" encoding="UTF-8"?>\n<tsduck>\n\n';

        // MGT (Master Guide Table)
        xml += `  <MGT version="1" PID_base="0x1FFB">\n`;
        xml += `    <table type="0x0004" PID="0x1FFB" version="1" size="128" />\n`; // TVCT
        xml += `    <table type="0x00CB" PID="0x1FFB" version="1" size="256" />\n`; // EIT-0
        xml += `  </MGT>\n\n`;

        // TVCT (Terrestrial Virtual Channel Table)
        xml += `  <TVCT transport_stream_id="0x${this.config.transportStreamId.toString(16).toUpperCase().padStart(4, '0')}" version="1" protocol_version="0">\n`;
        
        for (const channel of this.config.channels) {
            if (channel.enabled) {
                xml += `    <channel major="${channel.majorChannel}" minor="${channel.minorChannel}" `;
                xml += `short_name="${channel.name.substring(0, 7).padEnd(7, ' ')}" `;
                xml += `program_number="${channel.programNumber}" `;
                xml += `service_type="0x02" source_id="${channel.sourceId}">\n`;
                xml += `      <extended_channel_name_descriptor>\n`;
                xml += `        <long_name text="${channel.name}"/>\n`;
                xml += `      </extended_channel_name_descriptor>\n`;
                xml += `    </channel>\n`;
            }
        }
        
        xml += `  </TVCT>\n\n`;

        // PAT (Program Association Table)
        xml += `  <PAT transport_stream_id="0x${this.config.transportStreamId.toString(16).toUpperCase().padStart(4, '0')}" version="1">\n`;
        for (const channel of this.config.channels) {
            if (channel.enabled) {
                xml += `    <service service_id="${channel.programNumber}" program_map_PID="0x${channel.pids.pmt.toString(16).toUpperCase().padStart(4, '0')}"/>\n`;
            }
        }
        xml += `  </PAT>\n\n`;

        // PMT (Program Map Table) for each channel
        for (const channel of this.config.channels) {
            if (channel.enabled) {
                xml += `  <PMT version="1" service_id="${channel.programNumber}" PCR_PID="0x${channel.pids.pcr.toString(16).toUpperCase().padStart(4, '0')}">\n`;
                xml += `    <component elementary_PID="0x${channel.pids.video.toString(16).toUpperCase().padStart(4, '0')}" stream_type="0x1B"/>\n`; // 0x1B = H.264
                xml += `    <component elementary_PID="0x${channel.pids.audio.toString(16).toUpperCase().padStart(4, '0')}" stream_type="0x81"/>\n`; // 0x81 = AC-3
                xml += `  </PMT>\n\n`;
            }
        }

        // STT (System Time Table)
        xml += `  <STT version="1" system_time="${Math.floor(Date.now() / 1000)}" GPS_UTC_offset="0" daylight_saving="0"/>\n`;

        xml += '</tsduck>\n';
        return xml;
    }

    /**
     * Save PSIP XML to temporary file
     */
    private async savePSIPXML(xml: string): Promise<string> {
        if (typeof window !== 'undefined' && (window as any).electronAPI?.atscSavePSIPXML) {
            const result = await (window as any).electronAPI.atscSavePSIPXML(xml);
            return result.path || '/tmp/psip.xml';
        }
        return '/tmp/psip.xml';
    }

    /**
     * Get input URL for channel
     */
    private getInputUrl(channel: ATSCChannelConfig): string | null {
        switch (channel.inputSource.type) {
            case 'udp':
                return channel.inputSource.url || null;
            case 'srt':
                return channel.inputSource.url || null;
            case 'file':
                return channel.inputSource.filePath || null;
            case 'playplay':
                return channel.inputSource.instanceId ? `http://localhost:8080/api/playout/${channel.inputSource.instanceId}` : null;
            case 'ndi':
                return channel.inputSource.ndiSourceName ? `ndi://${channel.inputSource.ndiSourceName}` : null;
            case 'decklink':
                return channel.inputSource.deckLinkIndex !== undefined ? `decklink://${channel.inputSource.deckLinkIndex}` : null;
            default:
                return null;
        }
    }

    /**
     * Initialize status
     */
    private initializeStatus(): void {
        if (!this.config) return;

        this.status.channels = this.config.channels
            .filter(ch => ch.enabled)
            .map(ch => ({
                channelId: ch.channelId,
                encoding: false,
                bitrate: ch.encoding.videoBitrate,
                frameRate: ch.encoding.framerate,
                errors: [],
            }));

        // Single output for statmux
        if (this.config.output) {
            this.status.outputs = [{
                outputId: generateId(),
                type: this.config.output.type,
                active: false,
            }];
        } else {
            this.status.outputs = [];
        }
    }

    /**
     * Update channel status
     */
    private updateChannelStatus(channelId: number, updates: Partial<ChannelEncodingStatus>): void {
        const channel = this.status.channels.find(ch => ch.channelId === channelId);
        if (channel) {
            Object.assign(channel, updates);
        }
    }

    /**
     * Stop all processes
     */
    public async stop(): Promise<{ success: boolean }> {
        try {
            // Stop encoding processes
            for (const [channelId, processKey] of this.encodingProcesses.entries()) {
                if (processKey.startsWith('obs_branch_')) {
                    // Stop OBS Branch Output filter
                    if (this.obsBranchOutput && this.config) {
                        const channel = this.config.channels.find(ch => ch.channelId === channelId);
                        if (channel) {
                            let sourceName = 'Main View';
                            if (channel.inputSource.type === 'playplay' && channel.inputSource.instanceId) {
                                sourceName = `PlayPlay_${channel.inputSource.instanceId}`;
                            } else if (channel.inputSource.type === 'ndi' && channel.inputSource.ndiSourceName) {
                                sourceName = channel.inputSource.ndiSourceName;
                            }
                            await this.obsBranchOutput.setBranchOutputEnabled(
                                `ATSC_Channel_${channelId}`,
                                sourceName,
                                false
                            );
                        }
                    }
                } else {
                    // Stop FFmpeg encoding
                    if (typeof window !== 'undefined' && (window as any).electronAPI?.atscStopChannelEncoding) {
                        await (window as any).electronAPI.atscStopChannelEncoding(processKey);
                    }
                }
                this.updateChannelStatus(channelId, { encoding: false });
            }
            this.encodingProcesses.clear();

            // Stop branch outputs
            for (const [outputId, processKey] of this.branchProcessKeys.entries()) {
                if (typeof window !== 'undefined' && (window as any).electronAPI?.atscStopBranchOutput) {
                    await (window as any).electronAPI.atscStopBranchOutput(processKey);
                }
            }
            this.branchProcessKeys.clear();

            // Stop mux
            if (this.muxProcessKey) {
                if (typeof window !== 'undefined' && (window as any).electronAPI?.atscStopMux) {
                    await (window as any).electronAPI.atscStopMux(this.muxProcessKey);
                }
                this.muxProcessKey = null;
            }

            this.status.muxActive = false;
            this.status.psipInjected = false;
            this.status.outputs.forEach(output => output.active = false);

            return { success: true };
        } catch (error) {
            console.error('[ATSC Multi-Channel] Stop failed:', error);
            return { success: false };
        }
    }

    /**
     * Get status
     */
    public getStatus(): MultiChannelStatus {
        return { ...this.status };
    }
}

// Singleton instance
let multiChannelInstance: ATSCMultiChannelOutput | null = null;

export function getATSCMultiChannelOutput(api?: any, obsClient?: OBSWebSocketClient): ATSCMultiChannelOutput {
    if (!multiChannelInstance) {
        const electronAPI = api || (typeof window !== 'undefined' ? (window as any).electronAPI : null);
        multiChannelInstance = new ATSCMultiChannelOutput(electronAPI, obsClient);
    } else if (obsClient) {
        multiChannelInstance.setOBSClient(obsClient);
    }
    return multiChannelInstance;
}

