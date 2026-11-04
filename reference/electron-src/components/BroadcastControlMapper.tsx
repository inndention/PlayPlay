import React, { useState } from 'react';
import { CommandContainer, EquipmentConnection } from '../lib/types';
import { loadCommandContainers, loadEquipmentConnections } from '../lib/storage';
import { Radio, Wifi, WifiOff, Plus, Play } from 'lucide-react';
import styles from './BroadcastControlMapper.module.css';

export function BroadcastControlMapper() {
    const [commands, setCommands] = useState<CommandContainer[]>([]);
    const [connections, setConnections] = useState<EquipmentConnection[]>([]);
    const [selectedCommand, setSelectedCommand] = useState<CommandContainer | null>(null);

    React.useEffect(() => {
        loadCommandContainers().then(setCommands);
        loadEquipmentConnections().then(setConnections);
    }, []);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>Broadcast Control Mapper</div>
                <button className={styles.addBtn}>
                    <Plus size={16} />
                    New Command
                </button>
            </div>
            <div className={styles.content}>
                <div className={styles.sidebar}>
                    <div className={styles.section}>
                        <h4>Equipment Connections</h4>
                        {connections.map(conn => (
                            <div key={conn.id} className={styles.connectionItem}>
                                {conn.isConnected ? <Wifi size={14} /> : <WifiOff size={14} />}
                                <span>{conn.name}</span>
                                <span className={styles.connType}>{conn.type}</span>
                            </div>
                        ))}
                    </div>
                    <div className={styles.section}>
                        <h4>Commands</h4>
                        {commands.map(cmd => (
                            <div
                                key={cmd.id}
                                className={`${styles.commandItem} ${selectedCommand?.id === cmd.id ? styles.selected : ''}`}
                                onClick={() => setSelectedCommand(cmd)}
                            >
                                <div className={styles.commandName}>{cmd.name}</div>
                                <div className={styles.commandMeta}>{cmd.type} • {cmd.equipment}</div>
                            </div>
                        ))}
                    </div>
                </div>
                <div className={styles.main}>
                    {selectedCommand ? (
                        <div className={styles.commandDetails}>
                            <h3>{selectedCommand.name}</h3>
                            <div className={styles.detailRow}>
                                <span>Type:</span>
                                <span>{selectedCommand.type}</span>
                            </div>
                            <div className={styles.detailRow}>
                                <span>Equipment:</span>
                                <span>{selectedCommand.equipment}</span>
                            </div>
                            <div className={styles.detailRow}>
                                <span>Target:</span>
                                <span>{selectedCommand.target}</span>
                            </div>
                            <button className={styles.testBtn}>
                                <Play size={14} />
                                Test Command
                            </button>
                        </div>
                    ) : (
                        <div className={styles.empty}>
                            Select a command to view details
                        </div>
                    )}
                </div>
            </div>
        </div>
    );
}




