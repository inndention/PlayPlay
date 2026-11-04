// ============================================
// AI Controller Panel
// UI for the funny, clumsy AI control room controller
// ============================================

import React, { useState, useEffect, useCallback } from 'react';
import styles from './AIControllerPanel.module.css';
import {
    Radio, Play, Pause, Settings, RefreshCw, MessageSquare,
    Zap, AlertCircle, CheckCircle, XCircle, Volume2, VolumeX,
    History, Trash2, Send
} from 'lucide-react';
import { aiController, AIResponse, ShowEvent } from '../lib/aiController';
import { useToast } from './Toast';

export function AIControllerPanel() {
    const { showToast } = useToast();
    const [isEnabled, setIsEnabled] = useState(aiController.getEnabled());
    const [responses, setResponses] = useState<AIResponse[]>([]);
    const [autoRespond, setAutoRespond] = useState(true);
    const [minInterval, setMinInterval] = useState(5000);
    const [manualPrompt, setManualPrompt] = useState('');
    const [isGenerating, setIsGenerating] = useState(false);

    // Subscribe to responses
    useEffect(() => {
        const handleResponse = (response: AIResponse) => {
            setResponses(prev => [response, ...prev].slice(0, 50));
        };

        aiController.on('response', handleResponse);
        return () => {
            aiController.off('response', handleResponse);
        };
    }, []);

    // Load initial responses
    useEffect(() => {
        setResponses(aiController.getResponseHistory());
    }, []);

    // Toggle enabled
    const handleToggleEnabled = useCallback(() => {
        const newEnabled = !isEnabled;
        aiController.setEnabled(newEnabled);
        setIsEnabled(newEnabled);
        showToast(newEnabled ? 'AI Controller enabled' : 'AI Controller disabled', 'info', 2000);
    }, [isEnabled, showToast]);

    // Manual trigger
    const handleManualTrigger = useCallback(async () => {
        setIsGenerating(true);
        try {
            const response = await aiController.triggerResponse(manualPrompt || undefined);
            if (response) {
                setManualPrompt('');
                showToast('Response generated', 'success', 2000);
            } else {
                showToast('Failed to generate response', 'error', 2000);
            }
        } catch (error) {
            showToast(`Error: ${error instanceof Error ? error.message : 'Unknown error'}`, 'error', 3000);
        } finally {
            setIsGenerating(false);
        }
    }, [manualPrompt, showToast]);

    // Update settings
    useEffect(() => {
        aiController.setAutoRespond(autoRespond);
    }, [autoRespond]);

    useEffect(() => {
        aiController.setMinResponseInterval(minInterval);
    }, [minInterval]);

    // Clear history
    const handleClearHistory = useCallback(() => {
        setResponses([]);
        showToast('History cleared', 'info', 2000);
    }, [showToast]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Radio size={20} />
                    <h2>AI Control Room Controller</h2>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={`${styles.toggleBtn} ${isEnabled ? styles.enabled : ''}`}
                        onClick={handleToggleEnabled}
                    >
                        {isEnabled ? <CheckCircle size={16} /> : <XCircle size={16} />}
                        {isEnabled ? 'Enabled' : 'Disabled'}
                    </button>
                </div>
            </div>

            <div className={styles.content}>
                {/* Settings */}
                <div className={styles.settings}>
                    <h3>Settings</h3>
                    <div className={styles.settingGroup}>
                        <label>
                            <input
                                type="checkbox"
                                checked={autoRespond}
                                onChange={(e) => setAutoRespond(e.target.checked)}
                            />
                            Auto-respond to events
                        </label>
                    </div>
                    <div className={styles.settingGroup}>
                        <label>Min Response Interval: {minInterval}ms</label>
                        <input
                            type="range"
                            min="1000"
                            max="30000"
                            step="1000"
                            value={minInterval}
                            onChange={(e) => setMinInterval(parseInt(e.target.value))}
                        />
                    </div>
                </div>

                {/* Manual Trigger */}
                <div className={styles.manualTrigger}>
                    <h3>Manual Trigger</h3>
                    <div className={styles.promptInput}>
                        <input
                            type="text"
                            placeholder="Ask the AI controller something..."
                            value={manualPrompt}
                            onChange={(e) => setManualPrompt(e.target.value)}
                            onKeyPress={(e) => {
                                if (e.key === 'Enter' && !isGenerating) {
                                    handleManualTrigger();
                                }
                            }}
                        />
                        <button
                            onClick={handleManualTrigger}
                            disabled={isGenerating || !isEnabled}
                        >
                            {isGenerating ? <RefreshCw size={16} className={styles.spinning} /> : <Send size={16} />}
                        </button>
                    </div>
                </div>

                {/* Response History */}
                <div className={styles.history}>
                    <div className={styles.historyHeader}>
                        <h3>Response History</h3>
                        <button
                            className={styles.clearBtn}
                            onClick={handleClearHistory}
                        >
                            <Trash2 size={14} />
                            Clear
                        </button>
                    </div>
                    <div className={styles.responses}>
                        {responses.length === 0 ? (
                            <div className={styles.emptyState}>
                                <MessageSquare size={48} />
                                <p>No responses yet</p>
                            </div>
                        ) : (
                            responses.map((response, index) => (
                                <div key={index} className={styles.response}>
                                    <div className={styles.responseHeader}>
                                        <span className={styles.timestamp}>
                                            {new Date(response.timestamp).toLocaleTimeString()}
                                        </span>
                                        <span className={styles.confidence}>
                                            {Math.round(response.confidence * 100)}% confidence
                                        </span>
                                    </div>
                                    <div className={styles.responseText}>{response.text}</div>
                                </div>
                            ))
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
}

