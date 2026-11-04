import React, { useState } from 'react';
import { CommandContainer } from '../lib/types';
import { Save, Play, X } from 'lucide-react';
import styles from './CommandBuilder.module.css';

interface CommandBuilderProps {
    command?: CommandContainer;
    onSave: (command: CommandContainer) => void;
    onCancel: () => void;
}

export function CommandBuilder({ command, onSave, onCancel }: CommandBuilderProps) {
    const [name, setName] = useState(command?.name || '');
    const [type, setType] = useState<CommandContainer['type']>(command?.type || 'scene');
    const [equipment, setEquipment] = useState<CommandContainer['equipment']>(command?.equipment || 'obs');
    const [target, setTarget] = useState(command?.target || '');
    const [parameters, setParameters] = useState(JSON.stringify(command?.parameters || {}, null, 2));

    const handleSave = () => {
        const cmd: CommandContainer = {
            id: command?.id || `cmd_${Date.now()}`,
            name,
            type,
            equipment,
            target,
            parameters: JSON.parse(parameters),
            metadata: command?.metadata,
        };
        onSave(cmd);
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h3>{command ? 'Edit Command' : 'New Command'}</h3>
                <button className={styles.closeBtn} onClick={onCancel}>
                    <X size={18} />
                </button>
            </div>
            <div className={styles.content}>
                <div className={styles.field}>
                    <label>Name</label>
                    <input
                        type="text"
                        value={name}
                        onChange={(e) => setName(e.target.value)}
                        placeholder="Command name"
                    />
                </div>
                <div className={styles.field}>
                    <label>Type</label>
                    <select value={type} onChange={(e) => setType(e.target.value as CommandContainer['type'])}>
                        <option value="scene">Scene</option>
                        <option value="live_stream">Live Stream</option>
                        <option value="social_post">Social Post</option>
                        <option value="go_live">Go Live</option>
                        <option value="equipment_control">Equipment Control</option>
                        <option value="automation">Automation</option>
                        <option value="custom">Custom</option>
                    </select>
                </div>
                <div className={styles.field}>
                    <label>Equipment</label>
                    <select value={equipment} onChange={(e) => setEquipment(e.target.value as CommandContainer['equipment'])}>
                        <option value="obs">OBS</option>
                        <option value="onvif">ONVIF</option>
                        <option value="atem">ATEM</option>
                        <option value="tc1">TC1</option>
                        <option value="none">None</option>
                    </select>
                </div>
                <div className={styles.field}>
                    <label>Target</label>
                    <input
                        type="text"
                        value={target}
                        onChange={(e) => setTarget(e.target.value)}
                        placeholder="Equipment identifier"
                    />
                </div>
                <div className={styles.field}>
                    <label>Parameters (JSON)</label>
                    <textarea
                        value={parameters}
                        onChange={(e) => setParameters(e.target.value)}
                        rows={8}
                        placeholder='{"key": "value"}'
                    />
                </div>
            </div>
            <div className={styles.footer}>
                <button className={styles.testBtn}>
                    <Play size={14} />
                    Test
                </button>
                <div className={styles.actions}>
                    <button className={styles.cancelBtn} onClick={onCancel}>
                        Cancel
                    </button>
                    <button className={styles.saveBtn} onClick={handleSave}>
                        <Save size={14} />
                        Save
                    </button>
                </div>
            </div>
        </div>
    );
}




