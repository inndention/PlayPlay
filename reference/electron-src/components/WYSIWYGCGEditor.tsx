// ============================================
// WYSIWYG CG EDITOR - BROADCAST EDITION
// Professional broadcast graphics editor
// Lower thirds, OTS boxes, score bugs, tickers, etc.
// ============================================

import React, { useState, useCallback, useRef, useEffect } from 'react';
import { CGElement, CGType, CGPreset, CGPosition, CGColors, CGEffects } from '../lib/types';
import { loadCGElements, saveCGElements, loadCGPresets, saveCGPresets } from '../lib/storage';
import { safePrompt } from '../lib/promptPolyfill';
import { CGCanvas } from './CGCanvas';
import {
    Type, Square, Circle, Image, Layers, Save, Eye, EyeOff,
    Lock, Unlock, Copy, Trash2, Plus, Download, Upload,
    AlignLeft, AlignCenter, AlignRight, Bold, Italic,
    Palette, Move, Maximize2, Grid, Settings, Tv,
    MessageSquare, Clock, Trophy, TrendingUp, Radio, Zap,
    LayoutGrid, Monitor, PlayCircle, StopCircle
} from 'lucide-react';
import styles from './WYSIWYGCGEditor.module.css';

const generateId = () => Math.random().toString(36).substr(2, 9);

interface WYSIWYGCGEditorProps {
    initialElements?: CGElement[];
    onSave?: (elements: CGElement[]) => void;
    onClose?: () => void;
    channelId?: string;
}

