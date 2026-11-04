import React, { useState, useEffect, useRef, useCallback } from 'react';
import { MediaItem, PlaylistItem } from '../lib/types';
import './PanelCommand.module.css';

export type PanelMode = 'playout' | 'cg' | 'obs' | 'router' | 'ddr' | 'media' | 'custom';
export type SceneCollection = {
    id: string;
    name: string;
    mode: PanelMode;
    buttons: PanelButton[];
};

export interface PanelButton {
    id: string;
    label: string;
    type: 'action' | 'media' | 'video' | 'image' | 'animation';
    action?: string; // Action to execute
    mediaId?: string; // Media item ID
    videoUrl?: string; // Direct video URL
    imageUrl?: string; // Image URL
    animationUrl?: string; // Animation/GIF URL
    thumbnailUrl?: string; // Thumbnail for video buttons
    color?: string;
    icon?: string;
    // Video button settings
    videoSettings?: {
        resolution?: 'ultra-low' | 'low' | 'medium'; // Default: ultra-low
        framerate?: number; // Default: 1-2 fps for ultra-low
        loop?: boolean;
        autoplay?: boolean;
        muted?: boolean;
    };
}

interface PanelCommandProps {
    onButtonClick?: (button: PanelButton) => void;
    onAction?: (action: string, data?: any) => void;
    sceneCollections?: SceneCollection[];
    defaultCollection?: string;
    serverPort?: number; // For mobile access
}

