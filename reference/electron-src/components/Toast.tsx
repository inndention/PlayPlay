import React, { createContext, useContext, useState, useCallback, useRef } from 'react';
import styles from './Toast.module.css';

interface Toast {
    id: string;
    message: string;
    type: 'success' | 'error' | 'info' | 'warning';
    duration?: number;
}

export interface ToastContextType {
    /**
     * Show a toast. Pass `toastKey` to replace an existing toast with the same key
     * (idempotent dedupe for agent/status storms).
     */
    showToast: (message: string, type?: Toast['type'], duration?: number, toastKey?: string) => void;
}

const ToastContext = createContext<ToastContextType | null>(null);

export const useToast = () => {
    const context = useContext(ToastContext);
    if (!context) throw new Error('useToast must be used within ToastProvider');
    return context;
};

export const ToastProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
    const [toasts, setToasts] = useState<Toast[]>([]);
    const timeoutsRef = useRef<Map<string, ReturnType<typeof setTimeout>>>(new Map());

    const showToast = useCallback((message: string, type: Toast['type'] = 'info', duration = 3000, toastKey?: string) => {
        const id = toastKey && toastKey.trim().length > 0 ? toastKey.trim() : Math.random().toString(36).substring(7);
        const prevTimer = timeoutsRef.current.get(id);
        if (prevTimer) {
            clearTimeout(prevTimer);
            timeoutsRef.current.delete(id);
        }
        setToasts((prev) => {
            const without = prev.filter((t) => t.id !== id);
            return [...without, { id, message, type, duration }];
        });
        const t = setTimeout(() => {
            timeoutsRef.current.delete(id);
            setToasts((prev) => prev.filter((x) => x.id !== id));
        }, duration);
        timeoutsRef.current.set(id, t);
    }, []);

    const removeToast = (id: string) => {
        const t = timeoutsRef.current.get(id);
        if (t) {
            clearTimeout(t);
            timeoutsRef.current.delete(id);
        }
        setToasts((prev) => prev.filter((x) => x.id !== id));
    };

    return (
        <ToastContext.Provider value={{ showToast }}>
            {children}
            <div className={styles.toastContainer}>
                {toasts.map(toast => (
                    <div
                        key={toast.id}
                        className={`${styles.toast} ${styles[toast.type]}`}
                        onClick={() => removeToast(toast.id)}
                    >
                        <div className={styles.icon}>
                            {toast.type === 'success' && '✓'}
                            {toast.type === 'error' && '✕'}
                            {toast.type === 'info' && 'ℹ'}
                            {toast.type === 'warning' && '⚠'}
                        </div>
                        <span>{toast.message}</span>
                    </div>
                ))}
            </div>
        </ToastContext.Provider>
    );
};
