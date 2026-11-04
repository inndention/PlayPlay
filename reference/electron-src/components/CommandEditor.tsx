8// ============================================
// Comprehensive Command Editor
// Allows creating/editing schedule commands with in/out times
// ============================================

import React, { useState, useCallback, useMemo } from 'react';
import { ScheduleCommand, ScheduleCommandType, OBSSession } from '../lib/types';
import { formatDuration } from '../lib/utils';
import { 
    X, Save, Play, Globe, Monitor, Layout, Send, 
    Volume2, Lightbulb, Code, Clock, ChevronDown,
    Plus, Trash2, Copy
} from 'lucide-react';
import styles from './CommandEditor.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

interface CommandEditorProps {
    commands: ScheduleCommand[];
    onSave: (commands: ScheduleCommand[]) => void;
    onClose?: () => void;
    slotDuration?: number; // Duration in seconds (default 3600 = 1 hour)
    obsSessions?: OBSSession[];
}

const COMMAND_TYPES: { type: ScheduleCommandType; label: string; icon: React.ReactNode; description: string }[] = [
    { type: 'show_webpage', label: 'Show Webpage', icon: <Globe size={16} />, description: 'Display a webpage in OBS browser source' },
    { type: 'switch_scene', label: 'Switch Scene', icon: <Monitor size={16} />, description: 'Switch to an OBS scene temporarily' },
    { type: 'play_stream', label: 'Play Stream', icon: <Play size={16} />, description: 'Play a stream (native or OBS source)' },
    { type: 'cg_display', label: 'CG Overlay', icon: <Layout size={16} />, description: 'Show a CG template overlay' },
    { type: 'social_post', label: 'Social Post', icon: <Send size={16} />, description: 'Post to social media' },
    { type: 'send_to_remote', label: 'Send to Remote', icon: <Send size={16} />, description: 'Send command to remote instance' },
    { type: 'audio_ducking', label: 'Audio Ducking', icon: <Volume2 size={16} />, description: 'Duck audio levels' },
    { type: 'tally_control', label: 'Tally Control', icon: <Lightbulb size={16} />, description: 'Control tally lights' },
    { type: 'execute_script', label: 'Execute Script', icon: <Code size={16} />, description: 'Run a custom script' },
    { type: 'custom', label: 'Custom', icon: <Code size={16} />, description: 'Custom command' },
];

