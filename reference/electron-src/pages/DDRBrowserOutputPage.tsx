/**
 * DDR Browser Output – one channel for OBS Browser Source
 *
 * In OBS: Add Browser Source, URL: http://localhost:8080/ddr-output/A (or B, C, D).
 * Displays the current DDR channel video only; no controls.
 */

import React, { useEffect, useState } from 'react';
import { useParams } from 'react-router-dom';
import styles from './DDRBrowserOutputPage.module.css';

function getBaseUrl(): string {
    if (typeof window === 'undefined') return '';
    const { protocol, hostname, port } = window.location;
    const p = port || (protocol === 'https:' ? '443' : '80');
    return `${protocol}//${hostname}:${p}`;
}

export function DDRBrowserOutputPage() {
    const params = useParams<{ channel?: string }>();
    const channelParam = (params.channel || 'A').toUpperCase();
    const channel = ['A', 'B', 'C', 'D'].includes(channelParam) ? channelParam : 'A';
    const [state, setState] = useState<{ ddr?: Record<string, { src?: string; isPlaying?: boolean; title?: string }> }>({});
    const base = getBaseUrl();

    useEffect(() => {
        const fetchState = async () => {
            try {
                const res = await fetch(`${base}/api/state`);
                if (res.ok) {
                    const data = await res.json();
                    setState(data);
                }
            } catch (_) { /* ignore */ }
        };
        fetchState();
        const t = setInterval(fetchState, 500);
        return () => clearInterval(t);
    }, [base]);

    const ch = state.ddr && channel in state.ddr ? state.ddr[channel] : null;
    const src = ch?.src || '';
    const isPlaying = ch?.isPlaying ?? false;

    useEffect(() => {
        const video = document.getElementById('ddr-video') as HTMLVideoElement;
        if (!video) return;
        if (src) {
            video.src = src;
            if (isPlaying) video.play().catch(() => {});
        } else {
            video.removeAttribute('src');
        }
    }, [src, isPlaying]);

    return (
        <div className={styles.container}>
            <video
                id="ddr-video"
                className={styles.video}
                autoPlay
                muted
                playsInline
                onError={(e) => console.error('[DDR] Video error:', e)}
            />
            {!src && (
                <div className={styles.placeholder}>
                    DDR {channel} – No media
                </div>
            )}
        </div>
    );
}
