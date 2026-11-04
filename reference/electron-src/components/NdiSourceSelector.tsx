/**
 * NDI Source Selector
 * Auto-discovery and selection of NDI sources
 */

import React, { useState, useEffect, useCallback } from 'react';
import { RefreshCw, Wifi, WifiOff } from 'lucide-react';
import type { NdiSource } from '../lib/types';
import { NdiEngine } from '../lib/ndiEngine';
import styles from './NdiSourceSelector.module.css';

const getAPI = () => (window as any).electronAPI;

interface Props {
    selectedSource?: string;
    onSelect: (sourceName: string) => void;
}

export function NdiSourceSelector({ selectedSource, onSelect }: Props) {
    const [sources, setSources] = useState<NdiSource[]>([]);
    const [isDiscovering, setIsDiscovering] = useState(false);
    const [ndiEngine] = useState(() => new NdiEngine(getAPI()));

    useEffect(() => {
        // Start discovery
        ndiEngine.startDiscovery(5000);
        ndiEngine.discoverSources().then(setSources);

        // Subscribe to updates
        const unsubscribe = ndiEngine.onSourcesUpdate((updatedSources) => {
            setSources(updatedSources);
        });

        return () => {
            unsubscribe();
            ndiEngine.stopDiscovery();
        };
    }, [ndiEngine]);

    const handleRefresh = useCallback(async () => {
        setIsDiscovering(true);
        const discovered = await ndiEngine.discoverSources();
        setSources(discovered);
        setIsDiscovering(false);
    }, [ndiEngine]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <label>NDI Source</label>
                <button
                    className={styles.refreshButton}
                    onClick={handleRefresh}
                    disabled={isDiscovering}
                    title="Refresh NDI sources"
                >
                    <RefreshCw size={14} className={isDiscovering ? styles.spinning : ''} />
                </button>
            </div>
            <select
                value={selectedSource || ''}
                onChange={(e) => onSelect(e.target.value)}
                className={styles.select}
            >
                <option value="">Select NDI source...</option>
                {sources.map(source => (
                    <option key={source.name} value={source.name}>
                        {source.name}
                        {source.isAvailable ? ' ●' : ' ○'}
                        {source.resolution && ` (${source.resolution})`}
                    </option>
                ))}
            </select>
            {sources.length === 0 && (
                <div className={styles.empty}>
                    <WifiOff size={16} />
                    <span>No NDI sources found</span>
                </div>
            )}
        </div>
    );
}