// Broadcast-centric CG templates
const BROADCAST_TEMPLATES: CGPreset[] = [
    // LOWER THIRDS
    {
        id: 'lt_modern',
        name: 'Modern Lower Third',
        category: 'Lower Thirds',
        element: {
            name: 'Modern L3',
            type: 'lower_third',
            title: 'John Smith',
            subtitle: 'Senior Correspondent',
            colors: {
                primary: '#1e40af',
                secondary: '#3b82f6',
                text: '#ffffff',
                gradient: true,
                gradientDirection: 'horizontal',
            },
            effects: {
                shadow: true,
                shadowBlur: 8,
                shadowColor: 'rgba(0,0,0,0.5)',
                glow: false,
                borderRadius: 4,
                opacity: 100,
            },
            position: { x: 5, y: 78, width: 35, height: 12, anchor: 'bottom-left' },
            fontSize: 32,
            fontFamily: 'Inter',
            animationIn: 'slide_left',
            animationOut: 'slide_left',
            animationDuration: 500,
            loop: 'none',
            autoHideAfter: 8000,
            isPreset: true,
            presetCategory: 'Lower Thirds',
        },
    },
    {
        id: 'lt_breaking',
        name: 'Breaking News L3',
        category: 'Lower Thirds',
        element: {
            name: 'Breaking L3',
            type: 'lower_third',
            title: 'BREAKING NEWS',
            subtitle: 'Details developing...',
            colors: {
                primary: '#dc2626',
                secondary: '#b91c1c',
                text: '#ffffff',
                gradient: true,
                gradientDirection: 'horizontal',
            },
            effects: {
                shadow: true,
                shadowBlur: 12,
                shadowColor: 'rgba(220,38,38,0.4)',
                glow: true,
                glowColor: '#dc2626',
                glowIntensity: 20,
                borderRadius: 0,
                opacity: 100,
            },
            position: { x: 0, y: 75, width: 100, height: 15, anchor: 'bottom-left' },
            fontSize: 42,
            fontFamily: 'Impact',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 300,
            loop: 'pulse',
            loopDuration: 2000,
            isPreset: true,
            presetCategory: 'Lower Thirds',
        },
    },
    {
        id: 'lt_corporate',
        name: 'Corporate Lower Third',
        category: 'Lower Thirds',
        element: {
            name: 'Corporate L3',
            type: 'lower_third',
            title: 'Company Executive',
            subtitle: 'Chief Executive Officer',
            colors: {
                primary: '#0f172a',
                secondary: '#1e293b',
                text: '#f8fafc',
                gradient: false,
            },
            effects: {
                shadow: false,
                glow: false,
                borderRadius: 0,
                opacity: 95,
                glassMorphism: true,
            },
            position: { x: 5, y: 80, width: 30, height: 10, anchor: 'bottom-left' },
            fontSize: 28,
            fontFamily: 'Helvetica Neue',
            animationIn: 'slide_up',
            animationOut: 'slide_down',
            animationDuration: 400,
            loop: 'none',
            autoHideAfter: 6000,
            isPreset: true,
            presetCategory: 'Lower Thirds',
        },
    },
    // OTS (Over The Shoulder) BOXES
    {
        id: 'ots_standard',
        name: 'Standard OTS Box',
        category: 'OTS Boxes',
        element: {
            name: 'OTS Box',
            type: 'custom',
            title: 'HEADLINE',
            colors: {
                primary: '#1e40af',
                secondary: '#ffffff',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 15,
                shadowColor: 'rgba(0,0,0,0.4)',
                glow: false,
                borderRadius: 8,
                opacity: 100,
            },
            position: { x: 60, y: 15, width: 35, height: 45, anchor: 'top-right' },
            fontSize: 36,
            fontFamily: 'Arial Black',
            animationIn: 'slide_right',
            animationOut: 'slide_right',
            animationDuration: 500,
            loop: 'none',
            isPreset: true,
            presetCategory: 'OTS Boxes',
        },
    },
    {
        id: 'ots_map',
        name: 'Map OTS Box',
        category: 'OTS Boxes',
        element: {
            name: 'Map OTS',
            type: 'custom',
            title: 'LOCATION',
            colors: {
                primary: '#166534',
                secondary: '#22c55e',
                text: '#ffffff',
                gradient: true,
                gradientDirection: 'vertical',
            },
            effects: {
                shadow: true,
                shadowBlur: 20,
                shadowColor: 'rgba(22,101,52,0.3)',
                glow: false,
                borderRadius: 12,
                opacity: 100,
            },
            position: { x: 58, y: 12, width: 38, height: 50, anchor: 'top-right' },
            fontSize: 28,
            fontFamily: 'Roboto',
            animationIn: 'zoom',
            animationOut: 'zoom',
            animationDuration: 400,
            loop: 'none',
            isPreset: true,
            presetCategory: 'OTS Boxes',
        },
    },
    // SCORE BUGS
    {
        id: 'score_simple',
        name: 'Simple Score Bug',
        category: 'Score Bugs',
        element: {
            name: 'Score Bug',
            type: 'custom',
            title: 'HOME 0 - 0 AWAY',
            colors: {
                primary: '#18181b',
                secondary: '#27272a',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 6,
                shadowColor: 'rgba(0,0,0,0.5)',
                glow: false,
                borderRadius: 6,
                opacity: 95,
            },
            position: { x: 3, y: 5, width: 25, height: 8, anchor: 'top-left' },
            fontSize: 24,
            fontFamily: 'Roboto Condensed',
            animationIn: 'slide_down',
            animationOut: 'slide_up',
            animationDuration: 300,
            loop: 'none',
            isPreset: true,
            presetCategory: 'Score Bugs',
        },
    },
    {
        id: 'score_sports',
        name: 'Sports Score Bug',
        category: 'Score Bugs',
        element: {
            name: 'Sports Bug',
            type: 'custom',
            title: 'Q4  12:45',
            subtitle: 'GAME OF THE WEEK',
            colors: {
                primary: '#991b1b',
                secondary: '#1e3a8a',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 8,
                shadowColor: 'rgba(0,0,0,0.4)',
                glow: false,
                borderRadius: 4,
                opacity: 100,
            },
            position: { x: 2, y: 3, width: 30, height: 12, anchor: 'top-left' },
            fontSize: 28,
            fontFamily: 'Oswald',
            animationIn: 'slide_left',
            animationOut: 'slide_left',
            animationDuration: 400,
            loop: 'none',
            isPreset: true,
            presetCategory: 'Score Bugs',
        },
    },
    // TICKERS
    {
        id: 'ticker_news',
        name: 'News Ticker',
        category: 'Tickers',
        element: {
            name: 'News Ticker',
            type: 'ticker',
            title: 'BREAKING',
            tickerText: 'Breaking news headlines scroll across the screen...',
            colors: {
                primary: '#dc2626',
                secondary: '#0f172a',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 4,
                shadowColor: 'rgba(0,0,0,0.3)',
                glow: false,
                borderRadius: 0,
                opacity: 100,
            },
            position: { x: 0, y: 92, width: 100, height: 8, anchor: 'bottom-left' },
            fontSize: 22,
            fontFamily: 'Arial',
            animationIn: 'slide_up',
            animationOut: 'slide_down',
            animationDuration: 500,
            loop: 'ticker_scroll',
            loopDuration: 30000,
            isPreset: true,
            presetCategory: 'Tickers',
        },
    },
    {
        id: 'ticker_sports',
        name: 'Sports Scores Ticker',
        category: 'Tickers',
        element: {
            name: 'Scores Ticker',
            type: 'ticker',
            title: 'SCORES',
            tickerText: 'NYY 5 - BOS 3 | LAD 2 - SFG 1 | CHC 7 - STL 4',
            colors: {
                primary: '#1e3a8a',
                secondary: '#1e40af',
                text: '#ffffff',
                gradient: true,
                gradientDirection: 'horizontal',
            },
            effects: {
                shadow: false,
                glow: false,
                borderRadius: 0,
                opacity: 95,
            },
            position: { x: 0, y: 94, width: 100, height: 6, anchor: 'bottom-left' },
            fontSize: 18,
            fontFamily: 'Roboto Condensed',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 300,
            loop: 'ticker_scroll',
            loopDuration: 20000,
            isPreset: true,
            presetCategory: 'Tickers',
        },
    },
    // BUGS / WATERMARKS
    {
        id: 'bug_live',
        name: 'LIVE Bug',
        category: 'Bugs',
        element: {
            name: 'LIVE Bug',
            type: 'bug',
            title: 'LIVE',
            colors: {
                primary: '#dc2626',
                secondary: '#b91c1c',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: false,
                glow: true,
                glowColor: '#dc2626',
                glowIntensity: 15,
                borderRadius: 4,
                opacity: 100,
            },
            position: { x: 3, y: 5, width: 8, height: 4, anchor: 'top-left' },
            fontSize: 18,
            fontFamily: 'Arial Black',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 200,
            loop: 'pulse',
            loopDuration: 1500,
            isPreset: true,
            presetCategory: 'Bugs',
        },
    },
    {
        id: 'bug_recording',
        name: 'Recording Bug',
        category: 'Bugs',
        element: {
            name: 'REC Bug',
            type: 'bug',
            title: '● REC',
            colors: {
                primary: 'transparent',
                secondary: 'transparent',
                text: '#ef4444',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 4,
                shadowColor: 'rgba(0,0,0,0.5)',
                glow: false,
                borderRadius: 0,
                opacity: 100,
            },
            position: { x: 92, y: 5, width: 6, height: 3, anchor: 'top-right' },
            fontSize: 14,
            fontFamily: 'monospace',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 150,
            loop: 'pulse',
            loopDuration: 1000,
            isPreset: true,
            presetCategory: 'Bugs',
        },
    },
    {
        id: 'bug_logo',
        name: 'Logo Bug',
        category: 'Bugs',
        element: {
            name: 'Logo Bug',
            type: 'bug',
            title: '',
            imageSrc: '',
            colors: {
                primary: 'transparent',
                secondary: 'transparent',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 6,
                shadowColor: 'rgba(0,0,0,0.3)',
                glow: false,
                borderRadius: 0,
                opacity: 75,
            },
            position: { x: 92, y: 5, width: 6, height: 6, anchor: 'top-right' },
            fontSize: 12,
            fontFamily: 'Arial',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 500,
            loop: 'none',
            isPreset: true,
            presetCategory: 'Bugs',
        },
    },
    // CLOCKS
    {
        id: 'clock_digital',
        name: 'Digital Clock',
        category: 'Clocks',
        element: {
            name: 'Digital Clock',
            type: 'clock',
            title: '12:00:00',
            colors: {
                primary: 'rgba(0,0,0,0.6)',
                secondary: '#000000',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: false,
                glow: false,
                borderRadius: 4,
                opacity: 80,
            },
            position: { x: 90, y: 90, width: 8, height: 5, anchor: 'bottom-right' },
            fontSize: 20,
            fontFamily: 'Roboto Mono',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 300,
            loop: 'none',
            isPreset: true,
            presetCategory: 'Clocks',
        },
    },
    // FULLSCREEN
    {
        id: 'fs_title_card',
        name: 'Title Card',
        category: 'Fullscreen',
        element: {
            name: 'Title Card',
            type: 'custom',
            title: 'PROGRAM TITLE',
            subtitle: 'Subtitle or Description',
            colors: {
                primary: '#0f172a',
                secondary: '#1e293b',
                text: '#f8fafc',
                gradient: true,
                gradientDirection: 'vertical',
            },
            effects: {
                shadow: false,
                glow: false,
                borderRadius: 0,
                opacity: 100,
            },
            position: { x: 0, y: 0, width: 100, height: 100, anchor: 'center' },
            fontSize: 72,
            fontFamily: 'Playfair Display',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 800,
            loop: 'none',
            autoHideAfter: 5000,
            isPreset: true,
            presetCategory: 'Fullscreen',
        },
    },
    {
        id: 'fs_coming_up',
        name: 'Coming Up Next',
        category: 'Fullscreen',
        element: {
            name: 'Coming Up',
            type: 'custom',
            title: 'COMING UP NEXT',
            subtitle: 'Program Name',
            colors: {
                primary: '#7c3aed',
                secondary: '#4c1d95',
                text: '#ffffff',
                gradient: true,
                gradientDirection: 'diagonal',
            },
            effects: {
                shadow: true,
                shadowBlur: 30,
                shadowColor: 'rgba(124,58,237,0.4)',
                glow: true,
                glowColor: '#7c3aed',
                glowIntensity: 20,
                borderRadius: 0,
                opacity: 100,
            },
            position: { x: 0, y: 0, width: 100, height: 100, anchor: 'center' },
            fontSize: 56,
            fontFamily: 'Poppins',
            animationIn: 'zoom',
            animationOut: 'fade',
            animationDuration: 600,
            loop: 'shimmer',
            loopDuration: 3000,
            autoHideAfter: 7000,
            isPreset: true,
            presetCategory: 'Fullscreen',
        },
    },
];

