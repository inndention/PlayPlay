/**
 * QRCodeOverlay - Display QR codes on screen
 * 
 * Features:
 * - Generate QR codes for any URL
 * - On-screen QR display (CG overlay)
 * - Auto-generate unique session URLs
 */

import React, { useState, useEffect, useMemo } from 'react';
import styles from './QRCodeOverlay.module.css';
import { X, Copy, Check, RefreshCw, QrCode } from 'lucide-react';

interface QRCodeOverlayProps {
    url: string;
    title?: string;
    visible: boolean;
    position?: 'center' | 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right' | 'custom';
    customPosition?: { x: number; y: number }; // percentage
    size?: number; // pixels
    onClose?: () => void;
}

export const QRCodeOverlay: React.FC<QRCodeOverlayProps> = ({
    url,
    title = 'Scan to Join',
    visible,
    position = 'center',
    customPosition,
    size = 300,
    onClose,
}) => {
    const [copied, setCopied] = useState(false);

    const qrCodeUrl = useMemo(() => {
        return `https://api.qrserver.com/v1/create-qr-code/?size=${size}x${size}&data=${encodeURIComponent(url)}`;
    }, [url, size]);

    const handleCopyUrl = () => {
        navigator.clipboard.writeText(url).then(() => {
            setCopied(true);
            setTimeout(() => setCopied(false), 2000);
        });
    };

    if (!visible) return null;

    const positionStyles: React.CSSProperties = {
        position: 'absolute',
        ...(position === 'center' && {
            top: '50%',
            left: '50%',
            transform: 'translate(-50%, -50%)',
        }),
        ...(position === 'top-left' && {
            top: '20px',
            left: '20px',
        }),
        ...(position === 'top-right' && {
            top: '20px',
            right: '20px',
        }),
        ...(position === 'bottom-left' && {
            bottom: '20px',
            left: '20px',
        }),
        ...(position === 'bottom-right' && {
            bottom: '20px',
            right: '20px',
        }),
        ...(customPosition && {
            top: `${customPosition.y}%`,
            left: `${customPosition.x}%`,
            transform: 'translate(-50%, -50%)',
        }),
    };

    return (
        <div className={styles.overlay} style={positionStyles}>
            <div className={styles.qrContainer}>
                {onClose && (
                    <button className={styles.closeButton} onClick={onClose}>
                        <X size={16} />
                    </button>
                )}
                {title && (
                    <div className={styles.title}>{title}</div>
                )}
                <div className={styles.qrCode}>
                    <img src={qrCodeUrl} alt="QR Code" />
                </div>
                <div className={styles.urlSection}>
                    <div className={styles.url}>{url}</div>
                    <button
                        className={styles.copyButton}
                        onClick={handleCopyUrl}
                        title="Copy URL"
                    >
                        {copied ? <Check size={14} /> : <Copy size={14} />}
                    </button>
                </div>
            </div>
        </div>
    );
};
