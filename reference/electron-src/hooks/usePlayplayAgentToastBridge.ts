import { useEffect, useRef } from 'react';
import {
    agentEventToastType,
    emitPlayplayAgentEvent,
    formatAgentToastMessage,
    ingestPlayplayAgentIpcPayload,
    subscribePlayplayAgentBus,
} from '../lib/playplayAgentBus';
import type { ToastContextType } from '../components/Toast';

export const LS_PLAYPLAY_AGENT_TOASTS_ENABLED = 'playplay_agent_toasts_enabled';

function readAgentToastsEnabled(): boolean {
    try {
        return localStorage.getItem(LS_PLAYPLAY_AGENT_TOASTS_ENABLED) === '1';
    } catch {
        return false;
    }
}

/**
 * Wires `playplayAgentBus` + main-process `playplay-agent:event` IPC to `useToast`,
 * with idempotent keys carried on each event.
 */
export function usePlayplayAgentToastBridge(showToast: ToastContextType['showToast']): void {
    const showRef = useRef(showToast);
    showRef.current = showToast;
    const toastsEnabledRef = useRef(readAgentToastsEnabled());

    useEffect(() => {
        const syncFlag = () => {
            toastsEnabledRef.current = readAgentToastsEnabled();
        };

        const unsubBus = subscribePlayplayAgentBus((ev) => {
            if (!toastsEnabledRef.current) return;
            const msg = formatAgentToastMessage(ev);
            showRef.current(msg, agentEventToastType(ev.kind), ev.kind === 'done' ? 4500 : 6000, ev.toastKey);
        });

        const api = (typeof window !== 'undefined' ? (window as unknown as { electronAPI?: {
            onPlayplayAgentEvent?: (cb: (payload: unknown) => void) => () => void;
        } }).electronAPI : undefined);

        const off = api?.onPlayplayAgentEvent?.((payload) => {
            ingestPlayplayAgentIpcPayload(payload);
        });

        const onStorage = (e: StorageEvent) => {
            if (e.key === LS_PLAYPLAY_AGENT_TOASTS_ENABLED) syncFlag();
        };
        const onCustom = () => syncFlag();
        window.addEventListener('storage', onStorage);
        window.addEventListener('playplay-agent-settings-changed', onCustom);

        return () => {
            unsubBus();
            off?.();
            window.removeEventListener('storage', onStorage);
            window.removeEventListener('playplay-agent-settings-changed', onCustom);
        };
    }, []);

    /** Dev-only: renderer can push the same path as IPC (tests / future worker). */
    useEffect(() => {
        if (typeof window === 'undefined') return;
        const w = window as unknown as { __playplayAgentEmit?: typeof emitPlayplayAgentEvent };
        w.__playplayAgentEmit = emitPlayplayAgentEvent;
        return () => {
            delete w.__playplayAgentEmit;
        };
    }, []);
}
