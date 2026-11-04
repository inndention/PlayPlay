import React, { useState, useEffect, useCallback } from 'react';
import { MultiviewStream, VideoSource, createMultiviewLayout, MultiviewLayout } from '../components/MultiviewStream';
import { PlaylistItem } from '../lib/types';

export function MultiviewPage() {
    const [sources, setSources] = useState<VideoSource[]>([]);
    const [layouts, setLayouts] = useState<MultiviewLayout[]>([]);
    const [activeLayout, setActiveLayout] = useState<string | null>(null);
    const [streamUrl, setStreamUrl] = useState<string | null>(null);
    const [isStreaming, setIsStreaming] = useState(false);

    // Update stream URL when streaming starts
    useEffect(() => {
        if (isStreaming) {
            const getUrl = async () => {
                try {
                    if (typeof window !== 'undefined' && (window as any).electronAPI?.getMultiviewStreamUrl) {
                        const url = await (window as any).electronAPI.getMultiviewStreamUrl();
                        setStreamUrl(url);
                    }
                } catch (error) {
                    console.error('[Multiview] Failed to get stream URL:', error);
                }
            };
            getUrl();
        }
    }, [isStreaming]);

    // Load layouts
    useEffect(() => {
        const defaultLayouts = [
            createMultiviewLayout('Single', 1),
            createMultiviewLayout('Split', 2),
            createMultiviewLayout('Quad', 4),
            createMultiviewLayout('Grid 6', 6),
            createMultiviewLayout('Grid 9', 9),
        ];
        setLayouts(defaultLayouts);
        if (defaultLayouts.length > 0) {
            setActiveLayout(defaultLayouts[0].id);
        }
    }, []);

    // Setup video sources from playout state
    useEffect(() => {
        const setupSources = async () => {
            try {
                // Get playout state
                const response = await fetch('http://localhost:8080/api/state');
                const state = await response.json();

                const newSources: VideoSource[] = [
                    {
                        id: 'program',
                        name: 'Program',
                        type: 'playout',
                        url: state.currentMedia?.src || undefined,
                        position: { x: 0, y: 0, width: 0.5, height: 0.5 },
                        controls: {
                            play: true,
                            pause: true,
                            seek: true,
                            volume: true,
                            next: true,
                            previous: true,
                        },
                    },
                    {
                        id: 'preview',
                        name: 'Preview',
                        type: 'preview',
                        url: state.nextMedia?.src || undefined,
                        position: { x: 0.5, y: 0, width: 0.5, height: 0.5 },
                        controls: {
                            play: true,
                            pause: true,
                        },
                    },
                    {
                        id: 'ddr1',
                        name: 'DDR 1',
                        type: 'ddr1',
                        position: { x: 0, y: 0.5, width: 0.5, height: 0.5 },
                        controls: {
                            play: true,
                            pause: true,
                            seek: true,
                        },
                    },
                    {
                        id: 'ddr2',
                        name: 'DDR 2',
                        type: 'ddr2',
                        position: { x: 0.5, y: 0.5, width: 0.5, height: 0.5 },
                        controls: {
                            play: true,
                            pause: true,
                            seek: true,
                        },
                    },
                ];

                setSources(newSources);
            } catch (error) {
                console.error('[Multiview] Failed to setup sources:', error);
            }
        };

        setupSources();
        const interval = setInterval(setupSources, 5000); // Update every 5 seconds
        return () => clearInterval(interval);
    }, []);


    const handleSourceClick = useCallback((source: VideoSource, event: { x: number; y: number }) => {
        console.log('[Multiview] Source clicked:', source.id, event);
    }, []);

    const handleControl = useCallback(async (sourceId: string, action: string, value?: any) => {
        console.log('[Multiview] Control:', sourceId, action, value);

        try {
            if (typeof window !== 'undefined' && (window as any).electronAPI) {
                switch (action) {
                    case 'play':
                        if (sourceId === 'program') {
                            await (window as any).electronAPI.sendPlaylistCommand?.('play');
                        }
                        break;
                    case 'pause':
                        if (sourceId === 'program') {
                            await (window as any).electronAPI.sendPlaylistCommand?.('pause');
                        }
                        break;
                    case 'seek':
                        if (sourceId === 'program' && value !== undefined) {
                            // Seek to percentage
                            await (window as any).electronAPI.seekPlaylist?.(value);
                        }
                        break;
                    case 'volume':
                        if (value !== undefined) {
                            await (window as any).electronAPI.setVolume?.(value / 100);
                        }
                        break;
                    case 'next':
                        if (sourceId === 'program') {
                            await (window as any).electronAPI.sendPlaylistCommand?.('next');
                        }
                        break;
                    case 'previous':
                        if (sourceId === 'program') {
                            await (window as any).electronAPI.sendPlaylistCommand?.('previous');
                        }
                        break;
                }
            }
        } catch (error) {
            console.error('[Multiview] Control failed:', error);
        }
    }, []);

    const currentLayout = layouts.find(l => l.id === activeLayout);

    return (
        <div style={{ width: '100vw', height: '100vh', display: 'flex', flexDirection: 'column', background: '#000' }}>
            {/* Layout selector */}
            <div style={{ padding: '12px', background: '#1a1a1a', borderBottom: '1px solid #2a2a2a', display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
                {layouts.map(layout => (
                    <button
                        key={layout.id}
                        onClick={() => setActiveLayout(layout.id)}
                        style={{
                            padding: '6px 12px',
                            background: activeLayout === layout.id ? '#3b82f6' : '#2a2a2a',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer',
                            fontSize: '12px',
                        }}
                    >
                        {layout.name}
                    </button>
                ))}
            </div>

            {/* Multiview stream */}
            <MultiviewStream
                sources={currentLayout?.sources || sources}
                layout={currentLayout}
                onSourceClick={handleSourceClick}
                onControl={handleControl}
                streaming={isStreaming}
                streamUrl={streamUrl || undefined}
                interactive={true}
            />
        </div>
    );
}

