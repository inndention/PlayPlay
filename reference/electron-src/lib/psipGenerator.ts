// ============================================
// PSIP Generator
// Generates PSIP EIT events from playlist items
// ============================================

import { PlaylistItem } from './types';

export interface PSIPEvent {
    eventId: number;
    startTime: number; // Unix timestamp
    lengthInSeconds: number;
    title: string;
    description?: string;
    rating?: string;
    contentAdvisory?: string;
}

export interface PSIPChannel {
    shortName: string;
    majorChannelNumber: number;
    minorChannelNumber: number;
    modulationMode: number;
    carrierFrequency: number;
    channelTSID: number;
    programNumber: number;
    serviceType: number;
    sourceId: number;
}

/**
 * Generate PSIP EIT events from playlist
 */
export function generateEITFromPlaylist(
    playlist: PlaylistItem[],
    scheduleStartTime: number, // Unix timestamp when schedule starts
    channelName: string = 'PLAY PLAY'
): PSIPEvent[] {
    const events: PSIPEvent[] = [];
    let currentTime = scheduleStartTime;
    let eventId = 1;

    for (const item of playlist) {
        if (item.media.type === 'gap') {
            // Skip gaps or create filler event
            currentTime += item.media.duration * 1000;
            continue;
        }

        // Calculate effective duration (with time in/out)
        const effectiveDuration = item.timeOut && item.timeIn
            ? item.timeOut - item.timeIn
            : item.timeOut
                ? item.timeOut - (item.timeIn || 0)
                : item.timeIn
                    ? item.media.duration - item.timeIn
                    : item.media.duration;

        // Use fixed time if set, otherwise use calculated time
        const startTime = item.isFixedTime && item.startTime
            ? item.startTime
            : currentTime;

        const event: PSIPEvent = {
            eventId: eventId++,
            startTime: Math.floor(startTime / 1000), // Convert to Unix timestamp
            lengthInSeconds: Math.floor(effectiveDuration),
            title: item.media.title,
            description: generateDescription(item),
            contentAdvisory: getContentAdvisory(item.media.type),
        };

        events.push(event);

        // Advance time
        if (item.isFixedTime && item.startTime) {
            currentTime = item.startTime + (effectiveDuration * 1000);
        } else {
            currentTime += effectiveDuration * 1000;
        }
    }

    return events;
}

/**
 * Generate description for EIT event
 */
function generateDescription(item: PlaylistItem): string {
    const parts: string[] = [];

    if (item.media.client) {
        parts.push(`Client: ${item.media.client}`);
    }

    if (item.media.programSeries) {
        parts.push(`Series: ${item.media.programSeries}`);
    }

    if (item.media.episodeDate) {
        parts.push(`Aired: ${new Date(item.media.episodeDate).toLocaleDateString()}`);
    }

    if (item.timeIn !== undefined || item.timeOut !== undefined) {
        const inTime = item.timeIn !== undefined ? formatTime(item.timeIn) : 'start';
        const outTime = item.timeOut !== undefined ? formatTime(item.timeOut) : 'end';
        parts.push(`Segment: ${inTime} - ${outTime}`);
    }

    if (item.media.notes) {
        parts.push(item.media.notes);
    }

    return parts.join(' • ') || item.media.title;
}

/**
 * Format time in seconds to HH:MM:SS
 */
function formatTime(seconds: number): string {
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = Math.floor(seconds % 60);
    return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
}

/**
 * Get content advisory rating based on media type
 */
function getContentAdvisory(type: string): string {
    switch (type) {
        case 'ad':
            return 'Commercial';
        case 'promo':
            return 'Promotional';
        case 'station_id':
            return 'Station ID';
        case 'bumper':
            return 'Bumper';
        case 'program':
            return 'Program';
        case 'paid_program':
            return 'Paid Program';
        default:
            return 'General';
    }
}

/**
 * Generate VCT channel from configuration
 */
export function generateVCTChannel(config: {
    shortName: string;
    majorChannel: number;
    minorChannel: number;
    carrierFrequency: number; // Hz
    channelTSID: number;
    programNumber: number;
    sourceId: number;
}): PSIPChannel {
    return {
        shortName: config.shortName.substring(0, 7).padEnd(7, ' '),
        majorChannelNumber: config.majorChannel,
        minorChannelNumber: config.minorChannel,
        modulationMode: 0x04, // 8-VSB for ATSC terrestrial
        carrierFrequency: config.carrierFrequency,
        channelTSID: config.channelTSID,
        programNumber: config.programNumber,
        serviceType: 0x02, // ATSC digital television
        sourceId: config.sourceId,
    };
}

/**
 * Update PSIP EIT in real-time as playlist changes
 */
export class PSIPManager {
    private currentEvents: PSIPEvent[] = [];
    private updateCallback?: (events: PSIPEvent[]) => void;

    constructor() {
        // Auto-update will be handled by playlist changes
    }

    /**
     * Update EIT from playlist
     */
    updateFromPlaylist(
        playlist: PlaylistItem[],
        scheduleStartTime: number,
        channelName?: string
    ): PSIPEvent[] {
        this.currentEvents = generateEITFromPlaylist(playlist, scheduleStartTime, channelName);
        
        if (this.updateCallback) {
            this.updateCallback(this.currentEvents);
        }

        return this.currentEvents;
    }

    /**
     * Get current EIT events
     */
    getCurrentEvents(): PSIPEvent[] {
        return this.currentEvents;
    }

    /**
     * Set callback for EIT updates
     */
    onUpdate(callback: (events: PSIPEvent[]) => void): () => void {
        this.updateCallback = callback;
        return () => {
            this.updateCallback = undefined;
        };
    }
}

export const psipManager = new PSIPManager();






