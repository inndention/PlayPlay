import React, { useCallback, useEffect, useState } from 'react';
import { Bot, Radio, Sparkles } from 'lucide-react';
import { LS_PLAYPLAY_AGENT_TOASTS_ENABLED } from '../hooks/usePlayplayAgentToastBridge';
import styles from './PlayplayAgentSettingsCard.module.css';

type EnvHints = {
    voiceAgentSnapshot: boolean;
    agentDevStub: boolean;
    openrouterModel: string;
    packaged: boolean;
};

const defaultHints: EnvHints = {
    voiceAgentSnapshot: false,
    agentDevStub: false,
    openrouterModel: '(unset)',
    packaged: false,
};

export function PlayplayAgentSettingsCard() {
    const [toastsOn, setToastsOn] = useState(() => {
        try {
            return localStorage.getItem(LS_PLAYPLAY_AGENT_TOASTS_ENABLED) === '1';
        } catch {
            return false;
        }
    });
    const [hints, setHints] = useState<EnvHints>(defaultHints);

    const refreshHints = useCallback(async () => {
        const api = (window as unknown as { electronAPI?: {
            getPlayplayAgentEnvHints?: () => Promise<EnvHints>;
        } }).electronAPI;
        if (!api?.getPlayplayAgentEnvHints) {
            setHints(defaultHints);
            return;
        }
        try {
            const h = await api.getPlayplayAgentEnvHints();
            setHints({ ...defaultHints, ...h });
        } catch {
            setHints(defaultHints);
        }
    }, []);

    useEffect(() => {
        void refreshHints();
    }, [refreshHints]);

    const setToasts = (on: boolean) => {
        try {
            localStorage.setItem(LS_PLAYPLAY_AGENT_TOASTS_ENABLED, on ? '1' : '0');
        } catch {
            /* ignore */
        }
        setToastsOn(on);
        window.dispatchEvent(new Event('playplay-agent-settings-changed'));
    };

    return (
        <section className={styles.card} aria-labelledby="playplay-agent-heading">
            <div className={styles.cardHead}>
                <Sparkles size={18} className={styles.icon} />
                <div>
                    <h2 id="playplay-agent-heading">Play Play Agent</h2>
                    <p className={styles.sub}>
                        Toast-first agent signals (proposals, warnings, completions). VoIP slice uses the same read-only playout snapshot as{' '}
                        <code>GET /api/voice-agent/snapshot</code>.
                    </p>
                </div>
            </div>

            <label className={styles.row}>
                <input
                    type="checkbox"
                    checked={toastsOn}
                    onChange={(e) => setToasts(e.target.checked)}
                />
                <span><Bot size={14} /> Show agent toasts in this app</span>
            </label>

            <div className={styles.meta}>
                <div><strong>Main model (env)</strong>: <code>{hints.openrouterModel}</code></div>
                <div className={styles.sip}>
                    <Radio size={14} />
                    <span>
                        SIP / voice snapshot route:{' '}
                        <strong>{hints.voiceAgentSnapshot ? 'enabled' : 'off'}</strong>
                        {' · '}
                        Dev IPC stub: <strong>{hints.agentDevStub ? 'on' : 'off'}</strong>
                    </span>
                </div>
                <p className={styles.hint}>
                    Set <code>PLAYPLAY_VOICE_AGENT=1</code> for the snapshot HTTP route; <code>PLAYPLAY_AGENT_DEV=1</code> for simulated main→renderer agent events.
                    See <code>docs/PLAYPLAY_AI_STARTUP.md</code>.
                </p>
            </div>
        </section>
    );
}
