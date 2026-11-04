/**
 * RemoteSessionPanel - Manage WebRTC remote sessions
 * 
 * Features:
 * - Display active remote sessions (reporters/guests)
 * - Dual-bitrate streaming (low for multiview, high for simulcast)
 * - Start Facebook Live from remote session
 * - Automatic recording with segmentation
 * - Session controls and monitoring
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './RemoteSessionPanel.module.css';
import {
    Video, VideoOff, Mic, MicOff, Radio, Square, Play, Pause,
    Settings, X, RefreshCw, Eye, EyeOff, Download, Upload,
    Users, Wifi, WifiOff, AlertCircle, CheckCircle, Clock,
    Zap, Film, HardDrive, BarChart2
} from 'lucide-react';
import {
    getWebRTCSessionManager,
    WebRTCSession,
    RecordingSegment,
} from '../lib/webrtcSessionManager';
import { createFacebookLiveVideo } from '../lib/facebookLiveProducer';
import { formatDuration } from '../lib/utils';

interface RemoteSessionPanelProps {
    onSessionSelect?: (session: WebRTCSession) => void;
    onStreamReady?: (sessionId: string, stream: MediaStream, bitrate: 'low' | 'high') => void;
}

export const RemoteSessionPanel: React.FC<RemoteSessionPanelProps> = ({
    onSessionSelect,
    onStreamReady,
}) => {
    const [sessions, setSessions] = useState<WebRTCSession[]>([]);
    const [selectedSession, setSelectedSession] = useState<WebRTCSession | null>(null);
    const [isConnecting, setIsConnecting] = useState(false);
    const sessionManager = getWebRTCSessionManager();
    const videoRefs = useRef<Map<string, HTMLVideoElement>>(new Map());

    // Initialize session manager and listen for updates
    useEffect(() => {
        const handleSessionUpdate = (session: WebRTCSession) => {
            setSessions(prev => {
                const index = prev.findIndex(s => s.id === session.id);
                if (index >= 0) {
                    const updated = [...prev];
                    updated[index] = session;
                    return updated;
                } else {
                    return [...prev, session];
                }
            });
        };

        sessionManager.onSessionUpdateCallback(handleSessionUpdate);

        // Connect signaling
        sessionManager.connectSignaling().catch(err => {
            console.error('[RemoteSessionPanel] Failed to connect signaling:', err);
        });

        // Load existing sessions
        setSessions(sessionManager.getAllSessions());

        return () => {
            // Cleanup handled by session manager
        };
    }, []);

    // Handle remote streams
    useEffect(() => {
        const handleRemoteStream = (sessionId: string, stream: MediaStream) => {
            const session = sessionManager.getSession(sessionId);
            if (!session) return;

            // Set up video elements
            const videoElement = videoRefs.current.get(sessionId);
            if (videoElement) {
                videoElement.srcObject = stream;
            }

            // Notify parent component
            // Determine bitrate based on stream quality
            const videoTrack = stream.getVideoTracks()[0];
            const settings = videoTrack?.getSettings();
            const bitrate: 'low' | 'high' = (settings?.width || 0) < 1280 ? 'low' : 'high';
            onStreamReady?.(sessionId, stream, bitrate);
        };

        sessionManager.onRemoteStreamCallback(handleRemoteStream);
    }, [onStreamReady]);

    const handleCreateSession = useCallback(async (name: string, type: 'reporter' | 'guest') => {
        setIsConnecting(true);
        try {
            const session = await sessionManager.createSession(name, type);
            setSessions(prev => [...prev, session]);
            setSelectedSession(session);
            onSessionSelect?.(session);
        } catch (err) {
            console.error('Failed to create session:', err);
        } finally {
            setIsConnecting(false);
        }
    }, [onSessionSelect]);

    const handleStartRecording = useCallback(async (sessionId: string) => {
        try {
            await sessionManager.startRecording(sessionId);
        } catch (err) {
            console.error('Failed to start recording:', err);
        }
    }, []);

    const handleStopRecording = useCallback(async (sessionId: string) => {
        try {
            await sessionManager.stopRecording(sessionId);
        } catch (err) {
            console.error('Failed to stop recording:', err);
        }
    }, []);

    const handleStartFacebookLive = useCallback(async (sessionId: string) => {
        try {
            const session = sessionManager.getSession(sessionId);
            if (!session || !session.remoteStream) {
                throw new Error('Session not ready for streaming');
            }

            const liveSession = await createFacebookLiveVideo(
                `Live from ${session.name}`,
                `Remote session: ${session.name}`
            );

            // In a real implementation, you would:
            // 1. Get the RTMP URL from liveSession
            // 2. Set up an RTMP encoder (e.g., FFmpeg, OBS)
            // 3. Stream the remote stream to Facebook

            console.log('[RemoteSessionPanel] Facebook Live started:', liveSession);
        } catch (err) {
            console.error('Failed to start Facebook Live:', err);
        }
    }, []);

    const handleCloseSession = useCallback(async (sessionId: string) => {
        try {
            await sessionManager.closeSession(sessionId);
            setSessions(prev => prev.filter(s => s.id !== sessionId));
            if (selectedSession?.id === sessionId) {
                setSelectedSession(null);
            }
        } catch (err) {
            console.error('Failed to close session:', err);
        }
    }, [selectedSession]);

    return (
        <div className={styles.panel}>
            <div className={styles.header}>
                <div className={styles.titleSection}>
                    <Video size={20} />
                    <h2>Remote Sessions</h2>
                    <span className={styles.sessionCount}>{sessions.length}</span>
                </div>
                <button
                    className={styles.createButton}
                    onClick={async () => {
                        const { safePrompt } = await import('../lib/promptPolyfill');
                        const name = await safePrompt('Enter session name:');
                        if (!name) return;
                        const type = (await safePrompt('Type (reporter/guest):')) as 'reporter' | 'guest' | null;
                        if (name && type && (type === 'reporter' || type === 'guest')) {
                            handleCreateSession(name, type);
                        }
                    }}
                    disabled={isConnecting}
                >
                    <Users size={16} />
                    New Session
                </button>
            </div>

            <div className={styles.content}>
                {sessions.length === 0 ? (
                    <div className={styles.emptyState}>
                        <Video size={48} />
                        <p>No remote sessions</p>
                        <p className={styles.emptySubtext}>Create a session to connect with reporters or guests</p>
                    </div>
                ) : (
                    <div className={styles.sessionsList}>
                        {sessions.map(session => (
                            <SessionCard
                                key={session.id}
                                session={session}
                                isSelected={selectedSession?.id === session.id}
                                onSelect={() => {
                                    setSelectedSession(session);
                                    onSessionSelect?.(session);
                                }}
                                onStartRecording={() => handleStartRecording(session.id)}
                                onStopRecording={() => handleStopRecording(session.id)}
                                onStartFacebookLive={() => handleStartFacebookLive(session.id)}
                                onClose={() => handleCloseSession(session.id)}
                                videoRef={(el) => {
                                    if (el) {
                                        videoRefs.current.set(session.id, el);
                                    } else {
                                        videoRefs.current.delete(session.id);
                                    }
                                }}
                            />
                        ))}
                    </div>
                )}
            </div>
        </div>
    );
};

interface SessionCardProps {
    session: WebRTCSession;
    isSelected: boolean;
    onSelect: () => void;
    onStartRecording: () => void;
    onStopRecording: () => void;
    onStartFacebookLive: () => void;
    onClose: () => void;
    videoRef: (el: HTMLVideoElement | null) => void;
}

const SessionCard: React.FC<SessionCardProps> = ({
    session,
    isSelected,
    onSelect,
    onStartRecording,
    onStopRecording,
    onStartFacebookLive,
    onClose,
    videoRef,
}) => {
    const getStatusColor = () => {
        switch (session.status) {
            case 'connected':
                return 'var(--success)';
            case 'connecting':
                return 'var(--warning)';
            case 'disconnected':
            case 'error':
                return 'var(--danger)';
            default:
                return 'var(--text-muted)';
        }
    };

    const totalRecordedDuration = session.recordingSegments
        .filter(s => s.status === 'completed')
        .reduce((sum, s) => sum + s.duration, 0);

    return (
        <div
            className={`${styles.sessionCard} ${isSelected ? styles.selected : ''} ${session.status === 'connected' ? styles.connected : ''}`}
            onClick={onSelect}
        >
            <div className={styles.sessionHeader}>
                <div className={styles.sessionInfo}>
                    <div className={styles.sessionName}>
                        {session.name}
                        <span className={styles.sessionType}>{session.type}</span>
                    </div>
                    <div className={styles.sessionStatus}>
                        <div
                            className={styles.statusDot}
                            style={{ background: getStatusColor() }}
                        />
                        <span>{session.status}</span>
                    </div>
                </div>
                <button
                    className={styles.closeButton}
                    onClick={(e) => {
                        e.stopPropagation();
                        onClose();
                    }}
                    title="Close session"
                >
                    <X size={16} />
                </button>
            </div>

            {session.remoteStream && (
                <div className={styles.videoContainer}>
                    <video
                        ref={videoRef}
                        autoPlay
                        playsInline
                        muted
                        className={styles.video}
                    />
                    <div className={styles.videoOverlay}>
                        {session.isLive && (
                            <div className={styles.liveBadge}>
                                <Radio size={12} />
                                LIVE
                            </div>
                        )}
                        {session.isRecording && (
                            <div className={styles.recordingBadge}>
                                <Square size={12} />
                                REC
                            </div>
                        )}
                    </div>
                </div>
            )}

            <div className={styles.sessionControls}>
                <button
                    className={styles.controlButton}
                    onClick={(e) => {
                        e.stopPropagation();
                        if (session.isRecording) {
                            onStopRecording();
                        } else {
                            onStartRecording();
                        }
                    }}
                    disabled={session.status !== 'connected'}
                    title={session.isRecording ? 'Stop Recording' : 'Start Recording'}
                >
                    <Film size={14} />
                    {session.isRecording ? 'Stop Rec' : 'Record'}
                </button>
                <button
                    className={styles.controlButton}
                    onClick={(e) => {
                        e.stopPropagation();
                        onStartFacebookLive();
                    }}
                    disabled={session.status !== 'connected' || session.isLive}
                    title="Start Facebook Live"
                >
                    <Radio size={14} />
                    Go Live
                </button>
            </div>

            {session.recordingSegments.length > 0 && (
                <div className={styles.recordingInfo}>
                    <div className={styles.recordingStats}>
                        <Film size={12} />
                        <span>{session.recordingSegments.length} segments</span>
                        <span>•</span>
                        <span>{formatDuration(totalRecordedDuration)}</span>
                    </div>
                </div>
            )}
        </div>
    );
};
