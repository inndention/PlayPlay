/**
 * AUTOMATION STATE MACHINE
 * 
 * The engine exists in exactly ONE state at any moment.
 * State is DERIVED from time + schedule, never stored.
 * 
 * State Diagram:
 * 
 *     ┌──────────────┐
 *     │   STANDBY    │ ← Engine stopped or no schedule
 *     └──────┬───────┘
 *            │ start()
 *            ▼
 *     ┌──────────────┐
 *     │   RUNNING    │ ← Normal operation
 *     └──────┬───────┘
 *            │
 *     ┌──────┴──────┐
 *     │             │
 *     ▼             ▼
 * ┌────────┐   ┌────────┐
 * │ON_AIR  │   │FILLING │
 * │        │   │        │
 * └────┬───┘   └────┬───┘
 *      │            │
 *      ├────────────┤
 *      │            │
 *      ▼            ▼
 * ┌────────┐   ┌────────┐
 * │DRIFTED │   │GAP     │
 * │        │   │        │
 * └────────┘   └────────┘
 * 
 * ON_AIR: Active event, renderer aligned
 * FILLING: No scheduled event, playing fill content
 * DRIFTED: Active event but renderer misaligned
 * GAP: No event, no fill, silence (BUG STATE)
 */

// ============================================
// STATE TYPES
// ============================================

export type EngineMode = 'standby' | 'running';

export type PlaybackState = 
    | 'on_air'      // Playing scheduled content, aligned
    | 'filling'     // Playing fill content
    | 'drifted'     // Playing but misaligned
    | 'prerolling'  // Loading next asset
    | 'transitioning' // Fading between assets
    | 'gap'         // Nothing playing (BUG)
    | 'standby';    // Engine not running

export type TransitionType = 
    | 'cut'         // Instant switch
    | 'fade'        // Crossfade
    | 'dip'         // Dip to black/color
    | 'wipe';       // Future: wipe patterns

export type CorrectionAction =
    | 'none'        // Within tolerance
    | 'soft_seek'   // Small correction, seek
    | 'hard_seek'   // Large correction, seek + flash
    | 'reload'      // Reload asset entirely
    | 'skip'        // Skip to next event
    | 'extend'      // Extend current (for live)
    | 'fill';       // Switch to fill content

// ============================================
// POLICY MATRIX
// ============================================

export interface PolicyConfig {
    // Drift thresholds (ms)
    driftIgnoreThreshold: number;    // Below this: do nothing
    driftSoftSeekThreshold: number;  // Below this: soft seek
    driftHardSeekThreshold: number;  // Below this: hard seek
    driftReloadThreshold: number;    // Above this: reload asset
    
    // Transition defaults
    defaultTransition: TransitionType;
    transitionDuration: number;      // ms
    
    // Timing
    prerollTime: number;             // ms before event to start loading
    minEventDuration: number;        // ms - skip events shorter than this
    
    // Fill behavior
    fillMode: 'slate' | 'loop' | 'random' | 'black';
    fillTransition: TransitionType;
    
    // Recovery
    maxConsecutiveErrors: number;
    errorRecoveryDelay: number;      // ms
    
    // Live handling
    allowLiveExtension: boolean;
    maxLiveExtension: number;        // ms
}

export const DEFAULT_POLICY: PolicyConfig = {
    driftIgnoreThreshold: 50,        // 50ms - imperceptible
    driftSoftSeekThreshold: 500,     // 500ms - soft correction
    driftHardSeekThreshold: 2000,    // 2s - hard correction
    driftReloadThreshold: 10000,     // 10s - something is very wrong
    
    defaultTransition: 'cut',
    transitionDuration: 500,
    
    prerollTime: 5000,               // 5s preroll
    minEventDuration: 1000,          // Skip events < 1s
    
    fillMode: 'slate',
    fillTransition: 'fade',
    
    maxConsecutiveErrors: 3,
    errorRecoveryDelay: 1000,
    
    allowLiveExtension: true,
    maxLiveExtension: 300000,        // 5 min max extension
};

// ============================================
// POLICY DECISION ENGINE
// ============================================

export function determineCorrectionAction(
    drift: number,
    policy: PolicyConfig
): CorrectionAction {
    const absDrift = Math.abs(drift);
    
    if (absDrift <= policy.driftIgnoreThreshold) {
        return 'none';
    }
    
    if (absDrift <= policy.driftSoftSeekThreshold) {
        return 'soft_seek';
    }
    
    if (absDrift <= policy.driftHardSeekThreshold) {
        return 'hard_seek';
    }
    
    if (absDrift <= policy.driftReloadThreshold) {
        return 'reload';
    }
    
    // Drift is catastrophic - skip to next
    return 'skip';
}

