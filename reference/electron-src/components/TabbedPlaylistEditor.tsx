import React, { useState, useEffect } from 'react';
import { PlaylistEditor } from './PlaylistEditor';
import { PlaylistItem } from '../lib/types';
import { trackerApi } from '../lib/trackerApi';
import { Monitor, Server } from 'lucide-react';
import styles from './TabbedPlaylistEditor.module.css';

interface TabbedPlaylistEditorProps {
    playlists: Map<string, PlaylistItem[]>; // sessionId -> playlist
    setPlaylist: (sessionId: string, items: PlaylistItem[]) => void;
    onRemove: (sessionId: string, id: string) => void;
    onSetFixedTime?: (sessionId: string, id: string, time: number | null) => void;
    activeItemIds?: Map<string, string | null>; // sessionId -> activeItemId
    onItemContextMenu?: (e: React.MouseEvent, item: PlaylistItem, sessionId: string) => void;
}

interface RegisteredInstance {
    hostname: string;
    siteId?: string;
    playerId?: string;
    lastSeen?: number;
}

export function TabbedPlaylistEditor({
    playlists,
    setPlaylist,
    onRemove,
    onSetFixedTime,
    activeItemIds = new Map(),
    onItemContextMenu
}: TabbedPlaylistEditorProps) {
    const [activeTab, setActiveTab] = useState<string>('local');
    const [registeredInstances, setRegisteredInstances] = useState<RegisteredInstance[]>([]);
    const [localHostname, setLocalHostname] = useState<string>('local');

    // Load hostname
    useEffect(() => {
        const loadHostname = async () => {
            const api = (window as any).electronAPI;
            if (api?.getHostname) {
                try {
                    const h = await api.getHostname();
                    setLocalHostname(h || 'local');
                    setActiveTab(h || 'local');
                } catch (e) {
                    console.error('Failed to get hostname:', e);
                }
            }
        };
        loadHostname();
    }, []);

    // Load registered instances from tracker
    useEffect(() => {
        if (!trackerApi.isEnabled()) return;

        const loadInstances = async () => {
            try {
                const instances = await trackerApi.getRegisteredInstances();
                setRegisteredInstances(instances || []);
            } catch (err) {
                console.warn('[TabbedPlaylistEditor] Failed to load instances:', err);
            }
        };

        loadInstances();
        const interval = setInterval(loadInstances, 5000); // Refresh every 5 seconds
        return () => clearInterval(interval);
    }, []);

    // Get all available session IDs (local + registered instances)
    const availableSessions = [
        { id: 'local', hostname: localHostname, siteId: undefined, playerId: undefined },
        ...registeredInstances.map(inst => ({
            id: inst.hostname,
            hostname: inst.hostname,
            siteId: inst.siteId,
            playerId: inst.playerId,
        }))
    ];

    // Get display name for a session
    const getSessionDisplayName = (session: typeof availableSessions[0]) => {
        if (session.id === 'local') {
            return 'Local';
        }
        if (session.siteId && session.playerId) {
            return `${session.siteId} / ${session.playerId}`;
        }
        if (session.siteId) {
            return session.siteId;
        }
        return session.hostname.substring(0, 12);
    };

    // Get session status (online/offline)
    const isSessionOnline = (sessionId: string) => {
        if (sessionId === 'local') return true;
        const instance = registeredInstances.find(inst => inst.hostname === sessionId);
        if (!instance) return false;
        // Consider online if seen in last 30 seconds
        if (instance.lastSeen) {
            return (Date.now() - instance.lastSeen) < 30000;
        }
        return true; // Assume online if we have the instance
    };

    const handleSetPlaylist = (sessionId: string) => (items: PlaylistItem[]) => {
        setPlaylist(sessionId, items);
    };

    const handleRemove = (sessionId: string) => (id: string) => {
        onRemove(sessionId, id);
    };

    const handleSetFixedTime = (sessionId: string) => (id: string, time: number | null) => {
        onSetFixedTime?.(sessionId, id, time);
    };

    const handleContextMenu = (sessionId: string) => (e: React.MouseEvent, item: PlaylistItem) => {
        onItemContextMenu?.(e, item, sessionId);
    };

    const currentPlaylist = playlists.get(activeTab) || [];
    const currentActiveItemId = activeItemIds.get(activeTab) || null;

    return (
        <div className={styles.container}>
            <div className={styles.tabs}>
                {availableSessions.map(session => {
                    const isOnline = isSessionOnline(session.id);
                    const isActive = activeTab === session.id;
                    
                    return (
                        <button
                            key={session.id}
                            className={`${styles.tab} ${isActive ? styles.tabActive : ''} ${!isOnline ? styles.tabOffline : ''}`}
                            onClick={() => setActiveTab(session.id)}
                            title={session.hostname}
                        >
                            {isOnline ? (
                                <Monitor size={14} />
                            ) : (
                                <Server size={14} />
                            )}
                            <span>{getSessionDisplayName(session)}</span>
                            {!isOnline && <span className={styles.offlineBadge}>OFF</span>}
                        </button>
                    );
                })}
            </div>
            <div className={styles.editorContainer}>
                <PlaylistEditor
                    playlist={currentPlaylist}
                    setPlaylist={handleSetPlaylist(activeTab)}
                    onRemove={handleRemove(activeTab)}
                    onSetFixedTime={handleSetFixedTime(activeTab)}
                    activeItemId={currentActiveItemId}
                    onItemContextMenu={handleContextMenu(activeTab)}
                />
            </div>
        </div>
    );
}




