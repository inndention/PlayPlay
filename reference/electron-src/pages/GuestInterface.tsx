/**
 * GuestInterface - Simple interface for show guests
 * 
 * Features:
 * - Simple name entry and join
 * - Video/audio device selection
 * - PTT to studio (optional always-on)
 * - See countdown timers
 * - See "You're Live" indicator
 * - Minimal, clean interface
 */

import React, { useState, useEffect, useRef } from 'react';
import styles from './GuestInterface.module.css';
import {
    Video, VideoOff, Mic, MicOff, Radio, Clock, User, Check,
    AlertCircle, Settings
} from 'lucide-react';
import {
    getWebRTCSessionManager,
    WebRTCSession,
} from '../lib/webrtcSessionManager';

interface GuestInterfaceProps {
    sessionId?: string;
    guestName?: string;
}

export const GuestInterface: React.FC<GuestInterfaceProps> = ({
    sessionId: propSessionId,
    guestName: propGuestName,
}) => {
    const [sessionId, setSessionId] = useState<string | null>(propSessionId || null);
    const [guestName, setGuestName] = useState(propGuestName || '');
    const [session, setSession] = useState<WebRTCSession | null>(null);
    const [isConnected, setIsConnected] = useState(false);
    const [isLive, setIsLive] = useState(false);
    const [countdown, setCountdown] = useState<number | null>(null);
    const [selectedVideoDevice, setSelectedVideoDevice] = useState<string>('');
    const [selectedAudioDevice, setSelectedAudioDevice] = useState<string>('');
    const [availableDevices, setAvailableDevices] = useState<{
        video: MediaDeviceInfo[];
        audio: MediaDeviceInfo[];
    }>({ video: [], audio: [] });
    const [videoEnabled, setVideoEnabled] = useState(true);
    const [audioEnabled, setAudioEnabled] = useState(true);
    const [pttMode, setPttMode] = useState<'always-on' | 'push-to-talk'>('always-on');
    const sessionManager = getWebRTCSessionManager();
    const videoRef = useRef<HTMLVideoElement>(null);
    const countdownIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

    // Get session ID from URL or QR code
    useEffect(() => {
        const urlParams = new URLSearchParams(window.location.search);
        const urlSessionId = urlParams.get('sessionId');
        if (urlSessionId) {
            setSessionId(urlSessionId);
        }
    }, []);

    // Get available devices
    useEffect(() => {
        navigator.mediaDevices.enumerateDevices().then(devices => {
            setAvailableDevices({
                video: devices.filter(d => d.kind === 'videoinput'),
                audio: devices.filter(d => d.kind === 'audioinput'),
            });
        });
    }, []);

    const handleJoin = async () => {
        if (!sessionId || !guestName) {
            alert('Please enter your name and session ID');
            return;
        }

        try {
            const newSession = await sessionManager.joinSession(sessionId, guestName, 'guest');
            setSession(newSession);
            setIsConnected(newSession.status === 'connected');

            // Set up local video
            if (newSession.localStream && videoRef.current) {
                videoRef.current.srcObject = newSession.localStream;
            }

            // Listen for countdown (would come from signaling in real implementation)
            // Mock countdown for demo
        } catch (err) {
            console.error('Failed to join session:', err);
            alert('Failed to join session. Please check your session ID.');
        }
    };

    const handleDeviceChange = async (deviceId: string, type: 'video' | 'audio') => {
        if (!session) return;

        try {
            const constraints: MediaStreamConstraints = {
                video: type === 'video' ? { deviceId: { exact: deviceId } } : videoEnabled,
                audio: type === 'audio' ? { deviceId: { exact: deviceId } } : audioEnabled,
            };

            const stream = await navigator.mediaDevices.getUserMedia(constraints);
            
            // Replace tracks in session
            if (type === 'video') {
                stream.getVideoTracks().forEach(track => {
                    const oldTrack = session.localStream?.getVideoTracks()[0];
                    if (oldTrack) {
                        session.localStream?.removeTrack(oldTrack);
                        oldTrack.stop();
                    }
                    session.localStream?.addTrack(track);
                });
            } else {
                stream.getAudioTracks().forEach(track => {
                    const oldTrack = session.localStream?.getAudioTracks()[0];
                    if (oldTrack) {
                        session.localStream?.removeTrack(oldTrack);
                        oldTrack.stop();
                    }
                    session.localStream?.addTrack(track);
                });
            }

            if (videoRef.current) {
                videoRef.current.srcObject = session.localStream;
            }
        } catch (err) {
            console.error('Failed to change device:', err);
        }
    };

    const handleToggleVideo = () => {
        if (!session?.localStream) return;
        const videoTrack = session.localStream.getVideoTracks()[0];
        if (videoTrack) {
            videoTrack.enabled = !videoEnabled;
            setVideoEnabled(!videoEnabled);
        }
    };

    const handleToggleAudio = () => {
        if (!session?.localStream) return;
        const audioTrack = session.localStream.getAudioTracks()[0];
        if (audioTrack) {
            audioTrack.enabled = !audioEnabled;
            setAudioEnabled(!audioEnabled);
        }
    };

    if (!sessionId || !isConnected) {
        return (
            <div className={styles.joinScreen}>
                <div className={styles.joinCard}>
                    <div className={styles.logo}>
                        <User size={48} />
                    </div>
                    <h1>Guest Interface</h1>
                    <p className={styles.subtitle}>Enter your name to join the show</p>
                    
                    <div className={styles.joinForm}>
                        <div className={styles.inputGroup}>
                            <label>Your Name</label>
                            <input
                                type="text"
                                placeholder="Enter your name"
                                value={guestName}
                                onChange={(e) => setGuestName(e.target.value)}
                                onKeyPress={(e) => {
                                    if (e.key === 'Enter' && sessionId) {
                                        handleJoin();
                                    }
                                }}
                            />
                        </div>
                        {!sessionId && (
                            <div className={styles.inputGroup}>
                                <label>Session ID (or scan QR code)</label>
                                <input
                                    type="text"
                                    placeholder="Session ID"
                                    value={sessionId || ''}
                                    onChange={(e) => setSessionId(e.target.value)}
                                    onKeyPress={(e) => {
                                        if (e.key === 'Enter' && guestName) {
                                            handleJoin();
                                        }
                                    }}
                                />
                            </div>
                        )}
                        <button
                            className={styles.joinButton}
                            onClick={handleJoin}
                            disabled={!guestName || !sessionId}
                        >
                            <Check size={20} />
                            Join Session
                        </button>
                    </div>
                </div>
            </div>
        );
    }

    return (
        <div className={styles.interface}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <span className={styles.guestName}>{guestName}</span>
                    {isLive && (
                        <div className={styles.liveBadge}>
                            <Radio size={16} />
                            YOU'RE LIVE
                        </div>
                    )}
                </div>
                {countdown !== null && (
                    <div className={styles.countdownBadge}>
                        <Clock size={16} />
                        {countdown}s
                    </div>
                )}
            </div>

            <div className={styles.mainContent}>
                <div className={styles.videoContainer}>
                    <video
                        ref={videoRef}
                        autoPlay
                        playsInline
                        muted={!audioEnabled}
                        className={styles.video}
                    />
                    {!videoEnabled && (
                        <div className={styles.videoPlaceholder}>
                            <VideoOff size={48} />
                            <p>Camera Off</p>
                        </div>
                    )}
                </div>

                <div className={styles.controls}>
                    <div className={styles.deviceControls}>
                        <div className={styles.deviceGroup}>
                            <label>Camera</label>
                            <select
                                value={selectedVideoDevice}
                                onChange={(e) => {
                                    setSelectedVideoDevice(e.target.value);
                                    handleDeviceChange(e.target.value, 'video');
                                }}
                            >
                                {availableDevices.video.map(device => (
                                    <option key={device.deviceId} value={device.deviceId}>
                                        {device.label || `Camera ${device.deviceId.substring(0, 8)}`}
                                    </option>
                                ))}
                            </select>
                            <button
                                className={`${styles.toggleButton} ${!videoEnabled ? styles.off : ''}`}
                                onClick={handleToggleVideo}
                            >
                                {videoEnabled ? <Video size={20} /> : <VideoOff size={20} />}
                            </button>
                        </div>

                        <div className={styles.deviceGroup}>
                            <label>Microphone</label>
                            <select
                                value={selectedAudioDevice}
                                onChange={(e) => {
                                    setSelectedAudioDevice(e.target.value);
                                    handleDeviceChange(e.target.value, 'audio');
                                }}
                            >
                                {availableDevices.audio.map(device => (
                                    <option key={device.deviceId} value={device.deviceId}>
                                        {device.label || `Mic ${device.deviceId.substring(0, 8)}`}
                                    </option>
                                ))}
                            </select>
                            <button
                                className={`${styles.toggleButton} ${!audioEnabled ? styles.off : ''}`}
                                onClick={handleToggleAudio}
                            >
                                {audioEnabled ? <Mic size={20} /> : <MicOff size={20} />}
                            </button>
                        </div>
                    </div>

                    <div className={styles.pttControl}>
                        <label>Audio Mode</label>
                        <div className={styles.pttOptions}>
                            <button
                                className={`${styles.pttOption} ${pttMode === 'always-on' ? styles.active : ''}`}
                                onClick={() => setPttMode('always-on')}
                            >
                                Always On
                            </button>
                            <button
                                className={`${styles.pttOption} ${pttMode === 'push-to-talk' ? styles.active : ''}`}
                                onClick={() => setPttMode('push-to-talk')}
                            >
                                Push to Talk
                            </button>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    );
};
