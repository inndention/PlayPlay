import React, { useState, useEffect, useCallback } from 'react';
import styles from './AICallerPanel.module.css';
import { Phone, PhoneOff, AlertTriangle, MessageSquare, Volume2, History, Trash2, Zap } from 'lucide-react';
import { aiCaller, AIResponse } from '../lib/aiCaller';
import { useToast } from './Toast';

export function AICallerPanel() {
    const { showToast } = useToast();
    const [isEnabled, setIsEnabled] = useState(aiCaller.getEnabled());
    const [isOnAir, setIsOnAir] = useState(false);
    const [intensity, setIntensity] = useState(aiCaller.getIntensity());
    const [responses, setResponses] = useState<AIResponse[]>([]);

    useEffect(() => {
        const handleCallStarted = (response: AIResponse) => {
            setIsOnAir(true);
            setResponses(prev => [response, ...prev].slice(0, 50));
        };
        const handleCallEnded = () => setIsOnAir(false);
        const handleComplaint = (response: AIResponse) => {
            setResponses(prev => [response, ...prev].slice(0, 50));
        };

        aiCaller.on('callStarted', handleCallStarted);
        aiCaller.on('callEnded', handleCallEnded);
        aiCaller.on('complaint', handleComplaint);

        return () => {
            aiCaller.off('callStarted', handleCallStarted);
            aiCaller.off('callEnded', handleCallEnded);
            aiCaller.off('complaint', handleComplaint);
        };
    }, []);

    useEffect(() => {
        setResponses(aiCaller.getResponseHistory());
        setIsOnAir(aiCaller.getIsOnAir());
    }, []);

    useEffect(() => {
        aiCaller.setIntensity(intensity);
    }, [intensity]);

    const handleToggleEnabled = useCallback(() => {
        const newEnabled = !isEnabled;
        aiCaller.setEnabled(newEnabled);
        setIsEnabled(newEnabled);
        showToast(newEnabled ? 'AI Caller enabled' : 'AI Caller disabled', 'info', 2000);
    }, [isEnabled, showToast]);

    const handleStartCall = useCallback(async () => {
        const response = await aiCaller.startCall();
        if (response) {
            showToast('Call started', 'success', 2000);
        }
    }, [showToast]);

    const handleEndCall = useCallback(async () => {
        await aiCaller.endCall();
        showToast('Call ended', 'info', 2000);
    }, [showToast]);

    const handleGenerateComplaint = useCallback(async () => {
        const response = await aiCaller.generateComplaint();
        if (response) {
            showToast('Complaint generated', 'success', 2000);
        }
    }, [showToast]);

    const handleInterrupt = useCallback(async () => {
        const response = await aiCaller.interruptShow();
        if (response) {
            showToast('Show interrupted with call', 'warning', 3000);
        }
    }, [showToast]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Phone size={20} />
                    <h2>Disgruntled AI Caller</h2>
                </div>
                <button className={`${styles.toggleBtn} ${isEnabled ? styles.enabled : ''}`} onClick={handleToggleEnabled}>
                    {isEnabled ? 'Enabled' : 'Disabled'}
                </button>
            </div>
            <div className={styles.content}>
                <div className={styles.controls}>
                    <div className={styles.controlGroup}>
                        <label>Intensity: {Math.round(intensity * 100)}%</label>
                        <input type="range" min="0" max="1" step="0.1" value={intensity} onChange={(e) => setIntensity(parseFloat(e.target.value))} />
                    </div>
                    <div className={styles.actions}>
                        <button onClick={handleStartCall} disabled={!isEnabled || isOnAir}>
                            <Phone size={16} /> Start Call
                        </button>
                        <button onClick={handleEndCall} disabled={!isOnAir}>
                            <PhoneOff size={16} /> End Call
                        </button>
                        <button onClick={handleGenerateComplaint} disabled={!isEnabled}>
                            <AlertTriangle size={16} /> Generate Complaint
                        </button>
                        <button onClick={handleInterrupt} disabled={!isEnabled || isOnAir}>
                            <Zap size={16} /> Interrupt Show
                        </button>
                    </div>
                </div>
                <div className={styles.history}>
                    <h3>Call History</h3>
                    <div className={styles.responses}>
                        {responses.map((r, i) => (
                            <div key={i} className={styles.response}>
                                <div className={styles.responseText}>{r.text}</div>
                                <div className={styles.timestamp}>{new Date(r.timestamp).toLocaleTimeString()}</div>
                            </div>
                        ))}
                    </div>
                </div>
            </div>
        </div>
    );
}

