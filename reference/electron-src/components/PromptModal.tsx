import React, { useEffect, useState, useRef } from 'react';
import { resolvePrompt } from '../lib/promptPolyfill';
import styles from './PromptModal.module.css';

export type PromptModalProps = {
    message: string;
    defaultValue: string;
    onClose: () => void;
};

export function PromptModal({ message, defaultValue, onClose }: PromptModalProps) {
    const [value, setValue] = useState(defaultValue);
    const inputRef = useRef<HTMLInputElement>(null);

    useEffect(() => {
        setValue(defaultValue);
        const t = requestAnimationFrame(() => {
            inputRef.current?.focus();
            inputRef.current?.select();
        });
        return () => cancelAnimationFrame(t);
    }, [defaultValue, message]);

    const handleSubmit = (e: React.FormEvent) => {
        e.preventDefault();
        resolvePrompt(value);
    };

    const handleCancel = () => {
        resolvePrompt(null);
        onClose();
    };

    return (
        <div className={styles.overlay} onClick={handleCancel}>
            <div className={styles.dialog} onClick={e => e.stopPropagation()}>
                <form onSubmit={handleSubmit}>
                    <label className={styles.label}>{message}</label>
                    <input
                        ref={inputRef}
                        type="text"
                        className={styles.input}
                        value={value}
                        onChange={e => setValue(e.target.value)}
                        onKeyDown={e => e.key === 'Escape' && handleCancel()}
                    />
                    <div className={styles.actions}>
                        <button type="button" onClick={handleCancel}>Cancel</button>
                        <button type="submit">OK</button>
                    </div>
                </form>
            </div>
        </div>
    );
}
