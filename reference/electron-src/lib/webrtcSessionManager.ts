/**
 * WebRTC Session Manager
 * Handles WebRTC peer connections for remote reporters and guests
 * Supports dual-bitrate streaming (low for multiview, high for simulcast)
 */

export interface WebRTCSession {
    id: string;
    name: string;
    type: 'reporter' | 'guest';
    peerConnection: RTCPeerConnection;
    localStream: MediaStream | null;
    remoteStream: MediaStream | null;
    lowBitrateStream: MediaStream | null; // For multiview preview
    highBitrateStream: MediaStream | null; // For simulcast/air
    status: 'connecting' | 'connected' | 'disconnected' | 'error';
    isLive: boolean;
    isRecording: boolean;
    recordingSegments: RecordingSegment[];
    createdAt: Date;
    lastActivity: Date;
}

export interface RecordingSegment {
    id: string;
    startTime: Date;
    endTime?: Date;
    duration: number;
    filePath?: string;
    status: 'recording' | 'completed' | 'failed';
}

export interface WebRTCConfig {
    signalingServerUrl: string;
    iceServers: RTCIceServer[];
    lowBitrateConstraints: MediaStreamConstraints;
    highBitrateConstraints: MediaStreamConstraints;
    autoRecord: boolean;
    segmentDuration: number; // seconds
}

const DEFAULT_CONFIG: WebRTCConfig = {
    signalingServerUrl: 'ws://localhost:8080/ws',
    iceServers: [
        { urls: 'stun:stun.l.google.com:19302' },
        { urls: 'stun:stun1.l.google.com:19302' },
    ],
    lowBitrateConstraints: {
        video: {
            width: { ideal: 640 },
            height: { ideal: 360 },
            frameRate: { ideal: 15 },
        },
        audio: {
            sampleRate: 16000,
            channelCount: 1,
        },
    },
    highBitrateConstraints: {
        video: {
            width: { ideal: 1920 },
            height: { ideal: 1080 },
            frameRate: { ideal: 30 },
        },
        audio: {
            sampleRate: 48000,
            channelCount: 2,
        },
    },
    autoRecord: false,
    segmentDuration: 300, // 5 minutes
};

export class WebRTCSessionManager {
    private sessions: Map<string, WebRTCSession> = new Map();
    private signalingSocket: WebSocket | null = null;
    private config: WebRTCConfig;
    private onSessionUpdate?: (session: WebRTCSession) => void;
    private onRemoteStream?: (sessionId: string, stream: MediaStream) => void;

    constructor(config?: Partial<WebRTCConfig>) {
        this.config = { ...DEFAULT_CONFIG, ...config };
    }

    /**
     * Initialize signaling connection
     */
    async connectSignaling(): Promise<void> {
        return new Promise((resolve, reject) => {
            try {
                this.signalingSocket = new WebSocket(this.config.signalingServerUrl);

                this.signalingSocket.onopen = () => {
                    console.log('[WebRTC] Signaling connected');
                    resolve();
                };

                this.signalingSocket.onerror = (error) => {
                    console.error('[WebRTC] Signaling error:', error);
                    reject(error);
                };

                this.signalingSocket.onmessage = (event) => {
                    this.handleSignalingMessage(JSON.parse(event.data));
                };

                this.signalingSocket.onclose = () => {
                    console.log('[WebRTC] Signaling disconnected');
                    // Attempt reconnect
                    setTimeout(() => this.connectSignaling(), 3000);
                };
            } catch (error) {
                reject(error);
            }
        });
    }

