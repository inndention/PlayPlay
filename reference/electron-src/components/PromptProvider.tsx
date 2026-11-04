import React, { useState, useEffect } from 'react';
import { registerPromptProvider, resolvePrompt } from '../lib/promptPolyfill';
import { PromptModal } from './PromptModal';

export function PromptProvider({ children }: { children: React.ReactNode }) {
    const [state, setState] = useState<{ message: string; defaultValue: string } | null>(null);

    useEffect(() => {
        registerPromptProvider(setState);
        return () => registerPromptProvider(() => {});
    }, []);

    const handleClose = () => {
        resolvePrompt(null);
        setState(null);
    };

    return (
        <>
            {children}
            {state && (
                <PromptModal
                    message={state.message}
                    defaultValue={state.defaultValue}
                    onClose={handleClose}
                />
            )}
        </>
    );
}
