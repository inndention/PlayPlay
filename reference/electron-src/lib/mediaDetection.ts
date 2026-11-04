// ============================================
// MEDIA AUTO-DETECTION UTILITIES
// Auto-detect type, client, and other metadata from filenames
// ============================================

import { MediaType, BumperSubtype, AdClient, ID_PROMO_KEYWORDS } from './types';
import { loadClients } from './storage';

/**
 * Auto-detect media type based on duration and filename
 * Rules tuned for station ingest:
 * - Programs are usually 5+ minutes.
 * - Ads cluster around 30s, 60s, and 2-3 minute paid blocks.
 * - Bumpers usually say station/program ID, bump, open, close, intro, outro.
 * - Promos are station/program loops and teasers; seasonals are tagged separately.
 */
export function detectMediaType(filename: string, durationSeconds: number): MediaType {
    const lowerFilename = filename.toLowerCase();
    const normalized = lowerFilename.replace(/[_\-.]+/g, ' ');
    const hasAny = (words: string[]) => words.some(word => normalized.includes(word));

    if (hasAny(['seasonal', 'holiday', 'christmas', 'thanksgiving', 'halloween', 'summer', 'winter'])) {
        return 'promo';
    }
    
    // Check for ID/promo keywords first
    for (const keyword of ID_PROMO_KEYWORDS) {
        if (lowerFilename.includes(keyword)) {
            // Distinguish between ID and promo
            if (keyword === 'id' || keyword === 'legal' || keyword === 'station' || keyword === 'network') {
                return 'station_id';
            }
            if (keyword === 'promo') {
                return 'promo';
            }
            // Station call signs are usually IDs
            if (['wgsr', 'wmdv', 'wyat', 'star news'].some(k => lowerFilename.includes(k))) {
                return 'station_id';
            }
        }
    }
    
    // Check for bumper keywords
    if (hasAny(['bumper', 'bump', 'program id', 'show id', 'station id', 'open', 'close', 'intro', 'outro'])) {
        return 'bumper';
    }

    if (hasAny(['promo', 'teaser', 'trailer', 'loop', 'sizzle', 'coming up'])) {
        return 'promo';
    }

    if (hasAny(['commercial', 'spot', 'sponsor', 'paid', 'advert', 'ad '])) {
        return 'ad';
    }
    
    // Duration-based detection
    if (durationSeconds > 0) {
        if (durationSeconds >= 300) {
            return 'program';
        }

        // Common ad lengths and paid blocks.
        if (
            (durationSeconds >= 25 && durationSeconds <= 35) ||
            (durationSeconds >= 55 && durationSeconds <= 70) ||
            (durationSeconds >= 110 && durationSeconds <= 190)
        ) {
            return 'ad';
        }

        if (durationSeconds <= 20) {
            return 'station_id';
        }

        if (durationSeconds < 120) {
            return 'bumper';
        }

        if (durationSeconds < 300) {
            return 'promo';
        }
    }
    
    // Default to program for longer content
    return 'program';
}

/**
 * Auto-detect client from filename by matching against known client names
 * Returns client ID if found, undefined otherwise
 */
export function detectClientFromFilename(filename: string, clients?: AdClient[]): string | undefined {
    const clientList = clients || loadClients();
    const lowerFilename = filename.toLowerCase();
    
    // Try to match client names
    for (const client of clientList) {
        const clientName = client.name.toLowerCase();
        
        // Exact match or contained in filename
        if (lowerFilename.includes(clientName)) {
            return client.id;
        }
        
        // Try matching without spaces/special chars
        const normalizedClient = clientName.replace(/[^a-z0-9]/g, '');
        const normalizedFilename = lowerFilename.replace(/[^a-z0-9]/g, '');
        if (normalizedFilename.includes(normalizedClient) && normalizedClient.length >= 3) {
            return client.id;
        }
    }
    
    return undefined;
}

/**
 * Detect bumper subtype from filename
 */
export function detectBumperSubtype(filename: string): BumperSubtype {
    const lower = filename.toLowerCase();
    
    if (lower.includes('breaking') || lower.includes('alert') || lower.includes('urgent')) {
        return 'breaking';
    }
    
    if (lower.includes('program') || lower.includes('show') || lower.includes('series')) {
        return 'program';
    }
    
    return 'generic';
}

