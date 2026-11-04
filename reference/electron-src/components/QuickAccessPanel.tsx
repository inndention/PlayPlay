/**
 * QUICK ACCESS PANEL
 * Professional broadcast hot-key panel for instant operations
 * Inspired by: Ross Video Dashboard, Vizrt Trio, Chyron LUCI
 * 
 * Features:
 * - Customizable hot buttons
 * - Macro sequences
 * - GPI trigger buttons
 * - Media shortcuts
 * - Graphics recall
 * - Voice command indicators
 */

import React, { useState, useCallback, useMemo, useEffect } from 'react';
import {
    Zap, Play, Pause, Square, SkipForward, Radio,
    Mic, MicOff, Volume2, VolumeX, Camera, Tv,
    Image, Film, FileText, Clock, Target, Layers,
    AlertTriangle, Settings, Grid, Command, Hash,
    ChevronDown, ChevronRight, Plus, Trash2, Edit3,
    Copy, Save, RotateCcw, RefreshCw, Shuffle
} from 'lucide-react';
import styles from './QuickAccessPanel.module.css';

// Types
export interface QuickButton {
    id: string;
    label: string;
    shortcut?: string;
    icon?: string;
    color: 'red' | 'green' | 'blue' | 'yellow' | 'orange' | 'purple' | 'cyan' | 'neutral';
    size: 'small' | 'medium' | 'large';
    type: 'instant' | 'toggle' | 'hold' | 'sequence';
    isToggled?: boolean;
    isDisabled?: boolean;
    isLocked?: boolean;
    category?: string;
    action: QuickAction;
}

export interface QuickAction {
    type: 'media' | 'graphics' | 'audio' | 'gpi' | 'macro' | 'scene' | 'command' | 'custom';
    target?: string;
    parameters?: Record<string, any>;
    sequence?: QuickAction[];
}

export interface ButtonCategory {
    id: string;
    name: string;
    icon?: React.ReactNode;
    buttons: QuickButton[];
    isExpanded?: boolean;
}

interface QuickAccessPanelProps {
    categories?: ButtonCategory[];
    onButtonPress?: (button: QuickButton) => void;
    onButtonRelease?: (button: QuickButton) => void;
    onCategoryToggle?: (categoryId: string) => void;
    onEditMode?: (enabled: boolean) => void;
    showLabels?: boolean;
    gridColumns?: number;
    isEditMode?: boolean;
}

// Icon mapping
const iconMap: Record<string, React.ReactNode> = {
    play: <Play size={20} />,
    pause: <Pause size={20} />,
    stop: <Square size={20} />,
    skip: <SkipForward size={20} />,
    live: <Radio size={20} />,
    mic: <Mic size={20} />,
    micOff: <MicOff size={20} />,
    volume: <Volume2 size={20} />,
    mute: <VolumeX size={20} />,
    camera: <Camera size={20} />,
    tv: <Tv size={20} />,
    image: <Image size={20} />,
    film: <Film size={20} />,
    text: <FileText size={20} />,
    clock: <Clock size={20} />,
    target: <Target size={20} />,
    layers: <Layers size={20} />,
    alert: <AlertTriangle size={20} />,
    zap: <Zap size={20} />,
    command: <Command size={20} />,
    hash: <Hash size={20} />,
    refresh: <RefreshCw size={20} />,
    shuffle: <Shuffle size={20} />,
};

