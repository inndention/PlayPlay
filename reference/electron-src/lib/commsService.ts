/**
 * COMMS Service - Push-to-talk and chat communication
 * 
 * Features:
 * - Push-to-talk audio (studio-to-studio, studio-to-field, field-to-studio)
 * - Text chat channels
 * - Countdown requests
 * - Tally/status indicators
 */

import { getWebRTCSessionManager, WebRTCSession } from './webrtcSessionManager';

export interface CommsChannel {
    id: string;
    name: string;
    type: 'studio' | 'field' | 'reporter' | 'guest';
    participants: string[]; // Session IDs
    isActive: boolean;
}

export interface ChatMessage {
    id: string;
    channelId: string;
    senderId: string;
    senderName: string;
    message: string;
    timestamp: Date;
    type: 'text' | 'system' | 'countdown-request';
    countdownSeconds?: number;
}

export interface PTTSession {
    id: string;
    fromSessionId: string;
    toSessionId: string;
    isActive: boolean;
    startTime?: Date;
}

export class CommsService {
    private channels: Map<string, CommsChannel> = new Map();
    private chatMessages: Map<string, ChatMessage[]> = new Map(); // channelId -> messages
    private pttSessions: Map<string, PTTSession> = new Map();
    private audioContext: AudioContext | null = null;
    private mediaStream: MediaStream | null = null;
    private dataChannels: Map<string, RTCDataChannel> = new Map();
    private onMessageCallback?: (message: ChatMessage) => void;
    private onPTTStateChange?: (sessionId: string, isActive: boolean) => void;

    constructor() {
        // Initialize audio context when needed
    }

    /**
     * Initialize audio context
     */
    private async initAudioContext(): Promise<AudioContext> {
        if (!this.audioContext) {
            this.audioContext = new (window.AudioContext || (window as any).webkitAudioContext)();
        }
        return this.audioContext;
    }

    /**
     * Create a communication channel
     */
    createChannel(name: string, type: CommsChannel['type']): CommsChannel {
        const channel: CommsChannel = {
            id: `channel_${Date.now()}`,
            name,
            type,
            participants: [],
            isActive: true,
        };
        this.channels.set(channel.id, channel);
        this.chatMessages.set(channel.id, []);
        return channel;
    }

    /**
     * Join a channel
     */
    joinChannel(channelId: string, sessionId: string): void {
        const channel = this.channels.get(channelId);
        if (channel && !channel.participants.includes(sessionId)) {
            channel.participants.push(sessionId);
        }
    }

    /**
     * Leave a channel
     */
    leaveChannel(channelId: string, sessionId: string): void {
        const channel = this.channels.get(channelId);
        if (channel) {
            channel.participants = channel.participants.filter(id => id !== sessionId);
        }
    }

    /**
     * Send a chat message
     */
    sendChatMessage(channelId: string, sessionId: string, message: string): void {
        const session = getWebRTCSessionManager().getSession(sessionId);
        if (!session) return;

        const chatMessage: ChatMessage = {
            id: `msg_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`,
            channelId,
            senderId: sessionId,
            senderName: session.name,
            message,
            timestamp: new Date(),
            type: 'text',
        };

        const messages = this.chatMessages.get(channelId) || [];
        messages.push(chatMessage);
        this.chatMessages.set(channelId, messages);

        // Send via data channel if available
        this.broadcastToChannel(channelId, {
            type: 'chat-message',
            data: chatMessage,
        });

        this.onMessageCallback?.(chatMessage);
    }

    /**
     * Request countdown
     */
    requestCountdown(channelId: string, sessionId: string, seconds: number): void {
        const session = getWebRTCSessionManager().getSession(sessionId);
        if (!session) return;

        const message: ChatMessage = {
            id: `countdown_${Date.now()}`,
            channelId,
            senderId: sessionId,
            senderName: session.name,
            message: `Countdown request: ${seconds} seconds`,
            timestamp: new Date(),
            type: 'countdown-request',
            countdownSeconds: seconds,
        };

        const messages = this.chatMessages.get(channelId) || [];
        messages.push(message);
        this.chatMessages.set(channelId, messages);

        this.broadcastToChannel(channelId, {
            type: 'countdown-request',
            data: message,
        });

        this.onMessageCallback?.(message);
    }