// Template categories for the sidebar
const TEMPLATE_CATEGORIES = [
    { id: 'lower_thirds', name: 'Lower Thirds', icon: <MessageSquare size={16} /> },
    { id: 'ots_boxes', name: 'OTS Boxes', icon: <Tv size={16} /> },
    { id: 'score_bugs', name: 'Score Bugs', icon: <Trophy size={16} /> },
    { id: 'tickers', name: 'Tickers', icon: <TrendingUp size={16} /> },
    { id: 'bugs', name: 'Bugs', icon: <Radio size={16} /> },
    { id: 'clocks', name: 'Clocks', icon: <Clock size={16} /> },
    { id: 'fullscreen', name: 'Fullscreen', icon: <Maximize2 size={16} /> },
];

export function WYSIWYGCGEditor({
    initialElements = [],
    onSave,
    onClose,
    channelId
}: WYSIWYGCGEditorProps) {
    const [elements, setElements] = useState<CGElement[]>(initialElements);
    const [selectedElementId, setSelectedElementId] = useState<string | null>(null);
    const [showGrid, setShowGrid] = useState(true);
    const [showSafeZone, setShowSafeZone] = useState(true);
    const [snapToGrid, setSnapToGrid] = useState(true);
    const [templates, setTemplates] = useState<CGPreset[]>([...BROADCAST_TEMPLATES]);
    const [showTemplateMenu, setShowTemplateMenu] = useState(false);
    const [activeCategory, setActiveCategory] = useState<string | null>(null);
    const [isPreviewMode, setIsPreviewMode] = useState(false);

    // Load saved templates
    useEffect(() => {
        const saved = loadCGPresets();
        if (saved && saved.length > 0) {
            setTemplates([...BROADCAST_TEMPLATES, ...saved]);
        }
    }, []);

    const selectedElement = elements.find(el => el.id === selectedElementId);

    const handleAddElement = useCallback((type: 'text' | 'rectangle' | 'circle' | 'image' | 'lower_third' | 'ticker' | 'bug' | 'clock') => {
        const baseElement: CGElement = {
            id: generateId(),
            name: `Element ${elements.length + 1}`,
            type: type === 'text' || type === 'rectangle' || type === 'circle' ? 'custom' : type as CGType,
            title: type === 'text' ? 'New Text' : type === 'lower_third' ? 'Name' : type === 'ticker' ? 'Ticker' : '',
            subtitle: type === 'lower_third' ? 'Title' : undefined,
            tickerText: type === 'ticker' ? 'Breaking news ticker text...' : undefined,
            colors: {
                primary: type === 'lower_third' ? '#1e40af' : '#3b82f6',
                secondary: '#1e40af',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowBlur: 8,
                shadowColor: 'rgba(0,0,0,0.4)',
                glow: false,
                borderRadius: type === 'circle' ? 50 : 4,
                opacity: 100,
            },
            position: {
                x: 50,
                y: type === 'lower_third' ? 80 : type === 'ticker' ? 92 : type === 'bug' ? 5 : 50,
                width: type === 'ticker' ? 100 : type === 'lower_third' ? 30 : type === 'bug' ? 8 : 20,
                height: type === 'ticker' ? 6 : type === 'lower_third' ? 10 : type === 'bug' ? 4 : 10,
                anchor: type === 'lower_third' ? 'bottom-left' : type === 'ticker' ? 'bottom-left' : type === 'bug' ? 'top-left' : 'center',
            },
            fontSize: type === 'ticker' ? 20 : type === 'lower_third' ? 28 : type === 'bug' ? 16 : 48,
            fontFamily: 'Arial',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 500,
            loop: type === 'ticker' ? 'ticker_scroll' : 'none',
            loopDuration: type === 'ticker' ? 30000 : undefined,
            isVisible: true,
            isPreset: false,
        };
        
        setElements(prev => [...prev, baseElement]);
        setSelectedElementId(baseElement.id);
    }, [elements.length]);

    const handleDeleteElement = useCallback((elementId: string) => {
        setElements(prev => prev.filter(el => el.id !== elementId));
        if (selectedElementId === elementId) {
            setSelectedElementId(null);
        }
    }, [selectedElementId]);

    const handleDuplicateElement = useCallback((elementId: string) => {
        const element = elements.find(el => el.id === elementId);
        if (!element) return;
        
        const duplicated: CGElement = {
            ...element,
            id: generateId(),
            name: `${element.name} (Copy)`,
            position: {
                ...element.position,
                x: element.position.x + 2,
                y: element.position.y + 2,
            },
        };
        
        setElements(prev => [...prev, duplicated]);
        setSelectedElementId(duplicated.id);
    }, [elements]);

    const handleUpdateElement = useCallback((elementId: string, updates: Partial<CGElement>) => {
        setElements(prev => prev.map(el => 
            el.id === elementId ? { ...el, ...updates } : el
        ));
    }, []);

    const handleElementMove = useCallback((elementId: string, x: number, y: number) => {
        const percentX = (x / 1920) * 100;
        const percentY = (y / 1080) * 100;
        
        // Snap to safe zone if enabled
        let finalX = percentX;
        let finalY = percentY;
        
        if (showSafeZone) {
            const safeZone = { left: 5, right: 95, top: 5, bottom: 95 };
            if (percentX < safeZone.left + 2) finalX = safeZone.left;
            if (percentX > safeZone.right - 2) finalX = safeZone.right;
            if (percentY < safeZone.top + 2) finalY = safeZone.top;
            if (percentY > safeZone.bottom - 2) finalY = safeZone.bottom;
        }
        
        handleUpdateElement(elementId, {
            position: {
                ...elements.find(el => el.id === elementId)!.position,
                x: finalX,
                y: finalY,
            }
        });
    }, [elements, handleUpdateElement, showSafeZone]);

    const handleSaveTemplate = useCallback(async () => {
        if (!selectedElement) return;
        
        const templateName = await safePrompt('Template name:');
        if (!templateName) return;
        
        const category = (await safePrompt('Category (Lower Thirds, OTS Boxes, Score Bugs, Tickers, Bugs, Clocks, Fullscreen):')) || 'Custom';
        
        const preset: CGPreset = {
            id: generateId(),
            name: templateName,
            category,
            element: {
                name: selectedElement.name,
                type: selectedElement.type,
                title: selectedElement.title,
                subtitle: selectedElement.subtitle,
                tickerText: selectedElement.tickerText,
                imageSrc: selectedElement.imageSrc,
                colors: selectedElement.colors,
                effects: selectedElement.effects,
                position: selectedElement.position,
                fontSize: selectedElement.fontSize,
                fontFamily: selectedElement.fontFamily,
                animationIn: selectedElement.animationIn,
                animationOut: selectedElement.animationOut,
                animationDuration: selectedElement.animationDuration,
                loop: selectedElement.loop,
                loopDuration: selectedElement.loopDuration,
                autoHideAfter: selectedElement.autoHideAfter,
                isPreset: true,
                presetCategory: category,
            },
        };
        
        const customTemplates = templates.filter(t => !BROADCAST_TEMPLATES.some(bt => bt.id === t.id));
        const updated = [...customTemplates, preset];
        saveCGPresets(updated);
        setTemplates([...BROADCAST_TEMPLATES, ...updated]);
        alert('Template saved!');
    }, [selectedElement, templates]);

    const handleLoadTemplate = useCallback((template: CGPreset) => {
        const newElement: CGElement = {
            ...template.element,
            id: generateId(),
            isVisible: true,
        };
        
        setElements(prev => [...prev, newElement]);
        setSelectedElementId(newElement.id);
        setShowTemplateMenu(false);
    }, []);

    const handleSave = useCallback(() => {
        saveCGElements(elements);
        onSave?.(elements);
        onClose?.();
    }, [elements, onSave, onClose]);

    const filteredTemplates = activeCategory 
        ? templates.filter(t => t.category.toLowerCase().replace(/\s+/g, '_') === activeCategory)
        : templates;

    return (
        <div className={styles.container}>
            {/* Toolbar */}
            <div className={styles.toolbar}>
                <div className={styles.toolbarGroup}>
                    <span className={styles.toolbarLabel}>Add:</span>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('lower_third')}
                        title="Add Lower Third"
                    >
                        <MessageSquare size={16} />
                    </button>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('ticker')}
                        title="Add Ticker"
                    >
                        <TrendingUp size={16} />
                    </button>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('bug')}
                        title="Add Bug"
                    >
                        <Radio size={16} />
                    </button>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('text')}
                        title="Add Text"
                    >
                        <Type size={16} />
                    </button>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('rectangle')}
                        title="Add Rectangle"
                    >
                        <Square size={16} />
                    </button>
                    <button
                        className={styles.toolBtn}
                        onClick={() => handleAddElement('image')}
                        title="Add Image"
                    >
                        <Image size={16} />
                    </button>
                </div>

                <div className={styles.toolbarGroup}>
                    <button
                        className={`${styles.toolBtn} ${showGrid ? styles.active : ''}`}
                        onClick={() => setShowGrid(!showGrid)}
                        title="Toggle Grid"
                    >
                        <Grid size={16} />
                    </button>
                    <button
                        className={`${styles.toolBtn} ${showSafeZone ? styles.active : ''}`}
                        onClick={() => setShowSafeZone(!showSafeZone)}
                        title="Toggle Safe Zone"
                    >
                        <Monitor size={16} />
                    </button>
                    <button
                        className={`${styles.toolBtn} ${snapToGrid ? styles.active : ''}`}
                        onClick={() => setSnapToGrid(!snapToGrid)}
                        title="Snap to Grid"
                    >
                        <Move size={16} />
                    </button>
                </div>

                <div className={styles.toolbarGroup}>
                    <button
                        className={`${styles.toolBtn} ${isPreviewMode ? styles.previewActive : ''}`}
                        onClick={() => setIsPreviewMode(!isPreviewMode)}
                        title={isPreviewMode ? 'Exit Preview' : 'Preview Mode'}
                    >
                        {isPreviewMode ? <StopCircle size={16} /> : <PlayCircle size={16} />}
                        {isPreviewMode ? 'Exit Preview' : 'Preview'}
                    </button>
                </div>

                <div className={styles.toolbarGroup}>
                    <div className={styles.templateMenu}>
                        <button
                            className={styles.toolBtn}
                            onClick={() => setShowTemplateMenu(!showTemplateMenu)}
                            title="Templates"
                        >
                            <Layers size={16} />
                            Templates
                        </button>
                        {showTemplateMenu && (
                            <div className={styles.templateDropdown}>
                                <div className={styles.templateHeader}>
                                    <span>Broadcast Templates</span>
                                    <button onClick={() => setShowTemplateMenu(false)}>×</button>
                                </div>
                                <div className={styles.templateCategories}>
                                    <button
                                        className={`${styles.categoryBtn} ${!activeCategory ? styles.active : ''}`}
                                        onClick={() => setActiveCategory(null)}
                                    >
                                        All
                                    </button>
                                    {TEMPLATE_CATEGORIES.map(cat => (
                                        <button
                                            key={cat.id}
                                            className={`${styles.categoryBtn} ${activeCategory === cat.id ? styles.active : ''}`}
                                            onClick={() => setActiveCategory(cat.id)}
                                        >
                                            {cat.icon}
                                            {cat.name}
                                        </button>
                                    ))}
                                </div>
                                <div className={styles.templateList}>
                                    {filteredTemplates.length === 0 ? (
                                        <div className={styles.templateEmpty}>No templates in this category</div>
                                    ) : (
                                        filteredTemplates.map(template => (
                                            <button
                                                key={template.id}
                                                className={styles.templateItem}
                                                onClick={() => handleLoadTemplate(template)}
                                            >
                                                <span>{template.name}</span>
                                                <span className={styles.templateCategory}>{template.category}</span>
                                            </button>
                                        ))
                                    )}
                                </div>
                            </div>
                        )}
                    </div>
                </div>

                <div className={styles.toolbarGroup} style={{ marginLeft: 'auto' }}>
                    <button className={styles.saveBtn} onClick={handleSave}>
                        <Save size={16} />
                        Save
                    </button>
                    {onClose && (
                        <button className={styles.closeBtn} onClick={onClose}>
                            Close
                        </button>
                    )}
                </div>
            </div>

            <div className={styles.editorContent}>
                {/* Canvas */}
                <div className={styles.canvasArea}>
                    <CGCanvas
                        elements={elements}
                        selectedElementId={selectedElementId}
                        onElementSelect={setSelectedElementId}
                        onElementMove={handleElementMove}
                        showGrid={showGrid && !isPreviewMode}
                        showSafeZone={showSafeZone && !isPreviewMode}
                        snapToGrid={snapToGrid}
                        isPreviewMode={isPreviewMode}
                    />
                </div>

                {/* Properties Panel */}
                {!isPreviewMode && (
                    <div className={styles.propertiesPanel}>
                        {selectedElement ? (
                            <>
                                <div className={styles.panelSection}>
                                    <h4>Element Properties</h4>
                                    <input
                                        type="text"
                                        value={selectedElement.name}
                                        onChange={(e) => handleUpdateElement(selectedElement.id, { name: e.target.value })}
                                        className={styles.input}
                                        placeholder="Element name"
                                    />
                                    <select
                                        value={selectedElement.type}
                                        onChange={(e) => handleUpdateElement(selectedElement.id, { type: e.target.value as CGType })}
                                        className={styles.select}
                                    >
                                        <option value="lower_third">Lower Third</option>
                                        <option value="ticker">Ticker</option>
                                        <option value="bug">Bug</option>
                                        <option value="clock">Clock</option>
                                        <option value="custom">Custom</option>
                                    </select>
                                </div>

                                <div className={styles.panelSection}>
                                    <h4>Content</h4>
                                    <input
                                        type="text"
                                        value={selectedElement.title}
                                        onChange={(e) => handleUpdateElement(selectedElement.id, { title: e.target.value })}
                                        className={styles.input}
                                        placeholder="Title / Main text"
                                    />
                                    {(selectedElement.type === 'lower_third' || selectedElement.type === 'custom') && (
                                        <input
                                            type="text"
                                            value={selectedElement.subtitle || ''}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { subtitle: e.target.value })}
                                            className={styles.input}
                                            placeholder="Subtitle"
                                        />
                                    )}
                                    {selectedElement.type === 'ticker' && (
                                        <textarea
                                            value={selectedElement.tickerText || ''}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { tickerText: e.target.value })}
                                            className={styles.textarea}
                                            placeholder="Ticker text..."
                                            rows={3}
                                        />
                                    )}
                                </div>

                                <div className={styles.panelSection}>
                                    <h4>Position & Size</h4>
                                    <div className={styles.inputGrid}>
                                        <div className={styles.inputRow}>
                                            <label>X (%)</label>
                                            <input
                                                type="number"
                                                value={selectedElement.position.x.toFixed(1)}
                                                onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                    position: { ...selectedElement.position, x: parseFloat(e.target.value) || 0 }
                                                })}
                                                className={styles.numberInput}
                                            />
                                        </div>
                                        <div className={styles.inputRow}>
                                            <label>Y (%)</label>
                                            <input
                                                type="number"
                                                value={selectedElement.position.y.toFixed(1)}
                                                onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                    position: { ...selectedElement.position, y: parseFloat(e.target.value) || 0 }
                                                })}
                                                className={styles.numberInput}
                                            />
                                        </div>
                                        <div className={styles.inputRow}>
                                            <label>W (%)</label>
                                            <input
                                                type="number"
                                                value={selectedElement.position.width.toFixed(1)}
                                                onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                    position: { ...selectedElement.position, width: parseFloat(e.target.value) || 0 }
                                                })}
                                                className={styles.numberInput}
                                            />
                                        </div>
                                        <div className={styles.inputRow}>
                                            <label>H (%)</label>
                                            <input
                                                type="number"
                                                value={selectedElement.position.height.toFixed(1)}
                                                onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                    position: { ...selectedElement.position, height: parseFloat(e.target.value) || 0 }
                                                })}
                                                className={styles.numberInput}
                                            />
                                        </div>
                                    </div>
                                </div>

                                <div className={styles.panelSection}>
                                    <h4>Styling</h4>
                                    <div className={styles.colorRow}>
                                        <label>Primary</label>
                                        <input
                                            type="color"
                                            value={selectedElement.colors.primary}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                colors: { ...selectedElement.colors, primary: e.target.value }
                                            })}
                                            className={styles.colorInput}
                                        />
                                    </div>
                                    <div className={styles.colorRow}>
                                        <label>Text</label>
                                        <input
                                            type="color"
                                            value={selectedElement.colors.text}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                colors: { ...selectedElement.colors, text: e.target.value }
                                            })}
                                            className={styles.colorInput}
                                        />
                                    </div>
                                    <div className={styles.inputRow}>
                                        <label>Font Size</label>
                                        <input
                                            type="number"
                                            value={selectedElement.fontSize || 48}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                fontSize: parseInt(e.target.value, 10) || 48
                                            })}
                                            className={styles.numberInput}
                                        />
                                    </div>
                                    <div className={styles.inputRow}>
                                        <label>Font</label>
                                        <select
                                            value={selectedElement.fontFamily || 'Arial'}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { fontFamily: e.target.value })}
                                            className={styles.select}
                                        >
                                            <option value="Arial">Arial</option>
                                            <option value="Arial Black">Arial Black</option>
                                            <option value="Helvetica">Helvetica</option>
                                            <option value="Inter">Inter</option>
                                            <option value="Roboto">Roboto</option>
                                            <option value="Roboto Condensed">Roboto Condensed</option>
                                            <option value="Oswald">Oswald</option>
                                            <option value="Poppins">Poppins</option>
                                            <option value="Impact">Impact</option>
                                            <option value="Georgia">Georgia</option>
                                            <option value="Playfair Display">Playfair Display</option>
                                        </select>
                                    </div>
                                </div>

                                <div className={styles.panelSection}>
                                    <h4>Animation</h4>
                                    <div className={styles.inputRow}>
                                        <label>In</label>
                                        <select
                                            value={selectedElement.animationIn}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { animationIn: e.target.value as any })}
                                            className={styles.select}
                                        >
                                            <option value="none">None</option>
                                            <option value="fade">Fade</option>
                                            <option value="slide_left">Slide Left</option>
                                            <option value="slide_right">Slide Right</option>
                                            <option value="slide_up">Slide Up</option>
                                            <option value="slide_down">Slide Down</option>
                                            <option value="zoom">Zoom</option>
                                            <option value="bounce">Bounce</option>
                                        </select>
                                    </div>
                                    <div className={styles.inputRow}>
                                        <label>Out</label>
                                        <select
                                            value={selectedElement.animationOut}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { animationOut: e.target.value as any })}
                                            className={styles.select}
                                        >
                                            <option value="none">None</option>
                                            <option value="fade">Fade</option>
                                            <option value="slide_left">Slide Left</option>
                                            <option value="slide_right">Slide Right</option>
                                            <option value="slide_up">Slide Up</option>
                                            <option value="slide_down">Slide Down</option>
                                            <option value="zoom">Zoom</option>
                                        </select>
                                    </div>
                                    <div className={styles.inputRow}>
                                        <label>Loop</label>
                                        <select
                                            value={selectedElement.loop}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, { loop: e.target.value as any })}
                                            className={styles.select}
                                        >
                                            <option value="none">None</option>
                                            <option value="pulse">Pulse</option>
                                            <option value="sheen">Sheen</option>
                                            <option value="shimmer">Shimmer</option>
                                            <option value="glow_pulse">Glow Pulse</option>
                                            <option value="ticker_scroll">Ticker Scroll</option>
                                            <option value="bounce">Bounce</option>
                                        </select>
                                    </div>
                                    <div className={styles.inputRow}>
                                        <label>Duration</label>
                                        <input
                                            type="number"
                                            value={selectedElement.animationDuration || 500}
                                            onChange={(e) => handleUpdateElement(selectedElement.id, {
                                                animationDuration: parseInt(e.target.value, 10) || 500
                                            })}
                                            className={styles.numberInput}
                                        />
                                        <span className={styles.unit}>ms</span>
                                    </div>
                                </div>

                                <div className={styles.panelSection}>
                                    <h4>Actions</h4>
                                    <div className={styles.actionButtons}>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={() => handleDuplicateElement(selectedElement.id)}
                                        >
                                            <Copy size={14} /> Duplicate
                                        </button>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={() => handleSaveTemplate()}
                                        >
                                            <Save size={14} /> Save Template
                                        </button>
                                        <button
                                            className={`${styles.actionBtn} ${styles.danger}`}
                                            onClick={() => handleDeleteElement(selectedElement.id)}
                                        >
                                            <Trash2 size={14} /> Delete
                                        </button>
                                    </div>
                                </div>
                            </>
                        ) : (
                            <div className={styles.noSelection}>
                                <Layers size={32} strokeWidth={1} />
                                <p>Select an element to edit</p>
                                <p className={styles.hint}>or use Templates to add broadcast graphics</p>
                            </div>
                        )}
                    </div>
                )}

                {/* Layer Panel */}
                {!isPreviewMode && (
                    <div className={styles.layerPanel}>
                        <h4>Layers</h4>
                        <div className={styles.layerList}>
                            {elements.length === 0 ? (
                                <div className={styles.emptyLayers}>
                                    <p>No elements</p>
                                    <p className={styles.hint}>Add from toolbar or templates</p>
                                </div>
                            ) : (
                                elements.slice().reverse().map((el, index) => (
                                    <div
                                        key={el.id}
                                        className={`${styles.layerItem} ${selectedElementId === el.id ? styles.selected : ''}`}
                                        onClick={() => setSelectedElementId(el.id)}
                                    >
                                        <div className={styles.layerIcon}>
                                            {el.type === 'lower_third' ? <MessageSquare size={14} /> :
                                             el.type === 'ticker' ? <TrendingUp size={14} /> :
                                             el.type === 'bug' ? <Radio size={14} /> :
                                             el.type === 'clock' ? <Clock size={14} /> :
                                             <Square size={14} />}
                                        </div>
                                        <span className={styles.layerName}>{el.name}</span>
                                        <div className={styles.layerActions}>
                                            <button
                                                className={styles.layerBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleUpdateElement(el.id, { isVisible: !el.isVisible });
                                                }}
                                            >
                                                {el.isVisible ? <Eye size={12} /> : <EyeOff size={12} />}
                                            </button>
                                        </div>
                                    </div>
                                ))
                            )}
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

export default WYSIWYGCGEditor;
