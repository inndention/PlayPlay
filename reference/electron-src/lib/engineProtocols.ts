// ============================================
// Engine Protocol Adapters
// Unified playlist translation for playout
// ============================================

import { PlaylistItem, MediaItem } from './types';
import { resolveMediaUrlSync } from './fileResolver';

export interface EngineProtocolAdapter {
    /**
     * Convert a PlaylistItem to the engine's native format
     */
    convertItem(item: PlaylistItem): any;

    /**
     * Convert a full playlist to the engine's native format
     */
    convertPlaylist(items: PlaylistItem[]): any;

    /**
     * Validate that an item can be played on this engine
     */
    validateItem(item: PlaylistItem): { valid: boolean; error?: string };
}

// ============================================
// Internal (HTML5) Protocol Adapter
// ============================================

export class InternalProtocolAdapter implements EngineProtocolAdapter {
    convertItem(item: PlaylistItem): { src: string; startTime?: number; endTime?: number } {
        const src = resolveMediaUrlSync(item.media);
        
        return {
            src,
            startTime: item.timeIn,
            endTime: item.timeOut,
        };
    }

    convertPlaylist(items: PlaylistItem[]): Array<{ src: string; startTime?: number; endTime?: number }> {
        return items.map(item => this.convertItem(item));
    }

    validateItem(item: PlaylistItem): { valid: boolean; error?: string } {
        if (!item.media) {
            return { valid: false, error: 'No media attached to item' };
        }
        
        if (!item.media.src && !item.media.filePath) {
            return { valid: false, error: 'No source path for media' };
        }
        
        // Check for supported types
        const supportedTypes = ['video', 'image', 'program', 'ad', 'station_id', 'bumper', 'promo', 'paid_program'];
        if (!supportedTypes.includes(item.media.type)) {
            return { valid: false, error: `Unsupported media type: ${item.media.type}` };
        }
        
        return { valid: true };
    }
}

// ============================================
// Protocol Adapter Factory
// ============================================

const adapters = new Map<string, EngineProtocolAdapter>();

export function getProtocolAdapter(engineType: string): EngineProtocolAdapter {
    if (!adapters.has(engineType)) {
        // All engines use the internal adapter now
        adapters.set(engineType, new InternalProtocolAdapter());
    }
    return adapters.get(engineType)!;
}

/**
 * Translate a playlist to the target engine format
 */
export function translatePlaylistToEngine(
    items: PlaylistItem[],
    engineType: string
): any[] {
    const adapter = getProtocolAdapter(engineType);
    return items.map(item => adapter.convertItem(item));
}

/**
 * Validate all items in a playlist for a given engine
 */
export function validatePlaylistForEngine(
    items: PlaylistItem[],
    engineType: string
): Array<{ item: PlaylistItem; valid: boolean; error?: string }> {
    const adapter = getProtocolAdapter(engineType);
    return items.map(item => ({
        item,
        ...adapter.validateItem(item),
    }));
}