/**
 * Parse a filename to extract potential metadata
 */
export function parseFilename(filename: string): {
    cleanTitle: string;
    possibleDate: string | undefined;
    possibleClient: string | undefined;
    keywords: string[];
} {
    // Remove extension
    let name = filename.replace(/\.[^/.]+$/, '');
    
    // Replace underscores and hyphens with spaces
    name = name.replace(/[_-]/g, ' ');
    
    // Extract date patterns (YYYY-MM-DD, MM-DD-YYYY, etc.)
    const datePatterns = [
        /(\d{4})[.\-\/](\d{1,2})[.\-\/](\d{1,2})/,  // YYYY-MM-DD
        /(\d{1,2})[.\-\/](\d{1,2})[.\-\/](\d{4})/,  // MM-DD-YYYY
        /(\d{8})/,  // YYYYMMDD
    ];
    
    let possibleDate: string | undefined;
    for (const pattern of datePatterns) {
        const match = name.match(pattern);
        if (match) {
            // Try to parse as date
            try {
                if (match[0].length === 8) {
                    // YYYYMMDD format
                    const year = match[0].substring(0, 4);
                    const month = match[0].substring(4, 6);
                    const day = match[0].substring(6, 8);
                    possibleDate = `${year}-${month}-${day}`;
                } else {
                    possibleDate = match[0];
                }
                name = name.replace(pattern, '');
            } catch {
                // Not a valid date
            }
        }
    }
    
    // Extract keywords
    const keywords = name.toLowerCase().split(/\s+/).filter(w => w.length > 2);
    
    // Clean up title
    const cleanTitle = name.replace(/\s+/g, ' ').trim();
    
    return {
        cleanTitle: cleanTitle || filename,
        possibleDate,
        possibleClient: undefined, // Filled in by detectClientFromFilename
        keywords,
    };
}

/**
 * Comprehensive auto-detection for a new media item
 */
export function autoDetectMediaMetadata(
    filename: string,
    durationSeconds: number,
    clients?: AdClient[]
): {
    type: MediaType;
    client?: string;
    bumperSubtype?: BumperSubtype;
    title: string;
    episodeDate?: string;
    mediaClass?: 'program' | 'ad' | 'bumper' | 'promo' | 'seasonal' | 'station_id' | 'filler' | 'unknown';
    detectionNotes: string[];
} {
    const parsed = parseFilename(filename);
    const type = detectMediaType(filename, durationSeconds);
    const client = detectClientFromFilename(filename, clients);
    const lower = filename.toLowerCase();
    const detectionNotes: string[] = [];

    if (durationSeconds >= 300) detectionNotes.push('duration-over-5-minutes');
    if (lower.endsWith('.mpg') || lower.endsWith('.mpeg')) detectionNotes.push('mpg-treated-as-mp4-container');
    if (lower.includes('seasonal') || lower.includes('holiday')) detectionNotes.push('seasonal-title-keyword');
    
    let bumperSubtype: BumperSubtype | undefined;
    if (type === 'bumper') {
        bumperSubtype = detectBumperSubtype(filename);
    }

    const mediaClass = (() => {
        if (lower.includes('seasonal') || lower.includes('holiday')) return 'seasonal';
        if (type === 'station_id' || type === 'bumper' || type === 'program' || type === 'ad' || type === 'promo') return type;
        return 'unknown';
    })();
    
    return {
        type,
        client: type === 'ad' ? client : undefined,
        bumperSubtype,
        title: parsed.cleanTitle,
        episodeDate: parsed.possibleDate,
        mediaClass,
        detectionNotes,
    };
}

/**
 * Suggest a type based on duration alone
 */
export function suggestTypeFromDuration(durationSeconds: number): MediaType {
    if (durationSeconds <= 20) return 'station_id';
    if (durationSeconds <= 120) return 'bumper';
    if ((durationSeconds >= 25 && durationSeconds <= 35) || (durationSeconds >= 55 && durationSeconds <= 70) || (durationSeconds >= 110 && durationSeconds <= 190)) return 'ad';
    if (durationSeconds < 300) return 'promo';
    return 'program';
}





