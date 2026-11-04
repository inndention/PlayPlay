import React, { useState, useRef, useEffect } from 'react';
import { createPortal } from 'react-dom';
import styles from './Tooltip.module.css';

interface TooltipProps {
    content: string | React.ReactNode;
    shortcut?: string;
    helpId?: string;  // Links to HelpPanel topic
    position?: 'top' | 'bottom' | 'left' | 'right';
    delay?: number;
    children: React.ReactNode;
}

export function Tooltip({
    content,
    shortcut,
    helpId,
    position = 'top',
    delay = 400,
    children
}: TooltipProps) {
    const [visible, setVisible] = useState(false);
    const [coords, setCoords] = useState({ x: 0, y: 0 });
    const targetRef = useRef<HTMLDivElement>(null);
    const timeoutRef = useRef<ReturnType<typeof setTimeout>>();

    const showTooltip = () => {
        timeoutRef.current = setTimeout(() => {
            if (targetRef.current) {
                const rect = targetRef.current.getBoundingClientRect();
                let x = rect.left + rect.width / 2;
                let y = rect.top;

                switch (position) {
                    case 'bottom':
                        y = rect.bottom + 8;
                        break;
                    case 'left':
                        x = rect.left - 8;
                        y = rect.top + rect.height / 2;
                        break;
                    case 'right':
                        x = rect.right + 8;
                        y = rect.top + rect.height / 2;
                        break;
                    default: // top
                        y = rect.top - 8;
                }

                setCoords({ x, y });
                setVisible(true);
            }
        }, delay);
    };

    const hideTooltip = () => {
        if (timeoutRef.current) {
            clearTimeout(timeoutRef.current);
        }
        setVisible(false);
    };

    useEffect(() => {
        return () => {
            if (timeoutRef.current) {
                clearTimeout(timeoutRef.current);
            }
        };
    }, []);

    return (
        <>
            <div
                ref={targetRef}
                onMouseEnter={showTooltip}
                onMouseLeave={hideTooltip}
                onFocus={showTooltip}
                onBlur={hideTooltip}
                className={styles.wrapper}
            >
                {children}
            </div>
            {visible && typeof window !== 'undefined' && createPortal(
                <div
                    className={`${styles.tooltip} ${styles[position]}`}
                    style={{
                        left: coords.x,
                        top: coords.y,
                    }}
                >
                    <div className={styles.content}>{content}</div>
                    {shortcut && <span className={styles.shortcut}>{shortcut}</span>}
                    {helpId && <span className={styles.helpLink}>Press F1 for help</span>}
                </div>,
                document.body
            )}
        </>
    );
}

// Quick tooltip wrapper for icons/buttons
export function TipIcon({
    tip,
    shortcut,
    helpId,
    children
}: {
    tip: string;
    shortcut?: string;
    helpId?: string;
    children: React.ReactNode;
}) {
    return (
        <Tooltip content={tip} shortcut={shortcut} helpId={helpId}>
            {children}
        </Tooltip>
    );
}
