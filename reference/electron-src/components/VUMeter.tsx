import React, { useEffect, useRef, useState } from 'react';
import styles from './VUMeter.module.css';

interface VUMeterProps {
    levels: number[]; // 0 to 1 for each channel
    label?: string;
    showLabels?: boolean;
    orientation?: 'vertical' | 'horizontal';
}

export const VUMeter: React.FC<VUMeterProps> = ({ 
    levels, 
    label, 
    showLabels = true,
    orientation = 'vertical' 
}) => {
    const normalizeLevel = (raw: number): number => {
        if (!Number.isFinite(raw)) return 0;
        // dBFS style input (commonly -60..0)
        if (raw <= 0 && raw >= -120) {
            return Math.max(0, Math.min(1, (raw + 60) / 60));
        }
        // Percent-like input (0..100)
        if (raw > 1 && raw <= 100) {
            return Math.max(0, Math.min(1, raw / 100));
        }
        // Already normalized linear input
        return Math.max(0, Math.min(1, raw));
    };

    const normalizedLevels = levels.map(normalizeLevel);
    const channels = levels.length;
    const [peaks, setPeaks] = useState<number[]>(new Array(channels).fill(0));
    const lastPeaksUpdate = useRef<number>(Date.now());

    useEffect(() => {
        setPeaks(prev => prev.map((p, i) => Math.max(p, normalizedLevels[i] || 0)));
        lastPeaksUpdate.current = Date.now();
    }, [normalizedLevels]);

    // Decay peaks
    useEffect(() => {
        const interval = setInterval(() => {
            if (Date.now() - lastPeaksUpdate.current > 1000) {
                setPeaks(prev => prev.map(p => Math.max(0, p * 0.95)));
            }
        }, 100);
        return () => clearInterval(interval);
    }, []);

    const renderBar = (idx: number) => {
        const level = normalizedLevels[idx] || 0;
        const peak = peaks[idx] || 0;
        const barStyle = orientation === 'vertical' 
            ? { height: `${level * 100}%` } 
            : { width: `${level * 100}%` };
        
        const peakStyle = orientation === 'vertical'
            ? { bottom: `${peak * 100}%` }
            : { left: `${peak * 100}%` };

        return (
            <div key={idx} className={`${styles.meterTrack} ${styles[orientation]}`}>
                <div className={styles.meterFill} style={barStyle} />
                <div className={styles.peakMarker} style={peakStyle} />
                {/* Scale markers */}
                <div className={styles.markers}>
                    <div className={styles.marker} style={{ [orientation === 'vertical' ? 'bottom' : 'left']: '0%' }} />
                    <div className={styles.marker} style={{ [orientation === 'vertical' ? 'bottom' : 'left']: '50%' }} />
                    <div className={styles.marker} style={{ [orientation === 'vertical' ? 'bottom' : 'left']: '75%' }} />
                    <div className={styles.marker} style={{ [orientation === 'vertical' ? 'bottom' : 'left']: '90%' }} />
                </div>
            </div>
        );
    };

    return (
        <div className={`${styles.container} ${styles[orientation]}`}>
            {showLabels && <div className={styles.label}>{label || 'AUDIO VU'}</div>}
            <div className={styles.metersRow}>
                {Array.from({ length: channels }).map((_, i) => renderBar(i))}
            </div>
            {showLabels && (
                <div className={styles.scale}>
                    <span>0</span>
                    <span>-12</span>
                    <span>-6</span>
                    <span>0dB</span>
                </div>
            )}
        </div>
    );
};
