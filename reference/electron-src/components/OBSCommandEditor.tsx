import React, { useState, useEffect } from 'react';
import { OBSCommand, OBSCommandType, OBSSession } from '../lib/types';
import { loadOBSCommands, saveOBSCommands } from '../lib/storage';
import { Play, Save, X, Edit2, Trash, Plus, Eye, EyeOff, Video, Radio, Settings } from 'lucide-react';
import styles from './OBSCommandEditor.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

interface OBSCommandEditorProps {
    sessions: OBSSession[];
    onCommandSelect?: (commandId: string) => void; // For playlist integration
}

export function OBSCommandEditor({ sessions, onCommandSelect }: OBSCommandEditorProps) {
    const [commands, setCommands] = useState<OBSCommand[]>([]);
    const [isAddingCmd, setIsAddingCmd] = useState(false);
    const [editingCmd, setEditingCmd] = useState<OBSCommand | null>(null);
    const [cmdForm, setCmdForm] = useState<Partial<OBSCommand>>({});

    useEffect(() => {
        setCommands(loadOBSCommands());
    }, []);

    useEffect(() => {
        saveOBSCommands(commands);
    }, [commands]);

    const handleSaveCommand = () => {
        if (!cmdForm.label || !cmdForm.commandType) return;

        const newCmd: OBSCommand = {
            id: editingCmd?.id || generateId(),
            label: cmdForm.label,
            commandType: cmdForm.commandType as OBSCommandType,
            sessionId: cmdForm.sessionId || undefined,
            sceneName: cmdForm.sceneName || undefined,
            sourceName: cmdForm.sourceName || undefined,
            sceneCollection: cmdForm.sceneCollection || undefined,
            visible: cmdForm.visible,
            recordFileName: cmdForm.recordFileName || undefined,
            recordDirectory: cmdForm.recordDirectory || undefined,
            requestType: cmdForm.requestType || undefined,
            requestData: cmdForm.requestData || undefined,
            description: cmdForm.description || undefined,
            duration: cmdForm.duration || undefined,
        };

        if (editingCmd) {
            setCommands(prev => prev.map(c => c.id === editingCmd.id ? newCmd : c));
            setEditingCmd(null);
        } else {
            setCommands(prev => [...prev, newCmd]);
        }
        setIsAddingCmd(false);
        setCmdForm({});
    };

    const handleDeleteCommand = (id: string) => {
        if (confirm('Delete this command?')) {
            setCommands(prev => prev.filter(c => c.id !== id));
        }
    };

    const startEdit = (cmd: OBSCommand) => {
        setEditingCmd(cmd);
        setCmdForm(cmd);
        setIsAddingCmd(true);
    };

    const useTemplate = (template: Partial<OBSCommand>) => {
        setCmdForm(template);
        setIsAddingCmd(true);
        setEditingCmd(null);
    };

    const getCommandIcon = (type: OBSCommandType) => {
        switch (type) {
            case 'change_scene': return <Video size={14} />;
            case 'show_source': return <Eye size={14} />;
            case 'hide_source': return <EyeOff size={14} />;
            case 'toggle_source': return <Settings size={14} />;
            case 'start_record': return <Video size={14} />;
            case 'stop_record': return <Video size={14} />;
            case 'start_stream': return <Radio size={14} />;
            case 'stop_stream': return <Radio size={14} />;
            default: return <Settings size={14} />;
        }
    };

    const getCommandTypeLabel = (type: OBSCommandType) => {
        switch (type) {
            case 'change_scene': return 'Change Scene';
            case 'show_source': return 'Show Source';
            case 'hide_source': return 'Hide Source';
            case 'toggle_source': return 'Toggle Source';
            case 'start_record': return 'Start Record';
            case 'stop_record': return 'Stop Record';
            case 'start_stream': return 'Start Stream';
            case 'stop_stream': return 'Stop Stream';
            case 'custom': return 'Custom';
            default: return type;
        }
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <span>OBS Commands</span>
                {!isAddingCmd && (
                    <button className={styles.addBtn} onClick={() => { setIsAddingCmd(true); setEditingCmd(null); setCmdForm({}); }}>
                        <Plus size={12} /> New Command
                    </button>
                )}
            </div>

            {isAddingCmd ? (
                <div className={styles.commandForm}>
                    <div className={styles.formSection}>
                        <label>Label *</label>
                        <input
                            className={styles.formInput}
                            placeholder="e.g. Start Recording Hour 14"
                            value={cmdForm.label || ''}
                            onChange={e => setCmdForm({ ...cmdForm, label: e.target.value })}
                        />
                    </div>

                    <div className={styles.formSection}>
                        <label>Command Type *</label>
                        <select
                            className={styles.formInput}
                            value={cmdForm.commandType || ''}
                            onChange={e => setCmdForm({ ...cmdForm, commandType: e.target.value as OBSCommandType })}
                        >
                            <option value="">Select type...</option>
                            <option value="change_scene">Change Scene</option>
                            <option value="change_scene_collection">Change Scene Collection</option>
                            <option value="show_source">Show Source</option>
                            <option value="hide_source">Hide Source</option>
                            <option value="toggle_source">Toggle Source</option>
                            <option value="set_source_visibility">Set Source Visibility</option>
                            <option value="start_udp_source">Start UDP Source</option>
                            <option value="start_record">Start Recording</option>
                            <option value="stop_record">Stop Recording</option>
                            <option value="start_stream">Start Streaming</option>
                            <option value="stop_stream">Stop Streaming</option>
                            <option value="custom">Custom Command</option>
                        </select>
                    </div>

                    <div className={styles.formSection}>
                        <label>Target Session</label>
                        <select
                            className={styles.formInput}
                            value={cmdForm.sessionId || ''}
                            onChange={e => setCmdForm({ ...cmdForm, sessionId: e.target.value || undefined })}
                        >
                            <option value="">All Sessions</option>
                            {sessions.map(s => (
                                <option key={s.id} value={s.id}>{s.name}</option>
                            ))}
                        </select>
                    </div>

                    {/* Scene/Source fields */}
                    {(cmdForm.commandType === 'change_scene' ||
                        cmdForm.commandType === 'show_source' ||
                        cmdForm.commandType === 'hide_source' ||
                        cmdForm.commandType === 'toggle_source' ||
                        cmdForm.commandType === 'set_source_visibility' ||
                        cmdForm.commandType === 'start_udp_source') && (
                            <>
                                <div className={styles.formSection}>
                                    <label>Scene Name *</label>
                                    <input
                                        className={styles.formInput}
                                        placeholder="e.g. Main Scene"
                                        value={cmdForm.sceneName || ''}
                                        onChange={e => setCmdForm({ ...cmdForm, sceneName: e.target.value })}
                                    />
                                </div>
                                {(cmdForm.commandType === 'show_source' ||
                                    cmdForm.commandType === 'hide_source' ||
                                    cmdForm.commandType === 'toggle_source' ||
                                    cmdForm.commandType === 'set_source_visibility' ||
                                    cmdForm.commandType === 'start_udp_source') && (
                                        <div className={styles.formSection}>
                                            <label>Source Name *</label>
                                            <input
                                                className={styles.formInput}
                                                placeholder="e.g. Camera 1"
                                                value={cmdForm.sourceName || ''}
                                                onChange={e => setCmdForm({ ...cmdForm, sourceName: e.target.value })}
                                            />
                                        </div>
                                    )}
                                {cmdForm.commandType === 'set_source_visibility' && (
                                    <div className={styles.formSection}>
                                        <label>Visibility</label>
                                        <select
                                            className={styles.formInput}
                                            value={cmdForm.visible ? 'true' : 'false'}
                                            onChange={e => setCmdForm({ ...cmdForm, visible: e.target.value === 'true' })}
                                        >
                                            <option value="true">Visible</option>
                                            <option value="false">Hidden</option>
                                        </select>
                                    </div>
                                )}
                            </>
                        )}

                    {/* Scene Collection fields */}
                    {cmdForm.commandType === 'change_scene_collection' && (
                        <div className={styles.formSection}>
                            <label>Scene Collection Name *</label>
                            <input
                                className={styles.formInput}
                                placeholder="e.g. My Collection"
                                value={cmdForm.sceneCollection || ''}
                                onChange={e => setCmdForm({ ...cmdForm, sceneCollection: e.target.value })}
                            />
                        </div>
                    )}

                    {/* Recording fields */}
                    {(cmdForm.commandType === 'start_record') && (
                        <>
                            <div className={styles.formSection}>
                                <label>File Name (with wildcards)</label>
                                <input
                                    className={styles.formInput}
                                    placeholder="e.g. Record_{date}_{time}_{title}"
                                    value={cmdForm.recordFileName || ''}
                                    onChange={e => setCmdForm({ ...cmdForm, recordFileName: e.target.value })}
                                />
                                <div className={styles.helpText}>
                                    Wildcards: {'{date}'}, {'{time}'}, {'{datetime}'}, {'{title}'}, {'{slot}'}, {'{hour}'}
                                </div>
                            </div>
                            <div className={styles.formSection}>
                                <label>Record Directory (optional)</label>
                                <input
                                    className={styles.formInput}
                                    placeholder="e.g. C:\\Recordings"
                                    value={cmdForm.recordDirectory || ''}
                                    onChange={e => setCmdForm({ ...cmdForm, recordDirectory: e.target.value })}
                                />
                            </div>
                        </>
                    )}

                    {/* Custom command fields */}
                    {cmdForm.commandType === 'custom' && (
                        <>
                            <div className={styles.formSection}>
                                <label>Request Type *</label>
                                <input
                                    className={styles.formInput}
                                    placeholder="e.g. SetCurrentProgramScene"
                                    value={cmdForm.requestType || ''}
                                    onChange={e => setCmdForm({ ...cmdForm, requestType: e.target.value })}
                                />
                            </div>
                            <div className={styles.formSection}>
                                <label>Request Data (JSON)</label>
                                <textarea
                                    className={styles.formTextarea}
                                    placeholder='{"sceneName": "Main Scene"}'
                                    value={cmdForm.requestData || ''}
                                    onChange={e => setCmdForm({ ...cmdForm, requestData: e.target.value })}
                                    rows={3}
                                />
                            </div>
                        </>
                    )}

                    <div className={styles.formSection}>
                        <label>Description (optional)</label>
                        <input
                            className={styles.formInput}
                            placeholder="Brief description"
                            value={cmdForm.description || ''}
                            onChange={e => setCmdForm({ ...cmdForm, description: e.target.value })}
                        />
                    </div>

                    <div className={styles.formRow}>
                        <button
                            className={styles.btn}
                            onClick={handleSaveCommand}
                            disabled={!cmdForm.label || !cmdForm.commandType}
                        >
                            <Save size={12} /> Save
                        </button>
                        <button
                            className={`${styles.btn} ${styles.btnDanger}`}
                            onClick={() => { setIsAddingCmd(false); setEditingCmd(null); setCmdForm({}); }}
                        >
                            <X size={12} /> Cancel
                        </button>
                    </div>
                </div>
            ) : (
                <>
                    {/* Quick Templates */}
                    <div className={styles.templatesSection}>
                        <div className={styles.templatesHeader}>Quick Templates</div>
                        <div className={styles.templatesGrid}>
                            <button
                                className={styles.templateBtn}
                                onClick={() => useTemplate({
                                    label: 'Start Recording',
                                    commandType: 'start_record',
                                    recordFileName: 'Record_{date}_{time}'
                                })}
                            >
                                Start Recording
                            </button>
                            <button
                                className={styles.templateBtn}
                                onClick={() => useTemplate({
                                    label: 'Stop Recording',
                                    commandType: 'stop_record'
                                })}
                            >
                                Stop Recording
                            </button>
                            <button
                                className={styles.templateBtn}
                                onClick={() => useTemplate({
                                    label: 'Start Stream',
                                    commandType: 'start_stream'
                                })}
                            >
                                Start Stream
                            </button>
                            <button
                                className={styles.templateBtn}
                                onClick={() => useTemplate({
                                    label: 'Stop Stream',
                                    commandType: 'stop_stream'
                                })}
                            >
                                Stop Stream
                            </button>
                        </div>
                    </div>

                    {/* Command List */}
                    <div className={styles.commandList}>
                        {commands.map(cmd => (
                            <div key={cmd.id} className={styles.commandRow}>
                                <div className={styles.commandIcon}>
                                    {getCommandIcon(cmd.commandType)}
                                </div>
                                <div className={styles.commandInfo}>
                                    <div className={styles.commandLabel}>{cmd.label}</div>
                                    <div className={styles.commandMeta}>
                                        {getCommandTypeLabel(cmd.commandType)}
                                        {cmd.sessionId && ` • ${sessions.find(s => s.id === cmd.sessionId)?.name || cmd.sessionId}`}
                                        {cmd.sceneName && ` • Scene: ${cmd.sceneName}`}
                                        {cmd.sourceName && ` • Source: ${cmd.sourceName}`}
                                    </div>
                                </div>
                                {onCommandSelect && (
                                    <button
                                        className={styles.iconBtn}
                                        onClick={() => onCommandSelect(cmd.id)}
                                        title="Add to Playlist"
                                    >
                                        <Plus size={12} />
                                    </button>
                                )}
                                <button
                                    className={styles.iconBtn}
                                    onClick={() => startEdit(cmd)}
                                    title="Edit"
                                >
                                    <Edit2 size={12} />
                                </button>
                                <button
                                    className={styles.iconBtn}
                                    onClick={() => handleDeleteCommand(cmd.id)}
                                    title="Delete"
                                >
                                    <Trash size={12} />
                                </button>
                            </div>
                        ))}
                        {commands.length === 0 && (
                            <div className={styles.emptyState}>
                                No commands saved. Create one to get started.
                            </div>
                        )}
                    </div>
                </>
            )}
        </div>
    );
}