// Default categories
const defaultCategories: ButtonCategory[] = [
    {
        id: 'transport',
        name: 'Transport',
        icon: <Play size={16} />,
        isExpanded: true,
        buttons: [
            { id: 'play', label: 'PLAY', shortcut: 'Space', icon: 'play', color: 'green', size: 'large', type: 'instant', action: { type: 'command', target: 'play' } },
            { id: 'pause', label: 'PAUSE', shortcut: 'P', icon: 'pause', color: 'yellow', size: 'medium', type: 'toggle', action: { type: 'command', target: 'pause' } },
            { id: 'stop', label: 'STOP', shortcut: 'Esc', icon: 'stop', color: 'red', size: 'medium', type: 'instant', action: { type: 'command', target: 'stop' } },
            { id: 'next', label: 'NEXT', shortcut: 'N', icon: 'skip', color: 'blue', size: 'medium', type: 'instant', action: { type: 'command', target: 'next' } },
        ]
    },
    {
        id: 'live',
        name: 'Live Operations',
        icon: <Radio size={16} />,
        isExpanded: true,
        buttons: [
            { id: 'take', label: 'TAKE', shortcut: 'T', icon: 'zap', color: 'red', size: 'large', type: 'instant', action: { type: 'command', target: 'take' } },
            { id: 'cue', label: 'CUE', shortcut: 'C', icon: 'target', color: 'cyan', size: 'medium', type: 'instant', action: { type: 'command', target: 'cue' } },
            { id: 'recue', label: 'RECUE', shortcut: 'R', icon: 'refresh', color: 'orange', size: 'medium', type: 'instant', action: { type: 'command', target: 'recue' } },
            { id: 'hold', label: 'HOLD', shortcut: 'H', icon: 'pause', color: 'yellow', size: 'medium', type: 'toggle', action: { type: 'command', target: 'hold' } },
        ]
    },
    {
        id: 'graphics',
        name: 'Graphics',
        icon: <Layers size={16} />,
        isExpanded: false,
        buttons: [
            { id: 'cg1', label: 'CG 1', icon: 'layers', color: 'purple', size: 'medium', type: 'toggle', action: { type: 'graphics', target: 'cg1' } },
            { id: 'cg2', label: 'CG 2', icon: 'layers', color: 'purple', size: 'medium', type: 'toggle', action: { type: 'graphics', target: 'cg2' } },
            { id: 'lower', label: 'L/3', icon: 'text', color: 'blue', size: 'medium', type: 'toggle', action: { type: 'graphics', target: 'lower_third' } },
            { id: 'bug', label: 'BUG', icon: 'image', color: 'cyan', size: 'medium', type: 'toggle', action: { type: 'graphics', target: 'bug' } },
        ]
    },
    {
        id: 'audio',
        name: 'Audio',
        icon: <Volume2 size={16} />,
        isExpanded: false,
        buttons: [
            { id: 'mute_all', label: 'MUTE', icon: 'mute', color: 'red', size: 'medium', type: 'toggle', action: { type: 'audio', target: 'mute_all' } },
            { id: 'mic1', label: 'MIC 1', icon: 'mic', color: 'green', size: 'small', type: 'toggle', action: { type: 'audio', target: 'mic1' } },
            { id: 'mic2', label: 'MIC 2', icon: 'mic', color: 'green', size: 'small', type: 'toggle', action: { type: 'audio', target: 'mic2' } },
            { id: 'music', label: 'MUSIC', icon: 'volume', color: 'blue', size: 'small', type: 'toggle', action: { type: 'audio', target: 'music' } },
        ]
    },
    {
        id: 'gpi',
        name: 'GPI Triggers',
        icon: <Zap size={16} />,
        isExpanded: false,
        buttons: [
            { id: 'gpi1', label: 'GPI 1', shortcut: 'F1', icon: 'zap', color: 'neutral', size: 'small', type: 'instant', action: { type: 'gpi', target: '1' } },
            { id: 'gpi2', label: 'GPI 2', shortcut: 'F2', icon: 'zap', color: 'neutral', size: 'small', type: 'instant', action: { type: 'gpi', target: '2' } },
            { id: 'gpi3', label: 'GPI 3', shortcut: 'F3', icon: 'zap', color: 'neutral', size: 'small', type: 'instant', action: { type: 'gpi', target: '3' } },
            { id: 'gpi4', label: 'GPI 4', shortcut: 'F4', icon: 'zap', color: 'neutral', size: 'small', type: 'instant', action: { type: 'gpi', target: '4' } },
        ]
    },
];

// Quick Button Component
const QuickButtonComponent: React.FC<{
    button: QuickButton;
    showLabel: boolean;
    isEditMode: boolean;
    onPress: () => void;
    onRelease: () => void;
}> = ({ button, showLabel, isEditMode, onPress, onRelease }) => {
    const [isPressed, setIsPressed] = useState(false);

    const handleMouseDown = () => {
        if (button.isDisabled || button.isLocked) return;
        setIsPressed(true);
        onPress();
    };

    const handleMouseUp = () => {
        if (button.isDisabled || button.isLocked) return;
        setIsPressed(false);
        if (button.type === 'hold') {
            onRelease();
        }
    };

    const icon = button.icon ? iconMap[button.icon] : <Command size={20} />;

    return (
        <button
            className={`
                ${styles.quickButton} 
                ${styles[`color${button.color}`]} 
                ${styles[`size${button.size}`]}
                ${button.isToggled ? styles.toggled : ''}
                ${isPressed ? styles.pressed : ''}
                ${button.isDisabled ? styles.disabled : ''}
                ${button.isLocked ? styles.locked : ''}
                ${isEditMode ? styles.editMode : ''}
            `}
            onMouseDown={handleMouseDown}
            onMouseUp={handleMouseUp}
            onMouseLeave={() => isPressed && handleMouseUp()}
            disabled={button.isDisabled}
        >
            <div className={styles.buttonContent}>
                <span className={styles.buttonIcon}>{icon}</span>
                {showLabel && (
                    <span className={styles.buttonLabel}>{button.label}</span>
                )}
            </div>
            {button.shortcut && (
                <span className={styles.buttonShortcut}>{button.shortcut}</span>
            )}
            {button.isToggled && (
                <span className={styles.toggleIndicator} />
            )}
            {isEditMode && (
                <div className={styles.editOverlay}>
                    <Edit3 size={14} />
                </div>
            )}
        </button>
    );
};

