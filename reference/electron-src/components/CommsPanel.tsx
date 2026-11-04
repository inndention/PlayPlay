/**
 * CommsPanel - Communication control panel
 * 
 * Features:
 * - Push-to-talk controls
 * - Chat interface
 * - Countdown requests
 * - Channel management
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './CommsPanel.module.css';
import {
    Mic, MicOff, MessageSquare, Send, Clock, Users, Radio,
    ChevronDown, ChevronRight, Plus, X, AlertCircle
} from 'lucide-react';
import {
    getCommsService,
    CommsChannel,
    ChatMessage,
} from '../lib/commsService';
import { getWebRTCSessionManager } from '../lib/webrtcSessionManager';

interface CommsPanelProps {
    currentSessionId?: string;
    onCountdownRequest?: (seconds: number) => void;
}

export const CommsPanel: React.FC<CommsPanelProps> = ({
    currentSessionId,
    onCountdownRequest,
}) => {
    const [channels, setChannels] = useState<CommsChannel[]>([]);
    const [activeChannel, setActiveChannel] = useState<string | null>(null);
    const [messages, setMessages] = useState<ChatMessage[]>([]);
    const [messageInput, setMessageInput] = useState('');
    const [isPTTActive, setIsPTTActive] = useState(false);
    const [pttTarget, setPttTarget] = useState<string | null>(null);
    const commsService = getCommsService();
    const messagesEndRef = useRef<HTMLDivElement>(null);

    // Load channels
    useEffect(() => {
        setChannels(commsService.getAllChannels());
        
        // Set up message listener
        commsService.onMessage((message) => {
            if (message.channelId === activeChannel) {
                setMessages(prev => [...prev, message]);
            }
        });

        // Set up PTT state listener
        commsService.onPTTStateChangeCallback((sessionId, isActive) => {
            if (sessionId === currentSessionId) {
                setIsPTTActive(isActive);
            }
        });
    }, [activeChannel, currentSessionId]);

    // Auto-scroll to bottom of messages
    useEffect(() => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }, [messages]);

    // Load messages when channel changes
    useEffect(() => {
        if (activeChannel) {
            setMessages(commsService.getChannelMessages(activeChannel));
        }
    }, [activeChannel]);

    const handleSendMessage = useCallback(() => {
        if (!activeChannel || !currentSessionId || !messageInput.trim()) return;

        commsService.sendChatMessage(activeChannel, currentSessionId, messageInput);
        setMessageInput('');
    }, [activeChannel, currentSessionId, messageInput]);

    const handleRequestCountdown = useCallback((seconds: number) => {
        if (!activeChannel || !currentSessionId) return;

        commsService.requestCountdown(activeChannel, currentSessionId, seconds);
        onCountdownRequest?.(seconds);
    }, [activeChannel, currentSessionId, onCountdownRequest]);

    const handleStartPTT = useCallback(async (targetSessionId: string) => {
        if (!currentSessionId) return;

        try {
            await commsService.startPTT(currentSessionId, targetSessionId);
            setPttTarget(targetSessionId);
        } catch (err) {
            console.error('Failed to start PTT:', err);
        }
    }, [currentSessionId]);

    const handleStopPTT = useCallback(() => {
        if (!currentSessionId) return;

        commsService.stopPTT(currentSessionId);
        setPttTarget(null);
    }, [currentSessionId]);

    const handleCreateChannel = useCallback(async () => {
        const { safePrompt } = await import('../lib/promptPolyfill');
        const name = await safePrompt('Channel name:');
        if (!name) return;
        const type = (await safePrompt('Type (studio/field/reporter/guest):')) as CommsChannel['type'] | null;
        if (name && type) {
            const channel = commsService.createChannel(name, type);
            setChannels(prev => [...prev, channel]);
            setActiveChannel(channel.id);
        }
    }, []);

    return (
        <div className={styles.panel}>
            <div className={styles.header}>
                <div className={styles.titleSection}>
                    <MessageSquare size={20} />
                    <h2>COMMS</h2>
                </div>
                <button className={styles.createButton} onClick={handleCreateChannel}>
                    <Plus size={16} />
                    New Channel
                </button>
            </div>

            <div className={styles.content}>
                <div className={styles.channelsList}>
                    <h3>Channels</h3>
                    {channels.map(channel => (
                        <div
                            key={channel.id}
                            className={`${styles.channelItem} ${activeChannel === channel.id ? styles.active : ''}`}
                            onClick={() => setActiveChannel(channel.id)}
                        >
                            <Radio size={14} />
                            <span>{channel.name}</span>
                            <span className={styles.participantCount}>
                                {channel.participants.length}
                            </span>
                        </div>
                    ))}
                </div>

                {activeChannel && (
                    <div className={styles.chatArea}>
                        <div className={styles.messagesList}>
                            {messages.map(message => (
                                <div
                                    key={message.id}
                                    className={`${styles.messageItem} ${message.senderId === currentSessionId ? styles.ownMessage : ''}`}
                                >
                                    <div className={styles.messageHeader}>
                                        <span className={styles.messageSender}>{message.senderName}</span>
                                        <span className={styles.messageTime}>
                                            {message.timestamp.toLocaleTimeString()}
                                        </span>
                                    </div>
                                    {message.type === 'countdown-request' ? (
                                        <div className={styles.countdownRequest}>
                                            <Clock size={14} />
                                            <span>Countdown: {message.countdownSeconds}s</span>
                                            <button
                                                className={styles.acceptButton}
                                                onClick={() => onCountdownRequest?.(message.countdownSeconds || 0)}
                                            >
                                                Accept
                                            </button>
                                        </div>
                                    ) : (
                                        <div className={styles.messageText}>{message.message}</div>
                                    )}
                                </div>
                            ))}
                            <div ref={messagesEndRef} />
                        </div>

                        <div className={styles.chatInput}>
                            <input
                                type="text"
                                placeholder="Type a message..."
                                value={messageInput}
                                onChange={(e) => setMessageInput(e.target.value)}
                                onKeyPress={(e) => {
                                    if (e.key === 'Enter') {
                                        handleSendMessage();
                                    }
                                }}
                            />
                            <button
                                className={styles.sendButton}
                                onClick={handleSendMessage}
                                disabled={!messageInput.trim()}
                            >
                                <Send size={16} />
                            </button>
                            <button
                                className={styles.countdownButton}
                                onClick={async () => {
                                    const { safePrompt } = await import('../lib/promptPolyfill');
                                    const seconds = await safePrompt('Countdown seconds:');
                                    if (seconds) {
                                        handleRequestCountdown(parseInt(seconds, 10));
                                    }
                                }}
                                title="Request countdown"
                            >
                                <Clock size={16} />
                            </button>
                        </div>
                    </div>
                )}

                {/* PTT Controls */}
                <div className={styles.pttSection}>
                    <h3>Push-to-Talk</h3>
                    <div className={styles.pttControls}>
                        {isPTTActive ? (
                            <button
                                className={`${styles.pttButton} ${styles.active}`}
                                onMouseUp={handleStopPTT}
                                onTouchEnd={handleStopPTT}
                            >
                                <MicOff size={20} />
                                Release to Stop
                            </button>
                        ) : (
                            <div className={styles.pttTargets}>
                                {getWebRTCSessionManager().getAllSessions()
                                    .filter(s => s.id !== currentSessionId && s.status === 'connected')
                                    .map(session => (
                                        <button
                                            key={session.id}
                                            className={styles.pttButton}
                                            onMouseDown={() => handleStartPTT(session.id)}
                                            onTouchStart={() => handleStartPTT(session.id)}
                                        >
                                            <Mic size={16} />
                                            {session.name}
                                        </button>
                                    ))}
                            </div>
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
};