    /**
     * Start push-to-talk
     */
    async startPTT(fromSessionId: string, toSessionId: string): Promise<void> {
        const fromSession = getWebRTCSessionManager().getSession(fromSessionId);
        const toSession = getWebRTCSessionManager().getSession(toSessionId);

        if (!fromSession || !toSession) {
            throw new Error('Sessions not found');
        }

        // Get user media
        if (!this.mediaStream) {
            this.mediaStream = await navigator.mediaDevices.getUserMedia({
                audio: {
                    echoCancellation: true,
                    noiseSuppression: true,
                    autoGainControl: true,
                },
            });
        }

        // Set up data channel for PTT signaling
        const dataChannel = fromSession.peerConnection.createDataChannel('ptt', {
            ordered: true,
        });

        dataChannel.onopen = () => {
            const pttSession: PTTSession = {
                id: `ptt_${Date.now()}`,
                fromSessionId,
                toSessionId,
                isActive: true,
                startTime: new Date(),
            };
            this.pttSessions.set(pttSession.id, pttSession);
            this.onPTTStateChange?.(fromSessionId, true);

            // Send audio stream
            this.mediaStream?.getAudioTracks().forEach(track => {
                fromSession.peerConnection.addTrack(track, this.mediaStream!);
            });
        };

        dataChannel.onclose = () => {
            this.stopPTT(fromSessionId);
        };

        this.dataChannels.set(fromSessionId, dataChannel);
    }

    /**
     * Stop push-to-talk
     */
    stopPTT(sessionId: string): void {
        const pttSession = Array.from(this.pttSessions.values()).find(
            s => s.fromSessionId === sessionId && s.isActive
        );

        if (pttSession) {
            pttSession.isActive = false;
            this.onPTTStateChange?.(sessionId, false);

            // Stop audio tracks
            this.mediaStream?.getAudioTracks().forEach(track => {
                track.stop();
            });
            this.mediaStream = null;

            // Close data channel
            const dataChannel = this.dataChannels.get(sessionId);
            if (dataChannel) {
                dataChannel.close();
                this.dataChannels.delete(sessionId);
            }
        }
    }

    /**
     * Broadcast message to channel participants
     */
    private broadcastToChannel(channelId: string, message: any): void {
        const channel = this.channels.get(channelId);
        if (!channel) return;

        channel.participants.forEach(sessionId => {
            const dataChannel = this.dataChannels.get(sessionId);
            if (dataChannel && dataChannel.readyState === 'open') {
                dataChannel.send(JSON.stringify(message));
            }
        });
    }

    /**
     * Get messages for a channel
     */
    getChannelMessages(channelId: string): ChatMessage[] {
        return this.chatMessages.get(channelId) || [];
    }

    /**
     * Get all channels
     */
    getAllChannels(): CommsChannel[] {
        return Array.from(this.channels.values());
    }

    /**
     * Get channel by ID
     */
    getChannel(channelId: string): CommsChannel | undefined {
        return this.channels.get(channelId);
    }

    /**
     * Check if PTT is active for a session
     */
    isPTTActive(sessionId: string): boolean {
        return Array.from(this.pttSessions.values()).some(
            s => s.fromSessionId === sessionId && s.isActive
        );
    }

    /**
     * Set callback for new messages
     */
    onMessage(callback: (message: ChatMessage) => void): void {
        this.onMessageCallback = callback;
    }

    /**
     * Set callback for PTT state changes
     */
    onPTTStateChangeCallback(callback: (sessionId: string, isActive: boolean) => void): void {
        this.onPTTStateChange = callback;
    }

    /**
     * Cleanup
     */
    cleanup(): void {
        // Stop all PTT sessions
        Array.from(this.pttSessions.keys()).forEach(sessionId => {
            this.stopPTT(sessionId);
        });

        // Close all data channels
        this.dataChannels.forEach(channel => channel.close());
        this.dataChannels.clear();

        // Stop media stream
        this.mediaStream?.getTracks().forEach(track => track.stop());
        this.mediaStream = null;

        // Close audio context
        if (this.audioContext) {
            this.audioContext.close();
            this.audioContext = null;
        }
    }
}

// Singleton instance
let commsInstance: CommsService | null = null;

export function getCommsService(): CommsService {
    if (!commsInstance) {
        commsInstance = new CommsService();
    }
    return commsInstance;
}