// Main Component
export const QuickAccessPanel: React.FC<QuickAccessPanelProps> = ({
    categories = defaultCategories,
    onButtonPress,
    onButtonRelease,
    onCategoryToggle,
    onEditMode,
    showLabels = true,
    gridColumns = 4,
    isEditMode = false,
}) => {
    const [localCategories, setLocalCategories] = useState(categories);
    const [currentTime, setCurrentTime] = useState(new Date());

    // Update clock
    useEffect(() => {
        const interval = setInterval(() => setCurrentTime(new Date()), 1000);
        return () => clearInterval(interval);
    }, []);

    const handleCategoryToggle = (categoryId: string) => {
        setLocalCategories(prev => prev.map(cat => 
            cat.id === categoryId ? { ...cat, isExpanded: !cat.isExpanded } : cat
        ));
        onCategoryToggle?.(categoryId);
    };

    const handleButtonPress = (button: QuickButton) => {
        // Update toggle state if it's a toggle button
        if (button.type === 'toggle') {
            setLocalCategories(prev => prev.map(cat => ({
                ...cat,
                buttons: cat.buttons.map(btn => 
                    btn.id === button.id ? { ...btn, isToggled: !btn.isToggled } : btn
                )
            })));
        }
        onButtonPress?.(button);
    };

    const handleButtonRelease = (button: QuickButton) => {
        onButtonRelease?.(button);
    };

    return (
        <div className={styles.container}>
            {/* Header */}
            <header className={styles.header}>
                <div className={styles.headerLeft}>
                    <h2 className={styles.title}>
                        <Command size={18} />
                        Quick Access
                    </h2>
                </div>
                <div className={styles.headerRight}>
                    <span className={styles.clock}>
                        {currentTime.toLocaleTimeString('en-US', { hour12: false })}
                    </span>
                    <button 
                        className={`${styles.editToggle} ${isEditMode ? styles.active : ''}`}
                        onClick={() => onEditMode?.(!isEditMode)}
                        title="Edit Mode"
                    >
                        <Settings size={16} />
                    </button>
                </div>
            </header>

            {/* Categories */}
            <div className={styles.categoriesContainer}>
                {localCategories.map(category => (
                    <div key={category.id} className={styles.category}>
                        <button 
                            className={styles.categoryHeader}
                            onClick={() => handleCategoryToggle(category.id)}
                        >
                            {category.isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                            {category.icon}
                            <span>{category.name}</span>
                            <span className={styles.categoryCount}>{category.buttons.length}</span>
                        </button>

                        {category.isExpanded && (
                            <div 
                                className={styles.buttonsGrid}
                                style={{ gridTemplateColumns: `repeat(${gridColumns}, 1fr)` }}
                            >
                                {category.buttons.map(button => (
                                    <QuickButtonComponent
                                        key={button.id}
                                        button={button}
                                        showLabel={showLabels}
                                        isEditMode={isEditMode}
                                        onPress={() => handleButtonPress(button)}
                                        onRelease={() => handleButtonRelease(button)}
                                    />
                                ))}
                                {isEditMode && (
                                    <button className={styles.addButton}>
                                        <Plus size={20} />
                                    </button>
                                )}
                            </div>
                        )}
                    </div>
                ))}
            </div>

            {/* Status Bar */}
            <footer className={styles.statusBar}>
                <span className={styles.statusItem}>
                    <Grid size={12} />
                    {localCategories.reduce((sum, cat) => sum + cat.buttons.length, 0)} buttons
                </span>
                {isEditMode && (
                    <span className={styles.editIndicator}>
                        <Edit3 size={12} />
                        Edit Mode
                    </span>
                )}
            </footer>
        </div>
    );
};

export default QuickAccessPanel;