export function PanelCommand({
    onButtonClick,
    onAction,
    sceneCollections = [],
    defaultCollection,
    serverPort = 8080,
}: PanelCommandProps) {
    const [activeCollection, setActiveCollection] = useState<string | null>(defaultCollection || null);
    const [buttons, setButtons] = useState<PanelButton[]>([]);
    const [isMobile, setIsMobile] = useState(false);
    const videoRefs = useRef<Map<string, HTMLVideoElement>>(new Map());
    const canvasRefs = useRef<Map<string, HTMLCanvasElement>>(new Map());

    // Detect mobile device
    useEffect(() => {
        const checkMobile = () => {
            setIsMobile(/Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent) ||
                window.innerWidth < 768);
        };
        checkMobile();
        window.addEventListener('resize', checkMobile);
        return () => window.removeEventListener('resize', checkMobile);
    }, []);

    // Load buttons for active collection
    useEffect(() => {
        if (activeCollection) {
            const collection = sceneCollections.find(c => c.id === activeCollection);
            if (collection) {
                setButtons(collection.buttons);
            }
        } else if (sceneCollections.length > 0) {
            setActiveCollection(sceneCollections[0].id);
            setButtons(sceneCollections[0].buttons);
        }
    }, [activeCollection, sceneCollections]);

    // Setup video buttons with ultra-low res rendering
    useEffect(() => {
        buttons.forEach(button => {
            if (button.type === 'video' && button.videoUrl) {
                setupVideoButton(button);
            }
        });

        return () => {
            // Cleanup video elements
            videoRefs.current.forEach(video => {
                if (video) {
                    video.pause();
                    video.src = '';
                }
            });
            videoRefs.current.clear();
        };
    }, [buttons]);

    const setupVideoButton = useCallback((button: PanelButton) => {
        const videoId = `video-${button.id}`;
        const canvasId = `canvas-${button.id}`;
        
        const video = document.getElementById(videoId) as HTMLVideoElement;
        const canvas = document.getElementById(canvasId) as HTMLCanvasElement;
        
        if (!video || !canvas) return;

        const settings = button.videoSettings || {};
        const resolution = settings.resolution || 'ultra-low';
        const framerate = settings.framerate || (resolution === 'ultra-low' ? 1 : 2);
        const loop = settings.loop !== false;
        const autoplay = settings.autoplay !== false;
        const muted = settings.muted !== false;

        // Ultra-low res settings
        const resolutions = {
            'ultra-low': { width: 64, height: 36 }, // Tiny!
            'low': { width: 128, height: 72 },
            'medium': { width: 256, height: 144 },
        };

        const { width, height } = resolutions[resolution as keyof typeof resolutions] || resolutions['ultra-low'];
        canvas.width = width;
        canvas.height = height;

        video.loop = loop;
        video.muted = muted;
        video.playsInline = true;
        
        if (autoplay) {
            video.play().catch(() => {
                // Autoplay failed, will play on user interaction
            });
        }

        // Capture frames at low framerate
        let lastFrameTime = 0;
        const frameInterval = 1000 / framerate;

        const captureFrame = () => {
            const now = Date.now();
            if (now - lastFrameTime >= frameInterval && video.readyState >= 2) {
                const ctx = canvas.getContext('2d');
                if (ctx) {
                    ctx.drawImage(video, 0, 0, width, height);
                }
                lastFrameTime = now;
            }
            requestAnimationFrame(captureFrame);
        };

        video.addEventListener('loadedmetadata', () => {
            captureFrame();
        });

        videoRefs.current.set(button.id, video);
        canvasRefs.current.set(button.id, canvas);
    }, []);

    const handleButtonClick = useCallback((button: PanelButton) => {
        if (onButtonClick) {
            onButtonClick(button);
        }

        if (button.action && onAction) {
            onAction(button.action, {
                buttonId: button.id,
                mediaId: button.mediaId,
                ...button,
            });
        }

        // Handle video button click - toggle play/pause
        if (button.type === 'video') {
            const video = videoRefs.current.get(button.id);
            if (video) {
                if (video.paused) {
                    video.play().catch(console.error);
                } else {
                    video.pause();
                }
            }
        }
    }, [onButtonClick, onAction]);

    const renderButton = (button: PanelButton) => {
        const buttonClass = `panel-button panel-button-${button.type}`;
        
        return (
            <button
                key={button.id}
                className={buttonClass}
                onClick={() => handleButtonClick(button)}
                style={{
                    backgroundColor: button.color || '#1e1e1e',
                    position: 'relative',
                    overflow: 'hidden',
                }}
            >
                {/* Video button with ultra-low res canvas overlay */}
                {button.type === 'video' && button.videoUrl && (
                    <>
                        <video
                            id={`video-${button.id}`}
                            src={button.videoUrl}
                            style={{
                                position: 'absolute',
                                top: 0,
                                left: 0,
                                width: '100%',
                                height: '100%',
                                objectFit: 'cover',
                                opacity: 0, // Hidden, canvas shows the low-res version
                            }}
                            playsInline
                            muted
                        />
                        <canvas
                            id={`canvas-${button.id}`}
                            style={{
                                position: 'absolute',
                                top: '50%',
                                left: '50%',
                                transform: 'translate(-50%, -50%)',
                                width: '100%',
                                height: '100%',
                                objectFit: 'cover',
                                imageRendering: 'pixelated', // Pixelated for ultra-low res
                            }}
                        />
                    </>
                )}

                {/* Image button */}
                {button.type === 'image' && button.imageUrl && (
                    <img
                        src={button.imageUrl}
                        alt={button.label}
                        style={{
                            position: 'absolute',
                            top: 0,
                            left: 0,
                            width: '100%',
                            height: '100%',
                            objectFit: 'cover',
                        }}
                    />
                )}

                {/* Animation/GIF button */}
                {button.type === 'animation' && button.animationUrl && (
                    <img
                        src={button.animationUrl}
                        alt={button.label}
                        style={{
                            position: 'absolute',
                            top: 0,
                            left: 0,
                            width: '100%',
                            height: '100%',
                            objectFit: 'cover',
                        }}
                    />
                )}

                {/* Label overlay */}
                <span className="panel-button-label">
                    {button.icon && <span className="panel-button-icon">{button.icon}</span>}
                    {button.label}
                </span>
            </button>
        );
    };

    return (
        <div className={`panel-command ${isMobile ? 'panel-mobile' : ''}`}>
            {/* Collection selector */}
            {sceneCollections.length > 1 && (
                <div className="panel-collections">
                    {sceneCollections.map(collection => (
                        <button
                            key={collection.id}
                            className={`panel-collection-btn ${activeCollection === collection.id ? 'active' : ''}`}
                            onClick={() => setActiveCollection(collection.id)}
                        >
                            {collection.name}
                        </button>
                    ))}
                </div>
            )}

            {/* Button grid */}
            <div className="panel-grid">
                {buttons.map(button => renderButton(button))}
            </div>

            {/* Mobile access info */}
            {isMobile && (
                <div className="panel-mobile-info">
                    <p>Access this panel from any device at:</p>
                    <code>http://{window.location.hostname}:{serverPort}/panel</code>
                </div>
            )}
        </div>
    );
}

// Helper to create scene collections
export function createSceneCollection(
    id: string,
    name: string,
    mode: PanelMode,
    buttons: PanelButton[]
): SceneCollection {
    return { id, name, mode, buttons };
}

// Helper to create video button with ultra-low res
export function createVideoButton(
    id: string,
    label: string,
    videoUrl: string,
    options?: Partial<PanelButton>
): PanelButton {
    return {
        id,
        label,
        type: 'video',
        videoUrl,
        videoSettings: {
            resolution: 'ultra-low',
            framerate: 1,
            loop: true,
            autoplay: true,
            muted: true,
            ...options?.videoSettings,
        },
        ...options,
    };
}