export function CommandEditor({ 
    commands: initialCommands, 
    onSave, 
    onClose,
    slotDuration = 3600,
    obsSessions = []
}: CommandEditorProps) {
    const [commands, setCommands] = useState<ScheduleCommand[]>(initialCommands);
    const [selectedCommandId, setSelectedCommandId] = useState<string | null>(
        initialCommands.length > 0 ? initialCommands[0].id : null
    );
    const [showAddMenu, setShowAddMenu] = useState(false);

    const selectedCommand = useMemo(() => 
        commands.find(c => c.id === selectedCommandId) || null,
        [commands, selectedCommandId]
    );

    const handleAddCommand = useCallback((type: ScheduleCommandType) => {
        const typeInfo = COMMAND_TYPES.find(t => t.type === type);
        const newCommand: ScheduleCommand = {
            id: generateId(),
            type,
            name: typeInfo?.label || 'New Command',
            inPoint: 0,
            outPoint: 60, // Default 1 minute
            target: {},
            returnToAutomation: true,
            automationSceneName: 'AUTOMATION',
            enabled: true,
        };
        setCommands(prev => [...prev, newCommand]);
        setSelectedCommandId(newCommand.id);
        setShowAddMenu(false);
    }, []);

    const handleUpdateCommand = useCallback((id: string, updates: Partial<ScheduleCommand>) => {
        setCommands(prev => prev.map(cmd => 
            cmd.id === id ? { ...cmd, ...updates } : cmd
        ));
    }, []);

    const handleDeleteCommand = useCallback((id: string) => {
        setCommands(prev => prev.filter(cmd => cmd.id !== id));
        if (selectedCommandId === id) {
            setSelectedCommandId(commands.length > 1 ? commands[0].id : null);
        }
    }, [selectedCommandId, commands]);

    const handleDuplicateCommand = useCallback((cmd: ScheduleCommand) => {
        const newCommand: ScheduleCommand = {
            ...cmd,
            id: generateId(),
            name: `${cmd.name} (Copy)`,
        };
        setCommands(prev => [...prev, newCommand]);
        setSelectedCommandId(newCommand.id);
    }, []);

    const handleSave = useCallback(() => {
        onSave(commands);
        onClose?.();
    }, [commands, onSave, onClose]);

    const formatTime = (seconds: number) => {
        const totalSec = Math.round(seconds);
        const mins = Math.floor(totalSec / 60);
        const secs = totalSec % 60;
        return `${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
    };

    const parseTime = (value: string): number => {
        const parts = value.split(':');
        if (parts.length === 2) {
            return parseInt(parts[0], 10) * 60 + parseInt(parts[1], 10);
        }
        return parseInt(value, 10) || 0;
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h2 className={styles.title}>
                    <Clock size={20} />
                    Command Editor
                </h2>
                <div className={styles.headerActions}>
                    <button className={styles.saveBtn} onClick={handleSave}>
                        <Save size={16} />
                        Save Commands
                    </button>
                    {onClose && (
                        <button className={styles.closeBtn} onClick={onClose}>
                            <X size={18} />
                        </button>
                    )}
                </div>
            </div>

            <div className={styles.content}>
                {/* Command Timeline Visualization */}
                <div className={styles.timeline}>
                    <div className={styles.timelineHeader}>
                        <span>Command Timeline</span>
                        <span className={styles.timelineDuration}>{formatDuration(slotDuration)}</span>
                    </div>
                    <div className={styles.timelineTrack}>
                        {commands.map(cmd => {
                            const left = (cmd.inPoint / slotDuration) * 100;
                            const width = ((cmd.outPoint - cmd.inPoint) / slotDuration) * 100;
                            return (
                                <div
                                    key={cmd.id}
                                    className={`${styles.timelineCommand} ${selectedCommandId === cmd.id ? styles.selected : ''} ${!cmd.enabled ? styles.disabled : ''}`}
                                    style={{
                                        left: `${left}%`,
                                        width: `${Math.max(1, width)}%`,
                                        backgroundColor: cmd.color || '#3b82f6',
                                    }}
                                    onClick={() => setSelectedCommandId(cmd.id)}
                                    title={`${cmd.name}\n${formatTime(cmd.inPoint)} - ${formatTime(cmd.outPoint)}`}
                                >
                                    <span className={styles.timelineLabel}>{cmd.name}</span>
                                </div>
                            );
                        })}
                    </div>
                    <div className={styles.timeMarkers}>
                        <span>:00</span>
                        <span>:15</span>
                        <span>:30</span>
                        <span>:45</span>
                        <span>:60</span>
                    </div>
                </div>

                <div className={styles.panels}>
                    {/* Command List */}
                    <div className={styles.commandList}>
                        <div className={styles.listHeader}>
                            <span>Commands ({commands.length})</span>
                            <div className={styles.addButtonWrapper}>
                                <button 
                                    className={styles.addBtn}
                                    onClick={() => setShowAddMenu(!showAddMenu)}
                                >
                                    <Plus size={14} />
                                    Add
                                    <ChevronDown size={12} />
                                </button>
                                {showAddMenu && (
                                    <div className={styles.addMenu}>
                                        {COMMAND_TYPES.map(type => (
                                            <button
                                                key={type.type}
                                                className={styles.addMenuItem}
                                                onClick={() => handleAddCommand(type.type)}
                                            >
                                                {type.icon}
                                                <div className={styles.addMenuItemText}>
                                                    <span className={styles.addMenuItemLabel}>{type.label}</span>
                                                    <span className={styles.addMenuItemDesc}>{type.description}</span>
                                                </div>
                                            </button>
                                        ))}
                                    </div>
                                )}
                            </div>
                        </div>
                        <div className={styles.listItems}>
                            {commands.length === 0 ? (
                                <div className={styles.emptyList}>
                                    No commands added. Click "Add" to create a command.
                                </div>
                            ) : (
                                commands.map(cmd => {
                                    const typeInfo = COMMAND_TYPES.find(t => t.type === cmd.type);
                                    return (
                                        <div
                                            key={cmd.id}
                                            className={`${styles.listItem} ${selectedCommandId === cmd.id ? styles.selected : ''} ${!cmd.enabled ? styles.disabled : ''}`}
                                            onClick={() => setSelectedCommandId(cmd.id)}
                                        >
                                            <div className={styles.listItemIcon} style={{ color: cmd.color || '#3b82f6' }}>
                                                {typeInfo?.icon}
                                            </div>
                                            <div className={styles.listItemContent}>
                                                <span className={styles.listItemName}>{cmd.name}</span>
                                                <span className={styles.listItemTime}>
                                                    {formatTime(cmd.inPoint)} - {formatTime(cmd.outPoint)}
                                                </span>
                                            </div>
                                            <div className={styles.listItemActions}>
                                                <button 
                                                    className={styles.iconBtn}
                                                    onClick={(e) => { e.stopPropagation(); handleDuplicateCommand(cmd); }}
                                                    title="Duplicate"
                                                >
                                                    <Copy size={14} />
                                                </button>
                                                <button 
                                                    className={styles.iconBtn}
                                                    onClick={(e) => { e.stopPropagation(); handleDeleteCommand(cmd.id); }}
                                                    title="Delete"
                                                >
                                                    <Trash2 size={14} />
                                                </button>
                                            </div>
                                        </div>
                                    );
                                })
                            )}
                        </div>
                    </div>

                    {/* Command Details Editor */}
                    <div className={styles.detailsPanel}>
                        {selectedCommand ? (
                            <>
                                <div className={styles.detailsHeader}>
                                    <input
                                        type="text"
                                        value={selectedCommand.name}
                                        onChange={(e) => handleUpdateCommand(selectedCommand.id, { name: e.target.value })}
                                        className={styles.nameInput}
                                        placeholder="Command name"
                                    />
                                    <label className={styles.enabledToggle}>
                                        <input
                                            type="checkbox"
                                            checked={selectedCommand.enabled}
                                            onChange={(e) => handleUpdateCommand(selectedCommand.id, { enabled: e.target.checked })}
                                        />
                                        Enabled
                                    </label>
                                </div>

                                {/* Timing */}
                                <div className={styles.section}>
                                    <h4 className={styles.sectionTitle}>
                                        <Clock size={14} />
                                        Timing
                                    </h4>
                                    <div className={styles.timingRow}>
                                        <div className={styles.timeInput}>
                                            <label>In Point (MM:SS)</label>
                                            <input
                                                type="text"
                                                value={formatTime(selectedCommand.inPoint)}
                                                onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                    inPoint: Math.min(parseTime(e.target.value), selectedCommand.outPoint - 1) 
                                                })}
                                                placeholder="00:00"
                                            />
                                        </div>
                                        <div className={styles.timeInput}>
                                            <label>Out Point (MM:SS)</label>
                                            <input
                                                type="text"
                                                value={formatTime(selectedCommand.outPoint)}
                                                onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                    outPoint: Math.max(parseTime(e.target.value), selectedCommand.inPoint + 1) 
                                                })}
                                                placeholder="01:00"
                                            />
                                        </div>
                                        <div className={styles.durationDisplay}>
                                            Duration: {formatDuration(selectedCommand.outPoint - selectedCommand.inPoint)}
                                        </div>
                                    </div>
                                    {/* Quick duration buttons */}
                                    <div className={styles.quickDurations}>
                                        {[15, 30, 60, 120, 300].map(secs => (
                                            <button
                                                key={secs}
                                                className={styles.quickDurationBtn}
                                                onClick={() => handleUpdateCommand(selectedCommand.id, { 
                                                    outPoint: Math.min(selectedCommand.inPoint + secs, slotDuration) 
                                                })}
                                            >
                                                {formatDuration(secs)}
                                            </button>
                                        ))}
                                    </div>
                                </div>

                                {/* Target Configuration based on type */}
                                <div className={styles.section}>
                                    <h4 className={styles.sectionTitle}>
                                        {COMMAND_TYPES.find(t => t.type === selectedCommand.type)?.icon}
                                        Configuration
                                    </h4>
                                    
                                    {selectedCommand.type === 'show_webpage' && (
                                        <div className={styles.configFields}>
                                            <div className={styles.field}>
                                                <label>URL</label>
                                                <input
                                                    type="url"
                                                    value={selectedCommand.target.browserUrl || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, browserUrl: e.target.value } 
                                                    })}
                                                    placeholder="https://example.com"
                                                />
                                            </div>
                                            <div className={styles.field}>
                                                <label>Browser Source Name</label>
                                                <input
                                                    type="text"
                                                    value={selectedCommand.browserSourceName || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        browserSourceName: e.target.value 
                                                    })}
                                                    placeholder="BrowserSource"
                                                />
                                            </div>
                                        </div>
                                    )}

                                    {selectedCommand.type === 'switch_scene' && (
                                        <div className={styles.configFields}>
                                            <div className={styles.field}>
                                                <label>Target Scene</label>
                                                <input
                                                    type="text"
                                                    value={selectedCommand.target.obsScene || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, obsScene: e.target.value } 
                                                    })}
                                                    placeholder="Scene Name"
                                                />
                                            </div>
                                            {obsSessions.length > 0 && (
                                                <div className={styles.field}>
                                                    <label>OBS Session</label>
                                                    <select
                                                        value={selectedCommand.obsSessionId || ''}
                                                        onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                            obsSessionId: e.target.value || undefined 
                                                        })}
                                                    >
                                                        <option value="">All Sessions</option>
                                                        {obsSessions.map(s => (
                                                            <option key={s.id} value={s.id}>{s.name}</option>
                                                        ))}
                                                    </select>
                                                </div>
                                            )}
                                        </div>
                                    )}

                                    {selectedCommand.type === 'play_stream' && (
                                        <div className={styles.configFields}>
                                            <div className={styles.field}>
                                                <label>Stream URL</label>
                                                <input
                                                    type="url"
                                                    value={selectedCommand.target.streamUrl || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, streamUrl: e.target.value } 
                                                    })}
                                                    placeholder="rtmp://... or srt://... or rtsp://..."
                                                />
                                            </div>
                                            <div className={styles.field}>
                                                <label>Stream Type</label>
                                                <select
                                                    value={selectedCommand.target.streamType || 'native'}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, streamType: e.target.value as any } 
                                                    })}
                                                >
                                                    <option value="native">Native (MPV)</option>
                                                    <option value="obs_media_source">OBS Media Source</option>
                                                </select>
                                            </div>
                                            {selectedCommand.target.streamType === 'obs_media_source' && (
                                                <div className={styles.field}>
                                                    <label>Media Source Name</label>
                                                    <input
                                                        type="text"
                                                        value={selectedCommand.mediaSourceName || ''}
                                                        onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                            mediaSourceName: e.target.value 
                                                        })}
                                                        placeholder="MediaSource"
                                                    />
                                                </div>
                                            )}
                                        </div>
                                    )}

                                    {selectedCommand.type === 'send_to_remote' && (
                                        <div className={styles.configFields}>
                                            <div className={styles.field}>
                                                <label>Remote Instance ID</label>
                                                <input
                                                    type="text"
                                                    value={selectedCommand.target.remoteInstanceId || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, remoteInstanceId: e.target.value } 
                                                    })}
                                                    placeholder="remote-instance-1"
                                                />
                                            </div>
                                        </div>
                                    )}

                                    {selectedCommand.type === 'execute_script' && (
                                        <div className={styles.configFields}>
                                            <div className={styles.field}>
                                                <label>Script Path</label>
                                                <input
                                                    type="text"
                                                    value={selectedCommand.target.scriptPath || ''}
                                                    onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                        target: { ...selectedCommand.target, scriptPath: e.target.value } 
                                                    })}
                                                    placeholder="/path/to/script.sh"
                                                />
                                            </div>
                                        </div>
                                    )}
                                </div>

                                {/* Return behavior */}
                                <div className={styles.section}>
                                    <h4 className={styles.sectionTitle}>
                                        <Monitor size={14} />
                                        Return Behavior
                                    </h4>
                                    <label className={styles.checkbox}>
                                        <input
                                            type="checkbox"
                                            checked={selectedCommand.returnToAutomation ?? true}
                                            onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                returnToAutomation: e.target.checked 
                                            })}
                                        />
                                        Return to automation scene after command
                                    </label>
                                    {selectedCommand.returnToAutomation && (
                                        <div className={styles.field}>
                                            <label>Automation Scene Name</label>
                                            <input
                                                type="text"
                                                value={selectedCommand.automationSceneName || 'AUTOMATION'}
                                                onChange={(e) => handleUpdateCommand(selectedCommand.id, { 
                                                    automationSceneName: e.target.value 
                                                })}
                                                placeholder="AUTOMATION"
                                            />
                                        </div>
                                    )}
                                </div>

                                {/* Color */}
                                <div className={styles.section}>
                                    <h4 className={styles.sectionTitle}>Display Color</h4>
                                    <div className={styles.colorPicker}>
                                        {['#3b82f6', '#ef4444', '#10b981', '#f59e0b', '#8b5cf6', '#ec4899', '#06b6d4', '#6366f1'].map(color => (
                                            <button
                                                key={color}
                                                className={`${styles.colorBtn} ${selectedCommand.color === color ? styles.selected : ''}`}
                                                style={{ backgroundColor: color }}
                                                onClick={() => handleUpdateCommand(selectedCommand.id, { color })}
                                            />
                                        ))}
                                    </div>
                                </div>
                            </>
                        ) : (
                            <div className={styles.noSelection}>
                                Select a command to edit or add a new one
                            </div>
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
}

