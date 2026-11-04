import React, { useState } from 'react';
import { 
    Terminal, Play, Pause, Square, SkipForward, Volume2, VolumeX, 
    RefreshCw, Clock, Radio, Tv, Monitor, Layers, Zap, 
    AlertCircle, MessageSquare, Image, Film, ChevronDown, ChevronRight, GripVertical
} from 'lucide-react';
import styles from './CommandsPanel.module.css';

export interface CommandItem {
    id: string;
    type: 'command';
    command: CommandType;
    label: string;
    params?: Record<string, any>;
    duration?: number;
}

export type CommandType = 
    | 'cg_play'
    | 'cg_stop'
    | 'cg_update'
    | 'cg_next'
    | 'cg_clear'
    | 'audio_fade_in'
    | 'audio_fade_out'
    | 'audio_duck'
    | 'audio_restore'
    | 'video_fade_to_black'
    | 'video_fade_from_black'
    | 'video_transition'
    | 'live_switch'
    | 'switch_to_backup'
    | 'amcp_raw'
    | 'obs_scene_switch'
    | 'obs_source_toggle'
    | 'delay'
    | 'marker'
    | 'trigger_event'
    | 'log_message';

interface CommandDefinition {
    type: CommandType;
    label: string;
    icon: React.ReactNode;
    category: 'cg' | 'audio' | 'video' | 'switching' | 'control' | 'obs';
    description: string;
    defaultParams?: Record<string, any>;
    defaultDuration?: number;
}

const COMMAND_DEFINITIONS: CommandDefinition[] = [
    // CG Commands
    { type: 'cg_play', label: 'CG Play', icon: <Play size={14} />, category: 'cg', description: 'Play a CG template' },
    { type: 'cg_stop', label: 'CG Stop', icon: <Square size={14} />, category: 'cg', description: 'Stop CG template' },
    { type: 'cg_update', label: 'CG Update', icon: <RefreshCw size={14} />, category: 'cg', description: 'Update CG data' },
    { type: 'cg_next', label: 'CG Next', icon: <SkipForward size={14} />, category: 'cg', description: 'Advance CG state' },
    { type: 'cg_clear', label: 'CG Clear', icon: <Layers size={14} />, category: 'cg', description: 'Clear CG layer' },
    
    // Audio Commands
    { type: 'audio_fade_in', label: 'Audio Fade In', icon: <Volume2 size={14} />, category: 'audio', description: 'Fade audio in', defaultDuration: 2 },
    { type: 'audio_fade_out', label: 'Audio Fade Out', icon: <VolumeX size={14} />, category: 'audio', description: 'Fade audio out', defaultDuration: 2 },
    { type: 'audio_duck', label: 'Audio Duck', icon: <Volume2 size={14} />, category: 'audio', description: 'Duck audio level' },
    { type: 'audio_restore', label: 'Audio Restore', icon: <Volume2 size={14} />, category: 'audio', description: 'Restore audio level' },
    
    // Video Commands
    { type: 'video_fade_to_black', label: 'Fade to Black', icon: <Monitor size={14} />, category: 'video', description: 'Fade video to black', defaultDuration: 1 },
    { type: 'video_fade_from_black', label: 'Fade from Black', icon: <Monitor size={14} />, category: 'video', description: 'Fade video from black', defaultDuration: 1 },
    { type: 'video_transition', label: 'Video Transition', icon: <Film size={14} />, category: 'video', description: 'Apply video transition' },
    
    // Switching Commands
    { type: 'live_switch', label: 'Live Switch', icon: <Radio size={14} />, category: 'switching', description: 'Switch to live source' },
    { type: 'switch_to_backup', label: 'Switch to Backup', icon: <AlertCircle size={14} />, category: 'switching', description: 'Failover to backup' },
    
    // OBS Commands
    { type: 'obs_scene_switch', label: 'OBS Scene', icon: <Tv size={14} />, category: 'obs', description: 'Switch OBS scene' },
    { type: 'obs_source_toggle', label: 'OBS Source Toggle', icon: <Image size={14} />, category: 'obs', description: 'Toggle OBS source visibility' },
    
    // Control Commands
    { type: 'amcp_raw', label: 'AMCP Command', icon: <Terminal size={14} />, category: 'control', description: 'Send raw AMCP command' },
    { type: 'delay', label: 'Delay', icon: <Clock size={14} />, category: 'control', description: 'Add a timed delay', defaultDuration: 1 },
    { type: 'marker', label: 'Marker', icon: <Zap size={14} />, category: 'control', description: 'Insert a marker point', defaultDuration: 0 },
    { type: 'trigger_event', label: 'Trigger Event', icon: <Zap size={14} />, category: 'control', description: 'Trigger external event' },
    { type: 'log_message', label: 'Log Message', icon: <MessageSquare size={14} />, category: 'control', description: 'Log a message' },
];

