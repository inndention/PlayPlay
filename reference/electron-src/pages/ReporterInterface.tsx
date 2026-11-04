/**
 * ReporterInterface - Standalone web interface for remote reporters
 * 
 * Features:
 * - Basic playout control (play, pause, next)
 * - CG template selection and triggering
 * - Input source switching
 * - Countdown to fade-in-to-air with queue button
 * - Optional PIP layout control
 * - Push-to-talk COMMS to studio
 * - Chat with studio
 * - Countdown request to producer
 * - View live stream stats and events
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './ReporterInterface.module.css';
import {
    Play, Pause, Square, SkipForward, Video, VideoOff, Mic, MicOff,
    Layers, Radio, MessageSquare, Clock, BarChart2, Settings,
    Eye, EyeOff, ChevronDown, ChevronRight, Send, AlertCircle
} from 'lucide-react';
import {
    getWebRTCSessionManager,
    WebRTCSession,
} from '../lib/webrtcSessionManager';
import {
    getCommsService,
} from '../lib/commsService';
import { CGElement } from '../lib/types';

interface ReporterInterfaceProps {
    sessionId?: string;
    reporterName?: string;
}

export const ReporterInterface: React.FC<ReporterInterfaceProps> = ({
    sessionId: propSessionId,
    reporterName: propReporterName,
}) => {
    const [sessionId, setSessionId] = useState<string | null>(propSessionId || null);
    const [reporterName, setReporterName] = useState(propReporterName || '');
    const [session, setSession] = useState<WebRTCSession | null>(null);
    const [isConnected, setIsConnected] = useState(false);
    const [isLive, setIsLive] = useState(false);
    const [countdown, setCountdown] = useState<number | null>(null);
    const [selectedCG, setSelectedCG] = useState<CGElement | null>(null);
    const [availableCGs, setAvailableCGs] = useState<CGElement[]>([]);
    const [chatMessages, setChatMessages] = useState<any[]>([]);
    const [chatInput, setChatInput] = useState('');
    const [showStats, setShowStats] = useState(false);
    const [showPIP, setShowPIP] = useState(false);
    const [selectedInput, setSelectedInput] = useState<string>('camera-1');
    const [availableInputs, setAvailableInputs] = useState<MediaDeviceInfo[]>([]);
    const sessionManager = getWebRTCSessionManager();
    const commsService = getCommsService();
    const previewVideoRef = useRef<HTMLVideoElement>(null);
    const programVideoRef = useRef<HTMLVideoElement>(null);
    const countdownIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

    // Initialize session
    useEffect(() => {
        const initializeSession = async () => {
            if (!sessionId && !reporterName) {
                // Get session ID from URL or prompt
                const urlParams = new URLSearchParams(window.location.search);
                const urlSessionId = urlParams.get('sessionId');
                const urlName = urlParams.get('name');

                if (urlSessionId && urlName) {
                    setSessionId(urlSessionId);
                    setReporterName(urlName);
                    await joinSession(urlSessionId, urlName);
                } else {
                    // Show join form
                    return;
                }
            } else if (sessionId && reporterName) {
                await joinSession(sessionId, reporterName);
            }
        };

        initializeSession();
    }, []);

    // Get available inputs
    useEffect(() => {
        navigator.mediaDevices.enumerateDevices().then(devices => {
            setAvailableInputs(devices.filter(d => d.kind === 'videoinput'));
        });
    }, []);

    const joinSession = async (id: string, name: string) => {
        try {
            const newSession = await sessionManager.joinSession(id, name, 'reporter');
            setSession(newSession);
            setIsConnected(newSession.status === 'connected');

            // Set up stream
            if (newSession.localStream) {
                if (previewVideoRef.current) {
                    previewVideoRef.current.srcObject = newSession.localStream;
                }
            }

            // Listen for remote stream
            sessionManager.onRemoteStreamCallback((sessionId, stream) => {
                if (programVideoRef.current) {
                    programVideoRef.current.srcObject = stream;
                }
            });

            // Listen for countdown
            // In real implementation, this would come from signaling
        } catch (err) {
            console.error('Failed to join session:', err);
        }
    };

    const handleStartCountdown = useCallback((seconds: number) => {
        setCountdown(seconds);
        setIsLive(false);

        if (countdownIntervalRef.current) {
            clearInterval(countdownIntervalRef.current);
        }

        countdownIntervalRef.current = setInterval(() => {
            setCountdown(prev => {
                if (prev === null || prev <= 1) {
                    if (countdownIntervalRef.current) {
                        clearInterval(countdownIntervalRef.current);
                    }
                    setIsLive(true);
                    return null;
                }
                return prev - 1;
            });
        }, 1000);
    }, []);

    const handleRequestCountdown = useCallback(async () => {
        const { safePrompt } = await import('../lib/promptPolyfill');
        const seconds = await safePrompt('Request countdown (seconds):');
        if (seconds) {
            commsService.requestCountdown('default', sessionId || '', parseInt(seconds, 10));
        }
    }, [sessionId]);

    const handleSendChat = useCallback(() => {
        if (!sessionId || !chatInput.trim()) return;
        commsService.sendChatMessage('default', sessionId, chatInput);
        setChatInput('');
    }, [sessionId, chatInput]);

    const handleStartPTT = useCallback(async (targetSessionId: string) => {
        if (!sessionId) return;
        await commsService.startPTT(sessionId, targetSessionId);
    }, [sessionId]);

    const handleStopPTT = useCallback(() => {
        if (!sessionId) return;
        commsService.stopPTT(sessionId);
    }, [sessionId]);

    if (!sessionId || !reporterName) {
        return (
            <div className={styles.joinScreen}>
                <div className={styles.joinForm}>
                    <h2>Reporter Interface</h2>
                    <input
                        type="text"
                        placeholder="Your Name"
                        value={reporterName}
                        onChange={(e) => setReporterName(e.target.value)}
                    />
                    <input
                        type="text"
                        placeholder="Session ID"
                        value={sessionId || ''}
                        onChange={(e) => setSessionId(e.target.value)}
                    />
                    <button
                        onClick={() => sessionId && reporterName && joinSession(sessionId, reporterName)}
                        disabled={!sessionId || !reporterName}
                    >
                        Join Session
                    </button>
                </div>
            </div>
        );
    }

    return (
        <div className={styles.interface}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    {isLive && (
                        <div className={styles.liveBadge}>
                            <Radio size={16} />
                            LIVE
                        </div>
                    )}
                    <span className={styles.reporterName}>{reporterName}</span>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.statsButton}
                        onClick={() => setShowStats(!showStats)}
                    >
                        <BarChart2 size={16} />
                    </button>
                    <button className={styles.settingsButton}>
                        <Settings size={16} />
                    </button>
                </div>
            </div>

            <div className={styles.mainContent}>
                {/* Video Previews */}
                <div className={styles.videoSection}>
                    <div className={styles.previewArea}>
                        <div className={styles.videoContainer}>
                            <video
                                ref={previewVideoRef}
                                autoPlay
                                playsInline
                                muted
                                className={styles.video}
                            />
                            <div className={styles.videoLabel}>Preview</div>
                        </div>
                        {showPIP && (
                            <div className={styles.pipContainer}>
                                <video
                                    ref={programVideoRef}
                                    autoPlay
                                    playsInline
                                    muted
                                    className={styles.pipVideo}
                                />
                                <div className={styles.videoLabel}>Program</div>
                            </div>
                        )}
                    </div>

                    {/* Countdown Display */}
                    {countdown !== null && (
                        <div className={styles.countdownDisplay}>
                            <Clock size={24} />
                            <span className={styles.countdownValue}>{countdown}</span>
                        </div>
                    )}
                </div>

                {/* Controls */}
                <div className={styles.controlsSection}>
                    <div className={styles.controlGroup}>
                        <label>Input Source</label>
                        <select
                            value={selectedInput}
                            onChange={(e) => setSelectedInput(e.target.value)}
                        >
                            {availableInputs.map(device => (
                                <option key={device.deviceId} value={device.deviceId}>
                                    {device.label || `Camera ${device.deviceId.substring(0, 8)}`}
                                </option>
                            ))}
                        </select>
                    </div>

                    <div className={styles.controlGroup}>
                        <label>CG Template</label>
                        <select
                            value={selectedCG?.id || ''}
                            onChange={(e) => {
                                const cg = availableCGs.find(c => c.id === e.target.value);
                                setSelectedCG(cg || null);
                            }}
                        >
                            <option value="">None</option>
                            {availableCGs.map(cg => (
                                <option key={cg.id} value={cg.id}>
                                    {cg.name}
                                </option>
                            ))}
                        </select>
                        {selectedCG && (
                            <button className={styles.triggerButton}>
                                Trigger CG
                            </button>
                        )}
                    </div>

                    <div className={styles.playbackControls}>
                        <button className={styles.controlButton}>
                            <Play size={16} />
                        </button>
                        <button className={styles.controlButton}>
                            <Pause size={16} />
                        </button>
                        <button className={styles.controlButton}>
                            <Square size={16} />
                        </button>
                        <button className={styles.controlButton}>
                            <SkipForward size={16} />
                        </button>
                    </div>

                    <div className={styles.queueControls}>
                        <button
                            className={styles.queueButton}
                            onClick={handleRequestCountdown}
                        >
                            <Clock size={16} />
                            Request Countdown
                        </button>
                        <button className={styles.queueButton}>
                            Queue to Air
                        </button>
                    </div>
                </div>

                {/* COMMS */}
                <div className={styles.commsSection}>
                    <div className={styles.commsTabs}>
                        <button className={styles.commsTab}>Chat</button>
                        <button className={styles.commsTab}>PTT</button>
                    </div>

                    <div className={styles.chatArea}>
                        <div className={styles.messagesList}>
                            {chatMessages.map((msg, idx) => (
                                <div key={idx} className={styles.messageItem}>
                                    <span className={styles.messageSender}>{msg.sender}:</span>
                                    <span className={styles.messageText}>{msg.text}</span>
                                </div>
                            ))}
                        </div>
                        <div className={styles.chatInput}>
                            <input
                                type="text"
                                placeholder="Type a message..."
                                value={chatInput}
                                onChange={(e) => setChatInput(e.target.value)}
                                onKeyPress={(e) => {
                                    if (e.key === 'Enter') {
                                        handleSendChat();
                                    }
                                }}
                            />
                            <button onClick={handleSendChat}>
                                <Send size={16} />
                            </button>
                        </div>
                    </div>
                </div>

                {/* Stats Panel */}
                {showStats && (
                    <div className={styles.statsPanel}>
                        <h3>Live Stream Stats</h3>
                        <div className={styles.statsContent}>
                            <div className={styles.statItem}>
                                <span className={styles.statLabel}>Viewers:</span>
                                <span className={styles.statValue}>--</span>
                            </div>
                            <div className={styles.statItem}>
                                <span className={styles.statLabel}>Latency:</span>
                                <span className={styles.statValue}>--</span>
                            </div>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
};
