import React, { useState, useEffect, useRef, useCallback } from 'react';
import { MediaItem, PlaylistItem } from '../lib/types';
import { fitRect } from '../lib/fitRect';
import './MultiviewStream.module.css';

export interface VideoSource {
    id: string;
    name: string;
    type: 'playout' | 'ddr1' | 'ddr2' | 'preview' | 'output' | 'obs' | 'ndi' | 'custom';
    url?: string;
    element?: HTMLVideoElement;
    position: { x: number; y: number; width: number; height: number }; // Normalized 0-1
    zIndex?: number;
    controls?: {
        play?: boolean;
        pause?: boolean;
        seek?: boolean;
        volume?: boolean;
        next?: boolean;
        previous?: boolean;
    };
}

export interface MultiviewLayout {
    id: string;
    name: string;
    sources: VideoSource[];
    background?: string;
    grid?: { rows: number; cols: number };
}

interface MultiviewStreamProps {
    sources: VideoSource[];
    layout?: MultiviewLayout;
    onSourceClick?: (source: VideoSource, event: { x: number; y: number }) => void;
    onControl?: (sourceId: string, action: string, value?: any) => void;
    streaming?: boolean;
    streamUrl?: string;
    interactive?: boolean;
}

export function MultiviewStream({
    sources,
    layout,
    onSourceClick,
    onControl,
    streaming = false,
    streamUrl,
    interactive = true,
}: MultiviewStreamProps) {
    const containerRef = useRef<HTMLDivElement>(null);
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const videoRefs = useRef<Map<string, HTMLVideoElement>>(new Map());
    const [activeSource, setActiveSource] = useState<string | null>(null);
    const [compositeStream, setCompositeStream] = useState<MediaStream | null>(null);
    const [isCompositing, setIsCompositing] = useState(false);

    // Setup video elements for each source
    useEffect(() => {
        sources.forEach(source => {
            if (source.url && !videoRefs.current.has(source.id)) {
                const video = document.createElement('video');
                video.src = source.url;
                video.autoplay = true;
                video.muted = true; // Mute individual sources, mix audio separately
                video.playsInline = true;
                video.loop = false;
                videoRefs.current.set(source.id, video);
            }
        });

        return () => {
            videoRefs.current.forEach(video => {
                video.pause();
                video.src = '';
            });
            videoRefs.current.clear();
        };
    }, [sources]);

    // Composite all sources onto canvas
    useEffect(() => {
        if (!canvasRef.current || !isCompositing) return;

        const canvas = canvasRef.current;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;

        // Set canvas size (match output resolution)
        canvas.width = 1920;
        canvas.height = 1080;

        const drawFrame = () => {
            if (!isCompositing) return;

            // Clear canvas
            ctx.fillStyle = '#000';
            ctx.fillRect(0, 0, canvas.width, canvas.height);

            // Draw each source
            sources.forEach(source => {
                const video = videoRefs.current.get(source.id);
                if (!video || video.readyState < 2) return;

                const { x, y, width, height } = source.position;
                const slot = { x, y, w: width, h: height };

                // Use real decoded dimensions when available; fall back to slot size.
                const srcW = video.videoWidth  || Math.round(width  * canvas.width);
                const srcH = video.videoHeight || Math.round(height * canvas.height);

                // Aspect safety rule: never map media directly into slot dimensions.
                // Always pass through fitRect so portrait / ultra-wide content is
                // contained (pillarboxed) rather than stretched.
                const fitted = fitRect(srcW, srcH, canvas.width, canvas.height, slot, 'contain');

                const destX      = fitted.x * canvas.width;
                const destY      = fitted.y * canvas.height;
                const destWidth  = fitted.w * canvas.width;
                const destHeight = fitted.h * canvas.height;

                // Draw video frame
                ctx.drawImage(video, destX, destY, destWidth, destHeight);

                // Draw source label
                ctx.fillStyle = 'rgba(0, 0, 0, 0.7)';
                ctx.fillRect(destX, destY, destWidth, 30);
                ctx.fillStyle = '#fff';
                ctx.font = '16px Arial';
                ctx.fillText(source.name, destX + 10, destY + 20);

                // Draw active indicator
                if (activeSource === source.id) {
                    ctx.strokeStyle = '#3b82f6';
                    ctx.lineWidth = 4;
                    ctx.strokeRect(destX, destY, destWidth, destHeight);
                }
            });

            requestAnimationFrame(drawFrame);
        };

        drawFrame();
    }, [sources, isCompositing, activeSource]);

    // Start compositing stream
    const startCompositing = useCallback(() => {
        if (!canvasRef.current) return;

        setIsCompositing(true);

        // Start streaming via Electron if needed
        if (streaming && typeof window !== 'undefined' && (window as any).electronAPI?.startMultiviewStream) {
            // The actual streaming will be handled by Electron using the canvas
            // We just need to mark that we're compositing
            (window as any).electronAPI.startMultiviewStream().catch(console.error);
        }
    }, [streaming]);

    // Stop compositing
    const stopCompositing = useCallback(() => {
        setIsCompositing(false);
        if (compositeStream) {
            compositeStream.getTracks().forEach(track => track.stop());
            setCompositeStream(null);
        }
        if (streaming && typeof window !== 'undefined' && (window as any).electronAPI?.stopMultiviewStream) {
            (window as any).electronAPI.stopMultiviewStream().catch(console.error);
        }
    }, [compositeStream, streaming]);


    // Handle click on source region
    const handleClick = useCallback((e: React.MouseEvent<HTMLDivElement>) => {
        if (!interactive || !containerRef.current) return;

        const rect = containerRef.current.getBoundingClientRect();
        const x = (e.clientX - rect.left) / rect.width;
        const y = (e.clientY - rect.top) / rect.height;

        // Find which source was clicked
        const clickedSource = sources.find(source => {
            const { x: sx, y: sy, width, height } = source.position;
            return x >= sx && x <= sx + width && y >= sy && y <= sy + height;
        });

        if (clickedSource) {
            setActiveSource(clickedSource.id);
            if (onSourceClick) {
                onSourceClick(clickedSource, { x, y });
            }
        }
    }, [interactive, sources, onSourceClick]);

    // Handle touch for mobile
    const handleTouch = useCallback((e: React.TouchEvent<HTMLDivElement>) => {
        if (!interactive || !containerRef.current || e.touches.length === 0) return;

        const touch = e.touches[0];
        const rect = containerRef.current.getBoundingClientRect();
        const x = (touch.clientX - rect.left) / rect.width;
        const y = (touch.clientY - rect.top) / rect.height;

        const touchedSource = sources.find(source => {
            const { x: sx, y: sy, width, height } = source.position;
            return x >= sx && x <= sx + width && y >= sy && y <= sy + height;
        });

        if (touchedSource) {
            setActiveSource(touchedSource.id);
            if (onSourceClick) {
                onSourceClick(touchedSource, { x, y });
            }
        }
    }, [interactive, sources, onSourceClick]);

    // Control actions
    const handleControl = useCallback((action: string, value?: any) => {
        if (activeSource && onControl) {
            onControl(activeSource, action, value);
        }
    }, [activeSource, onControl]);

    return (
        <div className="multiview-stream">
            <div className="multiview-controls">
                <button onClick={isCompositing ? stopCompositing : startCompositing}>
                    {isCompositing ? 'Stop Stream' : 'Start Stream'}
                </button>
                {streamUrl && (
                    <div className="stream-url">
                        <label>Stream URL:</label>
                        <code>{streamUrl}</code>
                    </div>
                )}
            </div>

            <div
                ref={containerRef}
                className="multiview-container"
                onClick={handleClick}
                onTouchStart={handleTouch}
            >
                <canvas ref={canvasRef} className="multiview-canvas" />
                
                {/* Interaction overlay */}
                {interactive && (
                    <div className="interaction-overlay">
                        {sources.map(source => (
                            <div
                                key={source.id}
                                className={`source-region ${activeSource === source.id ? 'active' : ''}`}
                                style={{
                                    position: 'absolute',
                                    left: `${source.position.x * 100}%`,
                                    top: `${source.position.y * 100}%`,
                                    width: `${source.position.width * 100}%`,
                                    height: `${source.position.height * 100}%`,
                                    border: activeSource === source.id ? '3px solid #3b82f6' : '1px solid rgba(255,255,255,0.3)',
                                    pointerEvents: 'auto',
                                    cursor: 'pointer',
                                }}
                            />
                        ))}
                    </div>
                )}

                {/* Control panel for active source */}
                {activeSource && interactive && (
                    <div className="source-controls">
                        {sources.find(s => s.id === activeSource)?.controls?.play && (
                            <button onClick={() => handleControl('play')}>▶ Play</button>
                        )}
                        {sources.find(s => s.id === activeSource)?.controls?.pause && (
                            <button onClick={() => handleControl('pause')}>⏸ Pause</button>
                        )}
                        {sources.find(s => s.id === activeSource)?.controls?.seek && (
                            <input
                                type="range"
                                min="0"
                                max="100"
                                onChange={(e) => handleControl('seek', parseFloat(e.target.value))}
                            />
                        )}
                        {sources.find(s => s.id === activeSource)?.controls?.volume && (
                            <input
                                type="range"
                                min="0"
                                max="100"
                                onChange={(e) => handleControl('volume', parseFloat(e.target.value))}
                            />
                        )}
                        {sources.find(s => s.id === activeSource)?.controls?.next && (
                            <button onClick={() => handleControl('next')}>⏭ Next</button>
                        )}
                        {sources.find(s => s.id === activeSource)?.controls?.previous && (
                            <button onClick={() => handleControl('previous')}>⏮ Prev</button>
                        )}
                    </div>
                )}
            </div>
        </div>
    );
}

