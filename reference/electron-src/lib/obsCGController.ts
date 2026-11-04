// ============================================
// OBS CG Controller
// Manages CG layers in OBS for each channel
// ============================================

import { CGLayerConfig, ChannelCGConfig, CGElement } from './types';
import { OBSWebSocketClient } from './obsWebSocket';

const isElectron = !!(window as any).electronAPI;

// Default CG layer configuration per channel
const DEFAULT_LAYERS = [1, 2, 3, 4].map(layerId => ({
    channelId: '',
    layerId,
    obsSourceName: `CG_Layer_${layerId}`,
    obsSceneName: 'AUTOMATION',
    visible: false,
}));

export async function initializeChannelCG(channelId: string, obsSessionId?: string): Promise<ChannelCGConfig> {
    return {
        channelId,
        layers: DEFAULT_LAYERS.map(layer => ({ ...layer, channelId })),
    };
}

export async function showLayer(
    channelId: string,
    layerId: number,
    content?: CGElement,
    obsSessionId?: string
): Promise<boolean> {
    try {
        // Get channel CG config
        const config = await getChannelCGConfig(channelId);
        const layer = config.layers.find(l => l.layerId === layerId);
        
        if (!layer) {
            console.error(`[OBS CG] Layer ${layerId} not found for channel ${channelId}`);
            return false;
        }

        // Update layer content if provided
        if (content) {
            layer.content = content;
            await saveChannelCGConfig(config);
        }

        // Send OBS command to show source
        // Note: This would need to get the OBSWebSocketClient for the session
        // For now, we'll use a direct call if available
        if (isElectron && (window as any).electronAPI?.obs) {
            try {
                await (window as any).electronAPI.obs.setSceneItemEnabled(
                    obsSessionId || 'default',
                    layer.obsSceneName,
                    layer.obsSourceName,
                    true
                );
            } catch (err) {
                console.error('[OBS CG] Failed to show source via Electron API:', err);
            }
        }
        
        layer.visible = true;
        await saveChannelCGConfig(config);
        
        return true;
    } catch (error) {
        console.error(`[OBS CG] Error showing layer ${layerId} for channel ${channelId}:`, error);
        return false;
    }
}

export async function hideLayer(
    channelId: string,
    layerId: number,
    obsSessionId?: string
): Promise<boolean> {
    try {
        const config = await getChannelCGConfig(channelId);
        const layer = config.layers.find(l => l.layerId === layerId);
        
        if (!layer) {
            console.error(`[OBS CG] Layer ${layerId} not found for channel ${channelId}`);
            return false;
        }

        // Send OBS command to hide source
        if (isElectron && (window as any).electronAPI?.obs) {
            try {
                await (window as any).electronAPI.obs.setSceneItemEnabled(
                    obsSessionId || 'default',
                    layer.obsSceneName,
                    layer.obsSourceName,
                    false
                );
            } catch (err) {
                console.error('[OBS CG] Failed to hide source via Electron API:', err);
            }
        }
        
        layer.visible = false;
        await saveChannelCGConfig(config);
        
        return true;
    } catch (error) {
        console.error(`[OBS CG] Error hiding layer ${layerId} for channel ${channelId}:`, error);
        return false;
    }
}

export async function updateLayer(
    channelId: string,
    layerId: number,
    content: CGElement,
    obsSessionId?: string
): Promise<boolean> {
    try {
        const config = await getChannelCGConfig(channelId);
        const layer = config.layers.find(l => l.layerId === layerId);
        
        if (!layer) {
            console.error(`[OBS CG] Layer ${layerId} not found for channel ${channelId}`);
            return false;
        }

        layer.content = content;
        await saveChannelCGConfig(config);
        
        // If layer is visible, update the browser source URL
        if (layer.visible) {
            // Browser source URL would be: http://localhost:5173/#/output?channel=X&layer=Y
            const browserUrl = `http://localhost:5173/#/output?channel=${channelId}&layer=${layerId}`;
            
            // Update browser source URL
            if (isElectron && (window as any).electronAPI?.obs) {
                try {
                    await (window as any).electronAPI.obs.setInputSettings(
                        obsSessionId || 'default',
                        layer.obsSourceName,
                        { url: browserUrl }
                    );
                } catch (err) {
                    console.error('[OBS CG] Failed to update input settings via Electron API:', err);
                }
            }
        }
        
        return true;
    } catch (error) {
        console.error(`[OBS CG] Error updating layer ${layerId} for channel ${channelId}:`, error);
        return false;
    }
}

export async function sendToChannel(
    targetChannelId: string,
    layerId: number,
    content: CGElement,
    obsSessionId?: string
): Promise<boolean> {
    return updateLayer(targetChannelId, layerId, content, obsSessionId);
}

// Storage helpers
const CG_CONFIG_KEY = 'playplay_cg_configs';

async function getChannelCGConfig(channelId: string): Promise<ChannelCGConfig> {
    const stored = localStorage.getItem(CG_CONFIG_KEY);
    const configs: Record<string, ChannelCGConfig> = stored ? JSON.parse(stored) : {};
    
    if (!configs[channelId]) {
        configs[channelId] = await initializeChannelCG(channelId);
    }
    
    return configs[channelId];
}

async function saveChannelCGConfig(config: ChannelCGConfig): Promise<void> {
    const stored = localStorage.getItem(CG_CONFIG_KEY);
    const configs: Record<string, ChannelCGConfig> = stored ? JSON.parse(stored) : {};
    configs[config.channelId] = config;
    localStorage.setItem(CG_CONFIG_KEY, JSON.stringify(configs));
}

export async function getAllChannelCGConfigs(): Promise<Record<string, ChannelCGConfig>> {
    const stored = localStorage.getItem(CG_CONFIG_KEY);
    return stored ? JSON.parse(stored) : {};
}


