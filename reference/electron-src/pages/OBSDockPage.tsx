/**
 * OBS Dock – Playout & scheduling controls for OBS Studio
 *
 * Add in OBS: Docks → Custom Browser Dock → URL: http://localhost:8080/obs-dock
 * No video preview; control playout and view schedule from inside OBS.
 */

import React, { useEffect, useState, useCallback } from 'react';
import { Play, Pause, SkipForward, Square } from 'lucide-react';
import styles from './OBSDockPage.module.css';

interface PlayoutState {
    currentMedia?: { title?: string; media?: { title?: string }; mediaId?: string } | null;
    nextMedia?: { title?: string; media?: { title?: string }; mediaId?: string } | null;
    isPlaying?: boolean;
    currentTime?: number;
    timestamp?: number;
}

function getBaseUrl(): string {
    if (typeof window === 'undefined') return '';
    const { protocol, hostname, port } = window.location;
    const p = port || (protocol === 'https:' ? '443' : '80');
    return `${protocol}//${hostname}:${p}`;
}

export function OBSDockPage() {
    const [state, setState] = useState<PlayoutState>({});
    const [error, setError] = useState<string | null>(null);
    const base = getBaseUrl();

    const fetchState = useCallback(async () => {
        try {
            const res = await fetch(`${base}/api/state`);
            if (res.ok) {
                const data = await res.json();
                setState(data);
                setError(null);
            }
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Failed to connect');
        }
    }, [base]);

    useEffect(() => {
        fetchState();
        const t = setInterval(fetchState, 2000);
        return () => clearInterval(t);
    }, [fetchState]);

    const sendCommand = useCallback(async (action: string) => {
        try {
            await fetch(`${base}/api/playout/command`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ action }),
            });
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Command failed');
        }
    }, [base]);

    const currentTitle = state.currentMedia?.media?.title ?? state.currentMedia?.title ?? '—';
    const nextTitle = state.nextMedia?.media?.title ?? state.nextMedia?.title ?? '—';

    const dockUrl = typeof window !== 'undefined' ? window.location.origin + '/obs-dock' : 'http://localhost:8080/obs-dock';

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <span className={styles.title}>PlayPlay · OBS Dock</span>
                {state.isPlaying && <span className={styles.live}>LIVE</span>}
            </div>
            <div className={styles.urlHint}>
                Use this URL in OBS: Docks → Custom Browser Dock → <strong>{dockUrl}</strong>
            </div>

            {error && <div className={styles.error}>{error}</div>}

            <div className={styles.now}>
                <div className={styles.label}>Now</div>
                <div className={styles.value} title={currentTitle}>{currentTitle}</div>
            </div>
            <div className={styles.next}>
                <div className={styles.label}>Next</div>
                <div className={styles.value} title={nextTitle}>{nextTitle}</div>
            </div>

            <div className={styles.controls}>
                <button
                    type="button"
                    className={styles.btn}
                    onClick={() => sendCommand(state.isPlaying ? 'pause' : 'play')}
                    title={state.isPlaying ? 'Pause' : 'Play'}
                >
                    {state.isPlaying ? <Pause size={18} /> : <Play size={18} />}
                </button>
                <button
                    type="button"
                    className={styles.btn}
                    onClick={() => sendCommand('next')}
                    title="Next"
                >
                    <SkipForward size={18} />
                </button>
                <button
                    type="button"
                    className={styles.btn}
                    onClick={() => sendCommand('stop')}
                    title="Stop"
                >
                    <Square size={18} />
                </button>
            </div>
        </div>
    );
}