    /**
     * Create a new session (as studio/host)
     */
    async createSession(name: string, type: 'reporter' | 'guest'): Promise<WebRTCSession> {
        const sessionId = `session_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
        
        const peerConnection = new RTCPeerConnection({
            iceServers: this.config.iceServers,
        });

        const session: WebRTCSession = {
            id: sessionId,
            name,
            type,
            peerConnection,
            localStream: null,
            remoteStream: null,
            lowBitrateStream: null,
            highBitrateStream: null,
            status: 'connecting',
            isLive: false,
            isRecording: false,
            recordingSegments: [],
            createdAt: new Date(),
            lastActivity: new Date(),
        };

        // Handle incoming stream
        peerConnection.ontrack = (event) => {
            const stream = event.streams[0];
            session.remoteStream = stream;
            session.status = 'connected';
            session.lastActivity = new Date();
            this.onRemoteStream?.(sessionId, stream);
            this.updateSession(session);
        };

        // Handle ICE candidates
        peerConnection.onicecandidate = (event) => {
            if (event.candidate && this.signalingSocket?.readyState === WebSocket.OPEN) {
                this.sendSignalingMessage({
                    type: 'ice-candidate',
                    sessionId,
                    candidate: event.candidate,
                });
            }
        };

        // Handle connection state changes
        peerConnection.onconnectionstatechange = () => {
            const state = peerConnection.connectionState;
            if (state === 'connected') {
                session.status = 'connected';
            } else if (state === 'disconnected' || state === 'failed') {
                session.status = 'disconnected';
            } else if (state === 'closed') {
                session.status = 'disconnected';
            }
            this.updateSession(session);
        };

        this.sessions.set(sessionId, session);
        return session;
    }

    /**
     * Join a session (as remote client)
     */
    async joinSession(sessionId: string, name: string, type: 'reporter' | 'guest'): Promise<WebRTCSession> {
        // Get user media for both bitrates
        const lowBitrateStream = await navigator.mediaDevices.getUserMedia(this.config.lowBitrateConstraints);
        const highBitrateStream = await navigator.mediaDevices.getUserMedia(this.config.highBitrateConstraints);

        const peerConnection = new RTCPeerConnection({
            iceServers: this.config.iceServers,
        });

        // Add tracks to peer connection
        lowBitrateStream.getTracks().forEach(track => {
            peerConnection.addTrack(track, lowBitrateStream);
        });

        highBitrateStream.getTracks().forEach(track => {
            peerConnection.addTrack(track, highBitrateStream);
        });

        const session: WebRTCSession = {
            id: sessionId,
            name,
            type,
            peerConnection,
            localStream: highBitrateStream,
            remoteStream: null,
            lowBitrateStream,
            highBitrateStream,
            status: 'connecting',
            isLive: false,
            isRecording: false,
            recordingSegments: [],
            createdAt: new Date(),
            lastActivity: new Date(),
        };

        // Create offer
        const offer = await peerConnection.createOffer();
        await peerConnection.setLocalDescription(offer);

        // Send offer via signaling
        this.sendSignalingMessage({
            type: 'offer',
            sessionId,
            offer,
        });

        // Handle ICE candidates
        peerConnection.onicecandidate = (event) => {
            if (event.candidate && this.signalingSocket?.readyState === WebSocket.OPEN) {
                this.sendSignalingMessage({
                    type: 'ice-candidate',
                    sessionId,
                    candidate: event.candidate,
                });
            }
        };

        // Handle answer
        this.sessions.set(sessionId, session);
        return session;
    }

    /**
     * Handle signaling messages
     */
    private handleSignalingMessage(message: any): void {
        const { type, sessionId, offer, answer, candidate } = message;
        const session = this.sessions.get(sessionId);

        if (!session) {
            console.warn(`[WebRTC] Session ${sessionId} not found`);
            return;
        }

        switch (type) {
            case 'offer':
                this.handleOffer(session, offer);
                break;
            case 'answer':
                this.handleAnswer(session, answer);
                break;
            case 'ice-candidate':
                if (candidate) {
                    session.peerConnection.addIceCandidate(new RTCIceCandidate(candidate));
                }
                break;
        }
    }

    /**
     * Handle incoming offer
     */
    private async handleOffer(session: WebRTCSession, offer: RTCSessionDescriptionInit): Promise<void> {
        await session.peerConnection.setRemoteDescription(new RTCSessionDescription(offer));
        const answer = await session.peerConnection.createAnswer();
        await session.peerConnection.setLocalDescription(answer);

        this.sendSignalingMessage({
            type: 'answer',
            sessionId: session.id,
            answer,
        });
    }

    /**
     * Handle incoming answer
     */
    private async handleAnswer(session: WebRTCSession, answer: RTCSessionDescriptionInit): Promise<void> {
        await session.peerConnection.setRemoteDescription(new RTCSessionDescription(answer));
    }

    /**
     * Send signaling message
     */
    private sendSignalingMessage(message: any): void {
        if (this.signalingSocket?.readyState === WebSocket.OPEN) {
            this.signalingSocket.send(JSON.stringify(message));
        } else {
            console.warn('[WebRTC] Signaling socket not open');
        }
    }

    /**
     * Start recording with automatic segmentation
     */
    async startRecording(sessionId: string): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session || !session.remoteStream) {
            throw new Error('Session not found or no stream available');
        }

        session.isRecording = true;
        const segment: RecordingSegment = {
            id: `segment_${Date.now()}`,
            startTime: new Date(),
            duration: 0,
            status: 'recording',
        };
        session.recordingSegments.push(segment);
        this.updateSession(session);

        // Auto-segment based on duration
        if (this.config.autoRecord && this.config.segmentDuration > 0) {
            setTimeout(() => {
                this.endRecordingSegment(sessionId, segment.id);
                if (session.isRecording) {
                    this.startRecording(sessionId);
                }
            }, this.config.segmentDuration * 1000);
        }
    }

    /**
     * End current recording segment
     */
    async endRecordingSegment(sessionId: string, segmentId: string): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;

        const segment = session.recordingSegments.find(s => s.id === segmentId);
        if (!segment) return;

        segment.endTime = new Date();
        segment.duration = (segment.endTime.getTime() - segment.startTime.getTime()) / 1000;
        segment.status = 'completed';
        this.updateSession(session);
    }

    /**
     * Stop recording
     */
    async stopRecording(sessionId: string): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;

        session.isRecording = false;
        const activeSegment = session.recordingSegments.find(s => s.status === 'recording');
        if (activeSegment) {
            await this.endRecordingSegment(sessionId, activeSegment.id);
        }
        this.updateSession(session);
    }

    /**
     * Get session by ID
     */
    getSession(sessionId: string): WebRTCSession | undefined {
        return this.sessions.get(sessionId);
    }

    /**
     * Get all sessions
     */
    getAllSessions(): WebRTCSession[] {
        return Array.from(this.sessions.values());
    }

    /**
     * Close a session
     */
    async closeSession(sessionId: string): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;

        // Stop all tracks
        session.localStream?.getTracks().forEach(track => track.stop());
        session.remoteStream?.getTracks().forEach(track => track.stop());
        session.lowBitrateStream?.getTracks().forEach(track => track.stop());
        session.highBitrateStream?.getTracks().forEach(track => track.stop());

        // Close peer connection
        session.peerConnection.close();

        // Remove from map
        this.sessions.delete(sessionId);
    }

    /**
     * Update session and notify listeners
     */
    private updateSession(session: WebRTCSession): void {
        this.sessions.set(session.id, session);
        this.onSessionUpdate?.(session);
    }

    /**
     * Set callback for session updates
     */
    onSessionUpdateCallback(callback: (session: WebRTCSession) => void): void {
        this.onSessionUpdate = callback;
    }

    /**
     * Set callback for remote stream
     */
    onRemoteStreamCallback(callback: (sessionId: string, stream: MediaStream) => void): void {
        this.onRemoteStream = callback;
    }

    /**
     * Disconnect all sessions
     */
    async disconnectAll(): Promise<void> {
        const promises = Array.from(this.sessions.keys()).map(id => this.closeSession(id));
        await Promise.all(promises);

        if (this.signalingSocket) {
            this.signalingSocket.close();
            this.signalingSocket = null;
        }
    }
}

// Singleton instance
let sessionManagerInstance: WebRTCSessionManager | null = null;

export function getWebRTCSessionManager(): WebRTCSessionManager {
    if (!sessionManagerInstance) {
        sessionManagerInstance = new WebRTCSessionManager();
    }
    return sessionManagerInstance;
}
