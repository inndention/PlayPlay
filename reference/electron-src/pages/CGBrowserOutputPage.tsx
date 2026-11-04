import React, { useEffect, useMemo, useRef, useState } from 'react';
import { useLocation } from 'react-router-dom';
import { CGOverlay, CGLayerState } from '../components/CGOverlay';
import { CGElement } from '../lib/types';
import { logError } from '../lib/errorBus';
import styles from './CGBrowserOutputPage.module.css';

const DESIGN_WIDTH = 1920;
const DESIGN_HEIGHT = 1080;

interface CGChannelState {
    layers: CGLayerState[];
    timestamp?: number;
}

interface CGBrowserOutputPageProps {
    channel?: 'preview' | 'program';
}

function resolveChannel(pathname: string, explicitChannel?: 'preview' | 'program') {
    if (explicitChannel) {
        return explicitChannel;
    }
    if (pathname.includes('/cg-preview')) {
        return 'preview';
    }
    return 'program';
}

function getApiUrl(channel: 'preview' | 'program'): string {
    const path = channel === 'preview' ? '/api/cg-preview-state' : '/api/cg-program-state';
    if (typeof window === 'undefined') {
        return `http://localhost:8080${path}`;
    }
    if (window.location.protocol === 'http:' || window.location.protocol === 'https:') {
        const host = window.location.hostname || 'localhost';
        return `http://${host}:8080${path}`;
    }
    return `http://localhost:8080${path}`;
}

export function CGBrowserOutputPage({ channel: explicitChannel }: CGBrowserOutputPageProps) {
    const location = useLocation();
    const channel = useMemo(
        () => resolveChannel(location.pathname || location.hash || '', explicitChannel),
        [explicitChannel, location.hash, location.pathname],
    );
    const [state, setState] = useState<CGChannelState>({
        layers: [],
    });
    const [scale, setScale] = useState(1);
    const containerRef = useRef<HTMLDivElement>(null);
    const apiUrl = getApiUrl(channel);

    useEffect(() => {
        const updateScale = () => {
            if (!containerRef.current) {
                return;
            }
            const width = containerRef.current.clientWidth;
            const height = containerRef.current.clientHeight;
            if (width <= 0 || height <= 0) {
                return;
            }
            setScale(Math.min(width / DESIGN_WIDTH, height / DESIGN_HEIGHT, 1));
        };

        const rafId = requestAnimationFrame(() => requestAnimationFrame(updateScale));
        window.addEventListener('resize', updateScale);
        return () => {
            cancelAnimationFrame(rafId);
            window.removeEventListener('resize', updateScale);
        };
    }, []);

    useEffect(() => {
        const fetchState = async () => {
            try {
                const response = await fetch(apiUrl);
                if (!response.ok) {
                    return;
                }
                const data = await response.json();
                setState({
                    layers: Array.isArray(data?.layers) ? data.layers : [],
                    timestamp: data?.timestamp,
                });
            } catch (error) {
                logError(`[CGBrowserOutput:${channel}] Failed to fetch state`, error);
            }
        };

        void fetchState();
        const intervalId = window.setInterval(fetchState, 500);
        return () => window.clearInterval(intervalId);
    }, [apiUrl, channel]);

    useEffect(() => {
        const api = (window as Window & { electronAPI?: any }).electronAPI;
        if (!api?.onPlayoutStateUpdate) {
            return () => {};
        }

        return api.onPlayoutStateUpdate((nextState: any) => {
            if (channel === 'preview') {
                setState({
                    layers: Array.isArray(nextState?.cgPreviewLayers) ? nextState.cgPreviewLayers : [],
                    timestamp: nextState?.timestamp,
                });
                return;
            }

            setState({
                layers: Array.isArray(nextState?.cgLayers) ? nextState.cgLayers : [],
                timestamp: nextState?.timestamp,
            });
        });
    }, [channel]);

    return (
        <div ref={containerRef} className={styles.viewport}>
            <div
                className={styles.canvas}
                style={{
                    width: DESIGN_WIDTH,
                    height: DESIGN_HEIGHT,
                    transform: `scale(${scale})`,
                    transformOrigin: 'top left',
                }}
            >
                <CGOverlay elements={[] as CGElement[]} cgLayers={state.layers} isPreview={channel === 'preview'} />
            </div>
        </div>
    );
}