// Helper to create common layouts
export function createMultiviewLayout(name: string, sourceCount: number): MultiviewLayout {
    const sources: VideoSource[] = [];
    
    if (sourceCount === 1) {
        sources.push({
            id: 'source-1',
            name: 'Source 1',
            type: 'playout',
            position: { x: 0, y: 0, width: 1, height: 1 },
        });
    } else if (sourceCount === 2) {
        sources.push(
            { id: 'source-1', name: 'Source 1', type: 'playout', position: { x: 0, y: 0, width: 0.5, height: 1 } },
            { id: 'source-2', name: 'Source 2', type: 'ddr1', position: { x: 0.5, y: 0, width: 0.5, height: 1 } }
        );
    } else if (sourceCount === 4) {
        sources.push(
            { id: 'source-1', name: 'Program', type: 'playout', position: { x: 0, y: 0, width: 0.5, height: 0.5 } },
            { id: 'source-2', name: 'DDR1', type: 'ddr1', position: { x: 0.5, y: 0, width: 0.5, height: 0.5 } },
            { id: 'source-3', name: 'DDR2', type: 'ddr2', position: { x: 0, y: 0.5, width: 0.5, height: 0.5 } },
            { id: 'source-4', name: 'Preview', type: 'preview', position: { x: 0.5, y: 0.5, width: 0.5, height: 0.5 } }
        );
    } else {
        // Grid layout
        const cols = Math.ceil(Math.sqrt(sourceCount));
        const rows = Math.ceil(sourceCount / cols);
        const cellWidth = 1 / cols;
        const cellHeight = 1 / rows;

        for (let i = 0; i < sourceCount; i++) {
            const col = i % cols;
            const row = Math.floor(i / cols);
            sources.push({
                id: `source-${i + 1}`,
                name: `Source ${i + 1}`,
                type: 'playout',
                position: {
                    x: col * cellWidth,
                    y: row * cellHeight,
                    width: cellWidth,
                    height: cellHeight,
                },
            });
        }
    }

    return {
        id: `layout-${name.toLowerCase()}`,
        name,
        sources,
    };
}