/**
 * Decision matrix for event transitions
 */
export interface TransitionDecision {
    action: 'play' | 'skip' | 'fill' | 'extend' | 'wait';
    seekTo: number;           // ms into asset
    transition: TransitionType;
    transitionDuration: number;
    reason: string;
}

export function decideEventTransition(
    now: number,
    eventStart: number,
    eventEnd: number,
    eventDuration: number,
    isLive: boolean,
    policy: PolicyConfig
): TransitionDecision {
    // Guard against malformed schedule data
    if (!Number.isFinite(eventStart) || !Number.isFinite(eventEnd) || !Number.isFinite(eventDuration) || eventDuration <= 0) {
        return {
            action: 'skip',
            seekTo: 0,
            transition: 'cut',
            transitionDuration: 0,
            reason: 'Invalid event data — skipping',
        };
    }

    const timeIntoEvent = now - eventStart;
    const timeRemaining = eventEnd - now;
    
    // Event hasn't started yet
    if (timeIntoEvent < 0) {
        const timeUntil = -timeIntoEvent;
        
        if (timeUntil <= policy.prerollTime) {
            // Within preroll window - prepare but don't play
            return {
                action: 'wait',
                seekTo: 0,
                transition: policy.defaultTransition,
                transitionDuration: policy.transitionDuration,
                reason: `Prerolling, ${timeUntil}ms until start`
            };
        }
        
        // Too early - fill
        return {
            action: 'fill',
            seekTo: 0,
            transition: policy.fillTransition,
            transitionDuration: policy.transitionDuration,
            reason: `Event starts in ${timeUntil}ms`
        };
    }
    
    // Event has ended
    if (timeIntoEvent >= eventDuration) {
        // Live content might extend
        if (isLive && policy.allowLiveExtension) {
            const extension = timeIntoEvent - eventDuration;
            if (extension < policy.maxLiveExtension) {
                return {
                    action: 'extend',
                    seekTo: eventDuration, // Keep at "end"
                    transition: 'cut',
                    transitionDuration: 0,
                    reason: `Live extension: ${extension}ms`
                };
            }
        }
        
        // Event is truly over
        return {
            action: 'skip',
            seekTo: 0,
            transition: policy.defaultTransition,
            transitionDuration: policy.transitionDuration,
            reason: 'Event duration exceeded'
        };
    }
    
    // Event is too short to matter
    if (timeRemaining < policy.minEventDuration) {
        return {
            action: 'skip',
            seekTo: 0,
            transition: 'cut',
            transitionDuration: 0,
            reason: `Only ${timeRemaining}ms remaining, skipping`
        };
    }
    
    // Normal case: play at correct offset
    return {
        action: 'play',
        seekTo: timeIntoEvent,
        transition: timeIntoEvent < 100 ? policy.defaultTransition : 'cut',
        transitionDuration: timeIntoEvent < 100 ? policy.transitionDuration : 0,
        reason: `Playing from offset ${timeIntoEvent}ms`
    };
}

// ============================================
// STATE DERIVATION
// ============================================

export interface DerivedState {
    mode: EngineMode;
    playbackState: PlaybackState;
    activeEventId: string | null;
    expectedPosition: number;     // ms into current media
    actualPosition: number;       // reported by renderer
    drift: number;                // expected - actual
    correctionAction: CorrectionAction;
    nextEventId: string | null;
    timeToNext: number;           // ms
    isPrerolling: boolean;
    transitionDecision: TransitionDecision | null;
    fillActive: boolean;
    gapDetected: boolean;         // BUG condition
    timestamp: number;            // Wall clock of this derivation
}

export function derivePlaybackState(
    hasActiveEvent: boolean,
    hasFillContent: boolean,
    drift: number,
    isTransitioning: boolean,
    policy: PolicyConfig
): PlaybackState {
    if (isTransitioning) {
        return 'transitioning';
    }
    
    if (hasActiveEvent) {
        const absDrift = Math.abs(drift);
        // Only surface 'drifted' when drift is severe enough to need hard intervention.
        // Soft-seek range (< driftHardSeekThreshold) is handled transparently; the UI
        // stays 'on_air' so the operator isn't distracted by minor corrections.
        if (absDrift > policy.driftHardSeekThreshold) {
            return 'drifted';
        }
        return 'on_air';
    }
    
    if (hasFillContent) {
        return 'filling';
    }
    
    // This is a bug - should never happen with proper fill config
    return 'gap';
}

