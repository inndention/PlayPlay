import React, { useState, useEffect } from 'react';
import styles from './OBSAdvancedController.module.css';
import { obsPluginBridge } from '../lib/obsPluginBridge';
import { useToast } from './Toast';

export function OBSAdvancedController() {
    const { showToast } = useToast();
    const [isConnected, setIsConnected] = useState(false);

    useEffect(() => {
        obsPluginBridge.connect().then(connected => {
            setIsConnected(connected);
            if (connected) {
                showToast('OBS Plugin connected', 'success', 2000);
            }
        });
    }, [showToast]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h2>OBS Advanced Control</h2>
                <div className={styles.status}>
                    <span className={isConnected ? styles.connected : styles.disconnected}>
                        {isConnected ? 'Connected' : 'Disconnected'}
                    </span>
                </div>
            </div>
            <div className={styles.content}>
                <p>Deep OBS integration via native plugin</p>
            </div>
        </div>
    );
}

