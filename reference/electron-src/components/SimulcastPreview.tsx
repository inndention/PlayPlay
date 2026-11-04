import React, { useState, useEffect, useRef } from 'react';
import { Monitor, Zap, HardDrive, MoreVertical } from 'lucide-react';
import { simulcastManager, SimulcastStream } from '../lib/simulcastManager';
import { trackerApi } from '../lib/trackerApi';
import { ContextMenu, useContextMenu } from './ContextMenu';
import { ContextMenuItem } from '../lib/types';
import styles from './SimulcastPreview.module.css';

interface SimulcastPreviewProps {
    sessionId: string;
    hostname: string;
    onQualityChange?: (sessionId: string, quality: 'fast' | 'heavy') => void;
}

export function SimulcastPreview({ sessionId, hostname, onQualityChange }: SimulcastPreviewProps) {
    const [stream, setStream] = useState<SimulcastStream | null>(null);
    const [isLoading, setIsLoading] = useState(true);
    const videoRef = useRef<HTMLVideoElement>(null);
    const { showMenu, hideMenu } = useContextMenu();

    useEffect(() => {
        const loadStream = async () => {
            setIsLoading(true);
            try {
                // Start preview stream
                const streamUrl = await simulcastManager.startPreviewStream(sessionId, hostname);
                const stream = simulcastManager.getStream(sessionId);
                setStream(stream || null);
                
                // Load video element
                if (videoRef.current && streamUrl) {
                    videoRef.current.src = streamUrl;
                    videoRef.current.load();
                }
            } catch (e) {
                console.error(`[SimulcastPreview] Failed to load stream for ${sessionId}:`, e);
            } finally {
                setIsLoading(false);
            }
        };

        loadStream();

        // Update stream when it changes
        const interval = setInterval(() => {
            const currentStream = simulcastManager.getStream(sessionId);
            if (currentStream) {
                setStream(currentStream);
                if (videoRef.current && currentStream.streamUrl !== videoRef.current.src) {
                    videoRef.current.src = currentStream.streamUrl;
                    videoRef.current.load();
                }
            }
        }, 1000);

        return () => clearInterval(interval);
    }, [sessionId, hostname]);

    const handleContextMenu = (e: React.MouseEvent) => {
        e.preventDefault();
        e.stopPropagation();

        const menuItems: ContextMenuItem[] = [
            {
                id: 'simulcast-fast',
                label: 'SIMUCAST FAST HW VOLATILE',
                action: async () => {
                    try {
                        await simulcastManager.switchToFast(sessionId);
                        const updated = simulcastManager.getStream(sessionId);
                        setStream(updated || null);
                        onQualityChange?.(sessionId, 'fast');
                    } catch (e) {
                        console.error('[SimulcastPreview] Failed to switch to fast:', e);
                    }
                },
            },
            {
                id: 'simulcast-heavy',
                label: 'SIMUCAST HEAVY, HW SAFE',
                action: async () => {
                    try {
                        await simulcastManager.switchToHeavy(sessionId);
                        const updated = simulcastManager.getStream(sessionId);
                        setStream(updated || null);
                        onQualityChange?.(sessionId, 'heavy');
                    } catch (e) {
                        console.error('[SimulcastPreview] Failed to switch to heavy:', e);
                    }
                },
            },
        ];

        showMenu(e, menuItems);
    };

    return (
        <div 
            className={styles.container}
            onContextMenu={handleContextMenu}
        >
            <div className={styles.header}>
                <Monitor size={12} />
                <span className={styles.hostname}>{hostname.substring(0, 12)}</span>
                {stream?.isLocalMaster && (
                    <span className={styles.masterBadge}>MASTER</span>
                )}
            </div>
            <div className={styles.videoContainer}>
                {isLoading ? (
                    <div className={styles.loading}>
                        <div className={styles.spinner} />
                    </div>
                ) : (
                    <video
                        ref={videoRef}
                        className={styles.video}
                        autoPlay
                        muted
                        playsInline
                        width={stream?.width || 150}
                        height={stream?.height || 84}
                    />
                )}
            </div>
            {stream && (
                <div className={styles.info}>
                    <span className={styles.quality}>{stream.quality.toUpperCase()}</span>
                    <span className={styles.codec}>{stream.codec.toUpperCase()}</span>
                    {stream.hardwareAccel !== 'none' && (
                        <span className={styles.hw}>{stream.hardwareAccel.toUpperCase()}</span>
                    )}
                </div>
            )}
        </div>
    );
}

interface SimulcastPreviewGridProps {
    localSessionId: string;
    localHostname: string;
}

export function SimulcastPreviewGrid({ localSessionId, localHostname }: SimulcastPreviewGridProps) {
    const [registeredInstances, setRegisteredInstances] = useState<any[]>([]);

    useEffect(() => {
        if (!trackerApi.isEnabled()) return;

        const loadInstances = async () => {
            try {
                const instances = await trackerApi.getRegisteredInstances();
                setRegisteredInstances(instances || []);
            } catch (err) {
                console.warn('[SimulcastPreviewGrid] Failed to load instances:', err);
            }
        };

        loadInstances();
        const interval = setInterval(loadInstances, 5000);
        return () => clearInterval(interval);
    }, []);

    // Filter out local session from remote instances
    const remoteInstances = registeredInstances.filter(
        inst => inst.hostname !== localHostname
    );

    return (
        <div className={styles.grid}>
            {/* Local session */}
            <SimulcastPreview
                key={localSessionId}
                sessionId={localSessionId}
                hostname={localHostname}
            />
            
            {/* Remote sessions */}
            {remoteInstances.map(inst => (
                <SimulcastPreview
                    key={inst.hostname}
                    sessionId={inst.hostname}
                    hostname={inst.hostname}
                />
            ))}
        </div>
    );
}

