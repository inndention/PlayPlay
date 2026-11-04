import React, { useState, useEffect } from 'react';
import { PanelCommand, SceneCollection, PanelButton, createVideoButton, createSceneCollection } from '../components/PanelCommand';
import { MediaItem } from '../lib/types';

export function PanelCommandPage() {
    const [sceneCollections, setSceneCollections] = useState<SceneCollection[]>([]);
    const [mediaItems, setMediaItems] = useState<MediaItem[]>([]);

    // Load media items for video buttons
    useEffect(() => {
        const loadMedia = async () => {
            try {
                // Load from various sources
                const { loadLocalMedia } = await import('../lib/storage');
                const { loadLibraryMedia } = await import('../lib/storage');
                const { dbFetchMedia } = await import('../lib/mediaDbClient');

                const [dbMedia, localMedia, storedLibrary] = await Promise.all([
                    dbFetchMedia().catch(() => []),
                    Promise.resolve(loadLocalMedia()),
                    Promise.resolve(loadLibraryMedia([])),
                ]);

                setMediaItems([...storedLibrary, ...localMedia, ...dbMedia]);
            } catch (error) {
                console.error('[PanelCommand] Failed to load media:', error);
            }
        };

        loadMedia();
    }, []);

    // Create default scene collections
    useEffect(() => {
        if (mediaItems.length > 0 && sceneCollections.length === 0) {
            const collections: SceneCollection[] = [
                // Playout collection
                createSceneCollection('playout', 'Playout Control', 'playout', [
                    { id: 'play', label: 'Play', type: 'action', action: 'play', icon: '▶' },
                    { id: 'pause', label: 'Pause', type: 'action', action: 'pause', icon: '⏸' },
                    { id: 'stop', label: 'Stop', type: 'action', action: 'stop', icon: '⏹' },
                    { id: 'next', label: 'Next', type: 'action', action: 'next', icon: '⏭' },
                    { id: 'prev', label: 'Previous', type: 'action', action: 'previous', icon: '⏮' },
                ]),

                // Media collection with video buttons
                createSceneCollection('media', 'Media Library', 'media', 
                    mediaItems.slice(0, 12).map((item, idx) => {
                        if (item.type === 'video' && item.src) {
                            return createVideoButton(
                                `media-${item.id}`,
                                item.title || `Media ${idx + 1}`,
                                item.src,
                                {
                                    mediaId: item.id,
                                    action: 'playMedia',
                                    thumbnailUrl: item.thumbnail,
                                }
                            );
                        } else if (item.type === 'image' && item.src) {
                            return {
                                id: `media-${item.id}`,
                                label: item.title || `Image ${idx + 1}`,
                                type: 'image' as const,
                                imageUrl: item.src,
                                mediaId: item.id,
                                action: 'showImage',
                            };
                        } else {
                            return {
                                id: `media-${item.id}`,
                                label: item.title || `Item ${idx + 1}`,
                                type: 'action' as const,
                                action: 'playMedia',
                                mediaId: item.id,
                            };
                        }
                    })
                ),

                // CG collection
                createSceneCollection('cg', 'CG Control', 'cg', [
                    { id: 'cg-show', label: 'Show CG', type: 'action', action: 'showCG', icon: '📺' },
                    { id: 'cg-hide', label: 'Hide CG', type: 'action', action: 'hideCG', icon: '📴' },
                    { id: 'cg-next', label: 'Next Layer', type: 'action', action: 'nextCGLayer', icon: '⏭' },
                ]),

                // OBS collection
                createSceneCollection('obs', 'OBS Control', 'obs', [
                    { id: 'obs-scene1', label: 'Scene 1', type: 'action', action: 'obs:switchScene', icon: '🎬' },
                    { id: 'obs-scene2', label: 'Scene 2', type: 'action', action: 'obs:switchScene', icon: '🎬' },
                    { id: 'obs-transition', label: 'Transition', type: 'action', action: 'obs:transition', icon: '🔄' },
                ]),
            ];

            setSceneCollections(collections);
        }
    }, [mediaItems, sceneCollections.length]);

    const handleButtonClick = (button: PanelButton) => {
        console.log('[PanelCommand] Button clicked:', button);
        // Handle button actions
        if (button.action) {
            handleAction(button.action, { button, mediaId: button.mediaId });
        }
    };

    const handleAction = async (action: string, data?: any) => {
        console.log('[PanelCommand] Action:', action, data);

        // Send action to main process via IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI) {
            try {
                switch (action) {
                    case 'play':
                        await (window as any).electronAPI.sendPlaylistCommand?.('play');
                        break;
                    case 'pause':
                        await (window as any).electronAPI.sendPlaylistCommand?.('pause');
                        break;
                    case 'stop':
                        await (window as any).electronAPI.sendPlaylistCommand?.('stop');
                        break;
                    case 'next':
                        await (window as any).electronAPI.sendPlaylistCommand?.('next');
                        break;
                    case 'previous':
                        await (window as any).electronAPI.sendPlaylistCommand?.('previous');
                        break;
                    case 'playMedia':
                        if (data?.mediaId) {
                            await (window as any).electronAPI.addMediaToPlaylist?.(data.mediaId);
                        }
                        break;
                    default:
                        console.log('[PanelCommand] Unknown action:', action);
                }
            } catch (error) {
                console.error('[PanelCommand] Action failed:', error);
            }
        }

        // Also emit custom event for other components
        window.dispatchEvent(new CustomEvent('panelcommand:action', {
            detail: { action, data },
        }));
    };

    return (
        <div style={{ width: '100vw', height: '100vh', overflow: 'hidden' }}>
            <PanelCommand
                sceneCollections={sceneCollections}
                defaultCollection={sceneCollections[0]?.id}
                onButtonClick={handleButtonClick}
                onAction={handleAction}
                serverPort={8080}
            />
        </div>
    );
}

