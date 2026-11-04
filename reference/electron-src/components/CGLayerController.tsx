// ============================================
// CG Layer Controller
// UI for managing 4 CG layers per channel
// ============================================

import React, { useState, useEffect, useCallback } from 'react';
import { CGLayerConfig, ChannelCGConfig, CGElement } from '../lib/types';
import { showLayer, hideLayer, updateLayer, getAllChannelCGConfigs } from '../lib/obsCGController';
import { loadCGElements } from '../lib/storage';
import { Eye, EyeOff, Edit2, Send, Layers } from 'lucide-react';
import styles from './CGLayerController.module.css';

interface CGLayerControllerProps {
    channelId: string;
    obsSessionId?: string;
}

export function CGLayerController({ channelId, obsSessionId }: CGLayerControllerProps) {
    const [config, setConfig] = useState<ChannelCGConfig | null>(null);
    const [availableElements, setAvailableElements] = useState<CGElement[]>([]);
    const [selectedLayer, setSelectedLayer] = useState<number | null>(null);

    // Load channel CG config
    useEffect(() => {
        const load = async () => {
            const configs = await getAllChannelCGConfigs();
            setConfig(configs[channelId] || null);
        };
        load();
        
        // Load available CG elements
        setAvailableElements(loadCGElements());
    }, [channelId]);

    const handleToggleLayer = useCallback(async (layerId: number) => {
        if (!config) return;
        
        const layer = config.layers.find(l => l.layerId === layerId);
        if (!layer) return;

        if (layer.visible) {
            await hideLayer(channelId, layerId, obsSessionId);
        } else {
            // Show layer with existing content or empty
            await showLayer(channelId, layerId, layer.content, obsSessionId);
        }
        
        // Reload config
        const configs = await getAllChannelCGConfigs();
        setConfig(configs[channelId] || null);
    }, [channelId, config, obsSessionId]);

    const handleSendElement = useCallback(async (layerId: number, element: CGElement) => {
        await updateLayer(channelId, layerId, element, obsSessionId);
        
        // Reload config
        const configs = await getAllChannelCGConfigs();
        setConfig(configs[channelId] || null);
    }, [channelId, obsSessionId]);

    if (!config) {
        return (
            <div className={styles.container}>
                <div className={styles.loading}>Loading CG layers...</div>
            </div>
        );
    }

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Layers size={18} />
                <h3>CG Layers - {channelId}</h3>
            </div>

            <div className={styles.layers}>
                {config.layers.map(layer => (
                    <div key={layer.layerId} className={styles.layerCard}>
                        <div className={styles.layerHeader}>
                            <span className={styles.layerLabel}>Layer {layer.layerId}</span>
                            <button
                                className={`${styles.toggleBtn} ${layer.visible ? styles.visible : ''}`}
                                onClick={() => handleToggleLayer(layer.layerId)}
                                title={layer.visible ? 'Hide' : 'Show'}
                            >
                                {layer.visible ? <Eye size={14} /> : <EyeOff size={14} />}
                            </button>
                        </div>

                        <div className={styles.layerContent}>
                            {layer.content ? (
                                <div className={styles.elementPreview}>
                                    <span className={styles.elementName}>{layer.content.name}</span>
                                    <span className={styles.elementType}>{layer.content.type}</span>
                                </div>
                            ) : (
                                <div className={styles.emptyLayer}>No content</div>
                            )}
                        </div>

                        <div className={styles.layerActions}>
                            <div className={styles.elementSelector}>
                                <select
                                    value=""
                                    onChange={(e) => {
                                        const elementId = e.target.value;
                                        if (elementId) {
                                            const element = availableElements.find(el => el.id === elementId);
                                            if (element) {
                                                handleSendElement(layer.layerId, element);
                                            }
                                        }
                                    }}
                                    className={styles.select}
                                >
                                    <option value="">Select Element...</option>
                                    {availableElements.map(el => (
                                        <option key={el.id} value={el.id}>
                                            {el.name} ({el.type})
                                        </option>
                                    ))}
                                </select>
                            </div>
                        </div>

                        <div className={styles.layerInfo}>
                            <span className={styles.sourceName}>{layer.obsSourceName}</span>
                            <span className={styles.sceneName}>{layer.obsSceneName}</span>
                        </div>
                    </div>
                ))}
            </div>
        </div>
    );
}

