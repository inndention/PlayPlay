// ============================================
// Channel Breakout Component
// Manages Electron breakout windows for channels
// ============================================

import { useEffect, useRef } from 'react';

const isElectron = !!(window as any).electronAPI;

interface ChannelBreakoutProps {
    channelId: string;
    channelName: string;
    onClose?: () => void;
}

export function useChannelBreakout() {
    const openWindowsRef = useRef<Set<string>>(new Set());

    const openBreakoutWindow = (channelId: string, channelName: string) => {
        if (!isElectron) {
            console.warn('[ChannelBreakout] Not in Electron, cannot open breakout window');
            return;
        }

        if (openWindowsRef.current.has(channelId)) {
            console.log(`[ChannelBreakout] Window for ${channelId} already open`);
            return;
        }

        const channelNumber = channelId.replace('ch-', 'CH');
        const windowTitle = `PLAY - ${channelNumber}`;

        // Send IPC message to main process to create window
        (window as any).electronAPI?.channelBreakout?.(channelId, channelName, windowTitle)
            .then(() => {
                openWindowsRef.current.add(channelId);
                console.log(`[ChannelBreakout] Opened window for ${channelId}`);
            })
            .catch((err: Error) => {
                console.error(`[ChannelBreakout] Failed to open window:`, err);
            });
    };

    const closeBreakoutWindow = (channelId: string) => {
        if (!isElectron) return;
        
        (window as any).electronAPI?.closeChannelBreakout?.(channelId)
            .then(() => {
                openWindowsRef.current.delete(channelId);
            })
            .catch((err: Error) => {
                console.error(`[ChannelBreakout] Failed to close window:`, err);
            });
    };

    // Cleanup on unmount
    useEffect(() => {
        return () => {
            // Close all open windows
            openWindowsRef.current.forEach(channelId => {
                closeBreakoutWindow(channelId);
            });
        };
    }, []);

    return { openBreakoutWindow, closeBreakoutWindow };
}