// ============================================
// COMMAND GENERATION
// ============================================

export interface RendererCommand {
    id: string;                   // Unique command ID for tracking
    timestamp: number;            // When issued
    type: 'load' | 'seek' | 'play' | 'pause' | 'stop' | 'fade';
    mediaId?: string;
    mediaSrc?: string;
    seekTo?: number;              // ms
    fadeDirection?: 'in' | 'out';
    fadeDuration?: number;        // ms
    priority: 'normal' | 'urgent' | 'correction';
    reason: string;
}

let commandCounter = 0;
const COMMAND_COUNTER_MAX = 0x7fffffff; // wrap before float precision loss

export function generateCommand(
    type: RendererCommand['type'],
    options: Partial<Omit<RendererCommand, 'id' | 'timestamp' | 'type'>>
): RendererCommand {
    commandCounter = (commandCounter + 1) & COMMAND_COUNTER_MAX;
    return {
        id: `cmd_${Date.now()}_${commandCounter}`,
        timestamp: Date.now(),
        type,
        priority: 'normal',
        reason: 'No reason specified',
        ...options,
    };
}

export function generateCorrectionCommands(
    action: CorrectionAction,
    targetPosition: number,
    mediaId: string,
    mediaSrc: string,
    policy: PolicyConfig
): RendererCommand[] {
    const commands: RendererCommand[] = [];
    
    switch (action) {
        case 'none':
            break;
            
        case 'soft_seek':
            commands.push(generateCommand('seek', {
                seekTo: targetPosition,
                priority: 'correction',
                reason: 'Soft drift correction'
            }));
            break;
            
        case 'hard_seek':
            // Fade out, seek, fade in for smoother correction
            commands.push(generateCommand('fade', {
                fadeDirection: 'out',
                fadeDuration: 100,
                priority: 'urgent',
                reason: 'Hard correction fade out'
            }));
            commands.push(generateCommand('seek', {
                seekTo: targetPosition,
                priority: 'urgent',
                reason: 'Hard drift correction'
            }));
            commands.push(generateCommand('fade', {
                fadeDirection: 'in',
                fadeDuration: 100,
                priority: 'urgent',
                reason: 'Hard correction fade in'
            }));
            break;
            
        case 'reload':
            commands.push(generateCommand('stop', {
                priority: 'urgent',
                reason: 'Reload: stopping current'
            }));
            commands.push(generateCommand('load', {
                mediaId,
                mediaSrc,
                seekTo: targetPosition,
                priority: 'urgent',
                reason: 'Reload: loading fresh'
            }));
            commands.push(generateCommand('play', {
                priority: 'urgent',
                reason: 'Reload: starting playback'
            }));
            break;
            
        case 'skip':
            // Handled by main engine - moves to next event
            break;
            
        case 'fill':
            // Handled by main engine - switches to fill
            break;
    }
    
    return commands;
}

// ============================================
// LOGGING & TELEMETRY
// ============================================

export interface EngineLog {
    timestamp: number;
    level: 'debug' | 'info' | 'warn' | 'error';
    category: 'state' | 'command' | 'drift' | 'transition' | 'error';
    message: string;
    data?: Record<string, unknown>;
}

const logBuffer: EngineLog[] = [];
const MAX_LOG_SIZE = 1000;

export function logEngine(
    level: EngineLog['level'],
    category: EngineLog['category'],
    message: string,
    data?: Record<string, unknown>
) {
    const entry: EngineLog = {
        timestamp: Date.now(),
        level,
        category,
        message,
        data,
    };
    
    logBuffer.push(entry);
    if (logBuffer.length > MAX_LOG_SIZE) {
        logBuffer.shift();
    }
    
    // Also console log for debugging
    const prefix = `[Engine:${category}]`;
    switch (level) {
        case 'debug':
            console.debug(prefix, message, data || '');
            break;
        case 'info':
            console.info(prefix, message, data || '');
            break;
        case 'warn':
            console.warn(prefix, message, data || '');
            break;
        case 'error':
            console.error(prefix, message, data || '');
            break;
    }
}

export function getEngineLogs(
    since?: number,
    category?: EngineLog['category']
): EngineLog[] {
    let logs = logBuffer;
    
    if (since) {
        logs = logs.filter(l => l.timestamp >= since);
    }
    
    if (category) {
        logs = logs.filter(l => l.category === category);
    }
    
    return logs;
}

export function clearEngineLogs() {
    logBuffer.length = 0;
}