const CATEGORIES = [
    { id: 'cg', label: 'CG Graphics', icon: <Layers size={14} /> },
    { id: 'audio', label: 'Audio Control', icon: <Volume2 size={14} /> },
    { id: 'video', label: 'Video Control', icon: <Film size={14} /> },
    { id: 'switching', label: 'Source Switching', icon: <Radio size={14} /> },
    { id: 'obs', label: 'OBS Control', icon: <Tv size={14} /> },
    { id: 'control', label: 'Control & Markers', icon: <Terminal size={14} /> },
];

interface CommandsPanelProps {
    onCommandClick?: (command: CommandItem) => void;
}

export function CommandsPanel({ onCommandClick }: CommandsPanelProps) {
    const [expandedCategories, setExpandedCategories] = useState<Set<string>>(
        new Set(['cg', 'control'])
    );
    const [searchQuery, setSearchQuery] = useState('');

    const toggleCategory = (categoryId: string) => {
        setExpandedCategories(prev => {
            const next = new Set(prev);
            if (next.has(categoryId)) {
                next.delete(categoryId);
            } else {
                next.add(categoryId);
            }
            return next;
        });
    };

    const handleDragStart = (e: React.DragEvent, def: CommandDefinition) => {
        const commandItem: CommandItem = {
            id: `cmd_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,
            type: 'command',
            command: def.type,
            label: def.label,
            params: def.defaultParams || {},
            duration: def.defaultDuration || 0,
        };
        e.dataTransfer.setData('application/json', JSON.stringify(commandItem));
        e.dataTransfer.effectAllowed = 'copy';
    };

    const handleCommandClick = (def: CommandDefinition) => {
        if (onCommandClick) {
            const commandItem: CommandItem = {
                id: `cmd_${Date.now()}_${Math.random().toString(36).substr(2, 5)}`,
                type: 'command',
                command: def.type,
                label: def.label,
                params: def.defaultParams || {},
                duration: def.defaultDuration || 0,
            };
            onCommandClick(commandItem);
        }
    };

    const filteredCommands = searchQuery
        ? COMMAND_DEFINITIONS.filter(cmd => 
            cmd.label.toLowerCase().includes(searchQuery.toLowerCase()) ||
            cmd.description.toLowerCase().includes(searchQuery.toLowerCase())
        )
        : COMMAND_DEFINITIONS;

    const getCommandsByCategory = (categoryId: string) => {
        return filteredCommands.filter(cmd => cmd.category === categoryId);
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Terminal size={16} />
                <span>Commands</span>
            </div>

            <div className={styles.searchBar}>
                <input
                    type="text"
                    placeholder="Search commands..."
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                    className={styles.searchInput}
                />
            </div>

            <div className={styles.commandsList}>
                {searchQuery ? (
                    // Flat list when searching
                    <div className={styles.flatList}>
                        {filteredCommands.map(def => (
                            <div
                                key={def.type}
                                className={styles.commandItem}
                                draggable
                                onDragStart={(e) => handleDragStart(e, def)}
                                onClick={() => handleCommandClick(def)}
                                title={def.description}
                            >
                                <GripVertical size={12} className={styles.dragHandle} />
                                {def.icon}
                                <span className={styles.commandLabel}>{def.label}</span>
                            </div>
                        ))}
                    </div>
                ) : (
                    // Categorized list
                    CATEGORIES.map(category => {
                        const commands = getCommandsByCategory(category.id);
                        if (commands.length === 0) return null;

                        const isExpanded = expandedCategories.has(category.id);

                        return (
                            <div key={category.id} className={styles.category}>
                                <div
                                    className={styles.categoryHeader}
                                    onClick={() => toggleCategory(category.id)}
                                >
                                    {isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                                    {category.icon}
                                    <span>{category.label}</span>
                                    <span className={styles.categoryCount}>{commands.length}</span>
                                </div>
                                {isExpanded && (
                                    <div className={styles.categoryContent}>
                                        {commands.map(def => (
                                            <div
                                                key={def.type}
                                                className={styles.commandItem}
                                                draggable
                                                onDragStart={(e) => handleDragStart(e, def)}
                                                onClick={() => handleCommandClick(def)}
                                                title={def.description}
                                            >
                                                <GripVertical size={12} className={styles.dragHandle} />
                                                {def.icon}
                                                <span className={styles.commandLabel}>{def.label}</span>
                                            </div>
                                        ))}
                                    </div>
                                )}
                            </div>
                        );
                    })
                )}
            </div>

            <div className={styles.footer}>
                <span className={styles.hint}>Drag commands to playlist</span>
            </div>
        </div>
    );
}

