import React, { useEffect, useState } from 'react';
import styles from './StudioClock.module.css';

export function StudioClock() {
    const [mounted, setMounted] = useState(false);
    const [time, setTime] = useState<Date | null>(null);

    useEffect(() => {
        setMounted(true);
        setTime(new Date());
        const timer = setInterval(() => setTime(new Date()), 100);
        return () => clearInterval(timer);
    }, []);

    // Show placeholder during SSR to avoid hydration mismatch
    if (!mounted || !time) {
        return (
            <div className={styles.container}>
                <div className={styles.mainTime}>
                <div className={styles.label}>LOCAL TIME</div>
                <div className={styles.timeDisplay}>--:--:--<span className={styles.millis}>.0</span></div>
                <div className={styles.utcLine}>UTC --:--:--.0</div>
                </div>
                <div className={styles.divider}></div>
                <div className={styles.countdown}>
                    <div className={styles.label}>TO TOP OF HOUR</div>
                    <div className={styles.countdownDisplay}>--:--</div>
                </div>
            </div>
        );
    }

    const formatTime = (date: Date) => {
        return date.toLocaleTimeString('en-US', {
            hour12: false,
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit'
        });
    };

    const formatUtcTime = (date: Date) => {
        return date.toLocaleTimeString('en-GB', {
            hour12: false,
            timeZone: 'UTC',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    };

    const formatMillis = (date: Date) => {
        return Math.floor(date.getMilliseconds() / 100).toString();
    };

    // Calculate countdown to top of hour
    const nextTopOfHour = new Date(time);
    nextTopOfHour.setHours(time.getHours() + 1, 0, 0, 0);
    const diff = nextTopOfHour.getTime() - time.getTime();
    const minutes = Math.floor((diff % (1000 * 60 * 60)) / (1000 * 60));
    const seconds = Math.floor((diff % (1000 * 60)) / 1000);
    const countdown = `${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;

    // Warning if < 5 minutes to TOH
    const isWarning = minutes < 5;
    const isCritical = minutes < 1;

    return (
        <div className={styles.container}>
            <div className={styles.mainTime}>
                <div className={styles.label}>LOCAL TIME</div>
                <div className={styles.timeDisplay}>
                    {formatTime(time)}
                    <span className={styles.millis}>.{formatMillis(time)}</span>
                </div>
                <div
                    className={styles.utcLine}
                    title="Slot keys and hour auto-push use local calendar date (localCalendarDateKey in src/lib/utils). Caspar join-in-progress uses src/lib/casparBossClock. UTC line matches AMCP/server wire timestamps."
                >
                    UTC {formatUtcTime(time)}.{Math.floor(time.getUTCMilliseconds() / 100)}
                </div>
            </div>
            <div className={styles.divider}></div>
            <div className={styles.countdown}>
                <div className={styles.label}>TO TOP OF HOUR</div>
                <div className={`${styles.countdownDisplay} ${isCritical ? styles.critical : isWarning ? styles.warning : ''}`}>
                    {countdown}
                </div>
            </div>
        </div>
    );
}
