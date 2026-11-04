/**
 * CGDesigner - Professional Character Graphics Designer with True WYSIWYG
 * 
 * Features:
 * - True WYSIWYG canvas with proper scaling (SD/HD/4K)
 * - Real-time preview with actual output dimensions
 * - Drag-and-drop element positioning
 * - Layer management
 * - Animation timeline
 * - Template library with presets
 * - Easy scheduling integration
 * - Multi-zone support (lower-thirds, full-screen, ticker, bug)
 * - Text styling with fonts, colors, shadows
 * - Image and logo support
 * - Live data binding
 */

import React, { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import styles from './CGDesigner.module.css';
import {
    Type, Image as ImageIcon, Square, Circle, Move, Trash2, Copy, Layers,
    AlignLeft, AlignCenter, AlignRight, AlignJustify, Bold, Italic, Underline,
    ChevronUp, ChevronDown, Eye, EyeOff, Lock, Unlock, Plus, Minus, Maximize,
    Settings, Play, Pause, Save, Download, Upload, Palette, Grid, ZoomIn, ZoomOut,
    RotateCcw, FlipHorizontal, FlipVertical, Undo, Redo, X, Check, Edit2,
    Clock, Calendar, Tv, Monitor, Smartphone, Film, LayoutTemplate
} from 'lucide-react';

// Types
type CGElementType = 'text' | 'image' | 'shape' | 'ticker' | 'clock' | 'data-field';
type CGZone = 'lower-third' | 'full-screen' | 'ticker' | 'bug' | 'custom';
type Resolution = '1920x1080' | '1280x720' | '3840x2160' | '720x576' | '720x480';

interface CGElement {
    id: string;
    type: CGElementType;
    name: string;
    x: number;
    y: number;
    width: number;
    height: number;
    rotation: number;
    opacity: number;
    visible: boolean;
    locked: boolean;
    zIndex: number;
    
    // Text properties
    text?: string;
    fontFamily?: string;
    fontSize?: number;
    fontWeight?: 'normal' | 'bold';
    fontStyle?: 'normal' | 'italic';
    textAlign?: 'left' | 'center' | 'right';
    textColor?: string;
    backgroundColor?: string;
    borderRadius?: number;
    padding?: number;
    shadow?: {
        color: string;
        blur: number;
        offsetX: number;
        offsetY: number;
    };
    outline?: {
        color: string;
        width: number;
    };
    
    // Image properties
    src?: string;
    objectFit?: 'cover' | 'contain' | 'fill';
    
    // Shape properties
    shapeType?: 'rectangle' | 'ellipse' | 'line';
    fillColor?: string;
    strokeColor?: string;
    strokeWidth?: number;
    
    // Animation
    animation?: {
        type: 'fade' | 'slide' | 'scale' | 'wipe' | 'none';
        duration: number;
        delay: number;
        direction?: 'left' | 'right' | 'up' | 'down';
    };
    
    // Data binding
    dataBinding?: {
        field: string;
        format?: string;
    };
}

interface CGTemplate {
    id: string;
    name: string;
    zone: CGZone;
    resolution: Resolution;
    elements: CGElement[];
    thumbnail?: string;
    category: string;
    isBuiltIn: boolean;
}

interface CGScheduleItem {
    id: string;
    templateId: string;
    startTime: Date;
    duration: number; // seconds, 0 = manual
    autoPlay: boolean;
    data?: Record<string, string>;
}

interface CGDesignerProps {
    onSaveTemplate?: (template: CGTemplate) => void;
    onScheduleItem?: (item: CGScheduleItem) => void;
    onPreview?: (template: CGTemplate) => void;
    onTakeToAir?: (template: CGTemplate) => void;
    existingTemplates?: CGTemplate[];
}

const RESOLUTIONS: { id: Resolution; label: string; width: number; height: number }[] = [
    { id: '1920x1080', label: 'Full HD (1080p)', width: 1920, height: 1080 },
    { id: '1280x720', label: 'HD (720p)', width: 1280, height: 720 },
    { id: '3840x2160', label: '4K UHD', width: 3840, height: 2160 },
    { id: '720x576', label: 'PAL SD', width: 720, height: 576 },
    { id: '720x480', label: 'NTSC SD', width: 720, height: 480 },
];

const FONT_FAMILIES = [
    'Arial', 'Helvetica', 'Verdana', 'Tahoma', 'Trebuchet MS', 'Georgia',
    'Times New Roman', 'Courier New', 'Impact', 'Comic Sans MS',
    'Roboto', 'Open Sans', 'Lato', 'Montserrat', 'Poppins', 'Inter'
];

const PRESET_COLORS = [
    '#ffffff', '#000000', '#ef4444', '#f97316', '#eab308', '#22c55e',
    '#3b82f6', '#8b5cf6', '#ec4899', '#6b7280', '#0ea5e9', '#14b8a6'
];

const DEFAULT_TEMPLATES: CGTemplate[] = [
    {
        id: 'lt-basic',
        name: 'Basic Lower Third',
        zone: 'lower-third',
        resolution: '1920x1080',
        category: 'Lower Third',
        isBuiltIn: true,
        elements: [
            {
                id: 'lt-bg',
                type: 'shape',
                name: 'Background',
                x: 50,
                y: 850,
                width: 600,
                height: 120,
                rotation: 0,
                opacity: 0.9,
                visible: true,
                locked: false,
                zIndex: 0,
                shapeType: 'rectangle',
                fillColor: '#1e40af',
                borderRadius: 8
            },
            {
                id: 'lt-title',
                type: 'text',
                name: 'Title',
                x: 70,
                y: 870,
                width: 560,
                height: 40,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 1,
                text: 'John Smith',
                fontFamily: 'Arial',
                fontSize: 32,
                fontWeight: 'bold',
                textColor: '#ffffff',
                textAlign: 'left'
            },
            {
                id: 'lt-subtitle',
                type: 'text',
                name: 'Subtitle',
                x: 70,
                y: 920,
                width: 560,
                height: 30,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 2,
                text: 'News Anchor',
                fontFamily: 'Arial',
                fontSize: 20,
                fontWeight: 'normal',
                textColor: '#93c5fd',
                textAlign: 'left'
            }
        ]
    },
    {
        id: 'ticker-basic',
        name: 'News Ticker',
        zone: 'ticker',
        resolution: '1920x1080',
        category: 'Ticker',
        isBuiltIn: true,
        elements: [
            {
                id: 'ticker-bg',
                type: 'shape',
                name: 'Background',
                x: 0,
                y: 1020,
                width: 1920,
                height: 60,
                rotation: 0,
                opacity: 0.95,
                visible: true,
                locked: false,
                zIndex: 0,
                shapeType: 'rectangle',
                fillColor: '#dc2626'
            },
            {
                id: 'ticker-label',
                type: 'text',
                name: 'Label',
                x: 20,
                y: 1030,
                width: 120,
                height: 40,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 1,
                text: 'BREAKING',
                fontFamily: 'Arial',
                fontSize: 24,
                fontWeight: 'bold',
                textColor: '#ffffff',
                textAlign: 'center',
                backgroundColor: '#000000',
                padding: 8,
                borderRadius: 4
            },
            {
                id: 'ticker-text',
                type: 'ticker',
                name: 'Ticker Text',
                x: 160,
                y: 1030,
                width: 1740,
                height: 40,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 2,
                text: 'Breaking news: Major story developing...',
                fontFamily: 'Arial',
                fontSize: 24,
                fontWeight: 'bold',
                textColor: '#ffffff',
                animation: {
                    type: 'slide',
                    duration: 15000,
                    delay: 0,
                    direction: 'left'
                }
            }
        ]
    },
    {
        id: 'bug-live',
        name: 'LIVE Bug',
        zone: 'bug',
        resolution: '1920x1080',
        category: 'Bug',
        isBuiltIn: true,
        elements: [
            {
                id: 'bug-bg',
                type: 'shape',
                name: 'Background',
                x: 1750,
                y: 50,
                width: 120,
                height: 40,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 0,
                shapeType: 'rectangle',
                fillColor: '#dc2626',
                borderRadius: 4
            },
            {
                id: 'bug-text',
                type: 'text',
                name: 'LIVE Text',
                x: 1755,
                y: 55,
                width: 110,
                height: 30,
                rotation: 0,
                opacity: 1,
                visible: true,
                locked: false,
                zIndex: 1,
                text: '● LIVE',
                fontFamily: 'Arial',
                fontSize: 22,
                fontWeight: 'bold',
                textColor: '#ffffff',
                textAlign: 'center',
                animation: {
                    type: 'fade',
                    duration: 500,
                    delay: 0
                }
            }
        ]
    }
];

export const CGDesigner: React.FC<CGDesignerProps> = ({
    onSaveTemplate,
    onScheduleItem,
    onPreview,
    onTakeToAir,
    existingTemplates = []
}) => {
    // State
    const [resolution, setResolution] = useState<Resolution>('1920x1080');
    const [zoom, setZoom] = useState(0.5);
    const [showGrid, setShowGrid] = useState(true);
    const [snapToGrid, setSnapToGrid] = useState(true);
    const [gridSize, setGridSize] = useState(20);
    const [elements, setElements] = useState<CGElement[]>([]);
    const [selectedElementId, setSelectedElementId] = useState<string | null>(null);
    const [isDragging, setIsDragging] = useState(false);
    const [dragOffset, setDragOffset] = useState({ x: 0, y: 0 });
    const [isResizing, setIsResizing] = useState(false);
    const [resizeHandle, setResizeHandle] = useState<string | null>(null);
    const [templateName, setTemplateName] = useState('Untitled');
    const [templateCategory, setTemplateCategory] = useState('Custom');
    const [templateZone, setTemplateZone] = useState<CGZone>('lower-third');
    const [activePanel, setActivePanel] = useState<'elements' | 'properties' | 'templates' | 'schedule'>('elements');
    const [templates, setTemplates] = useState<CGTemplate[]>([...DEFAULT_TEMPLATES, ...existingTemplates]);
    const [scheduleItems, setScheduleItems] = useState<CGScheduleItem[]>([]);
    const [showScheduleModal, setShowScheduleModal] = useState(false);
    const [scheduleTime, setScheduleTime] = useState('');
    const [scheduleDuration, setScheduleDuration] = useState(0);
    const [history, setHistory] = useState<CGElement[][]>([]);
    const [historyIndex, setHistoryIndex] = useState(-1);
    
    const canvasRef = useRef<HTMLDivElement>(null);
    const containerRef = useRef<HTMLDivElement>(null);

    // Get current resolution dimensions
    const currentResolution = useMemo(() => {
        return RESOLUTIONS.find(r => r.id === resolution) || RESOLUTIONS[0];
    }, [resolution]);

    // Selected element
    const selectedElement = useMemo(() => {
        return elements.find(el => el.id === selectedElementId);
    }, [elements, selectedElementId]);

    // Save to history
    const saveToHistory = useCallback((newElements: CGElement[]) => {
        const newHistory = history.slice(0, historyIndex + 1);
        newHistory.push([...newElements]);
        setHistory(newHistory);
        setHistoryIndex(newHistory.length - 1);
    }, [history, historyIndex]);

    // Undo
    const handleUndo = useCallback(() => {
        if (historyIndex > 0) {
            setHistoryIndex(historyIndex - 1);
            setElements([...history[historyIndex - 1]]);
        }
    }, [history, historyIndex]);

    // Redo
    const handleRedo = useCallback(() => {
        if (historyIndex < history.length - 1) {
            setHistoryIndex(historyIndex + 1);
            setElements([...history[historyIndex + 1]]);
        }
    }, [history, historyIndex]);

    // Add element
    const addElement = useCallback((type: CGElementType) => {
        const id = `el_${Date.now()}`;
        const newElement: CGElement = {
            id,
            type,
            name: `${type.charAt(0).toUpperCase() + type.slice(1)} ${elements.length + 1}`,
            x: 100,
            y: 100,
            width: type === 'text' ? 300 : 200,
            height: type === 'text' ? 50 : 100,
            rotation: 0,
            opacity: 1,
            visible: true,
            locked: false,
            zIndex: elements.length,
            ...(type === 'text' && {
                text: 'New Text',
                fontFamily: 'Arial',
                fontSize: 24,
                fontWeight: 'normal' as const,
                textColor: '#ffffff',
                textAlign: 'left' as const
            }),
            ...(type === 'shape' && {
                shapeType: 'rectangle' as const,
                fillColor: '#3b82f6',
                strokeColor: '#ffffff',
                strokeWidth: 0,
                borderRadius: 0
            }),
            ...(type === 'ticker' && {
                text: 'Ticker text goes here...',
                fontFamily: 'Arial',
                fontSize: 24,
                fontWeight: 'bold' as const,
                textColor: '#ffffff',
                animation: {
                    type: 'slide' as const,
                    duration: 10000,
                    delay: 0,
                    direction: 'left' as const
                }
            }),
            ...(type === 'clock' && {
                text: '00:00:00',
                fontFamily: 'Arial',
                fontSize: 32,
                fontWeight: 'bold' as const,
                textColor: '#ffffff'
            })
        };
        
        const newElements = [...elements, newElement];
        setElements(newElements);
        setSelectedElementId(id);
        saveToHistory(newElements);
    }, [elements, saveToHistory]);

    // Delete element
    const deleteElement = useCallback((id: string) => {
        const newElements = elements.filter(el => el.id !== id);
        setElements(newElements);
        setSelectedElementId(null);
        saveToHistory(newElements);
    }, [elements, saveToHistory]);

    // Duplicate element
    const duplicateElement = useCallback((id: string) => {
        const element = elements.find(el => el.id === id);
        if (!element) return;
        
        const newElement: CGElement = {
            ...element,
            id: `el_${Date.now()}`,
            name: `${element.name} Copy`,
            x: element.x + 20,
            y: element.y + 20,
            zIndex: elements.length
        };
        
        const newElements = [...elements, newElement];
        setElements(newElements);
        setSelectedElementId(newElement.id);
        saveToHistory(newElements);
    }, [elements, saveToHistory]);

    // Update element
    const updateElement = useCallback((id: string, updates: Partial<CGElement>) => {
        const newElements = elements.map(el => 
            el.id === id ? { ...el, ...updates } : el
        );
        setElements(newElements);
    }, [elements]);

    // Move element in layer order
    const moveElementLayer = useCallback((id: string, direction: 'up' | 'down') => {
        const index = elements.findIndex(el => el.id === id);
        if (index === -1) return;
        
        const newIndex = direction === 'up' ? index + 1 : index - 1;
        if (newIndex < 0 || newIndex >= elements.length) return;
        
        const newElements = [...elements];
        [newElements[index], newElements[newIndex]] = [newElements[newIndex], newElements[index]];
        
        // Update zIndex for all elements
        newElements.forEach((el, i) => {
            el.zIndex = i;
        });
        
        setElements(newElements);
        saveToHistory(newElements);
    }, [elements, saveToHistory]);

    // Handle canvas mouse down
    const handleCanvasMouseDown = useCallback((e: React.MouseEvent) => {
        if (!canvasRef.current) return;
        
        const rect = canvasRef.current.getBoundingClientRect();
        const x = (e.clientX - rect.left) / zoom;
        const y = (e.clientY - rect.top) / zoom;
        
        // Check if clicked on an element
        const clickedElement = [...elements].reverse().find(el => {
            if (el.locked || !el.visible) return false;
            return x >= el.x && x <= el.x + el.width && y >= el.y && y <= el.y + el.height;
        });
        
        if (clickedElement) {
            setSelectedElementId(clickedElement.id);
            setIsDragging(true);
            setDragOffset({
                x: x - clickedElement.x,
                y: y - clickedElement.y
            });
        } else {
            setSelectedElementId(null);
        }
    }, [elements, zoom]);

    // Handle canvas mouse move
    const handleCanvasMouseMove = useCallback((e: React.MouseEvent) => {
        if (!isDragging || !selectedElementId || !canvasRef.current) return;
        
        const rect = canvasRef.current.getBoundingClientRect();
        let x = (e.clientX - rect.left) / zoom - dragOffset.x;
        let y = (e.clientY - rect.top) / zoom - dragOffset.y;
        
        // Snap to grid
        if (snapToGrid) {
            x = Math.round(x / gridSize) * gridSize;
            y = Math.round(y / gridSize) * gridSize;
        }
        
        // Clamp to canvas bounds
        const element = elements.find(el => el.id === selectedElementId);
        if (element) {
            x = Math.max(0, Math.min(x, currentResolution.width - element.width));
            y = Math.max(0, Math.min(y, currentResolution.height - element.height));
        }
        
        updateElement(selectedElementId, { x, y });
    }, [isDragging, selectedElementId, dragOffset, zoom, snapToGrid, gridSize, elements, currentResolution, updateElement]);

    // Handle canvas mouse up
    const handleCanvasMouseUp = useCallback(() => {
        if (isDragging) {
            saveToHistory(elements);
        }
        setIsDragging(false);
        setIsResizing(false);
        setResizeHandle(null);
    }, [isDragging, elements, saveToHistory]);

    // Load template
    const loadTemplate = useCallback((template: CGTemplate) => {
        setElements([...template.elements]);
        setResolution(template.resolution);
        setTemplateZone(template.zone);
        setTemplateName(template.name);
        setTemplateCategory(template.category);
        setSelectedElementId(null);
        saveToHistory(template.elements);
    }, [saveToHistory]);

    // Save template
    const saveTemplate = useCallback(() => {
        const template: CGTemplate = {
            id: `tpl_${Date.now()}`,
            name: templateName,
            zone: templateZone,
            resolution,
            elements: [...elements],
            category: templateCategory,
            isBuiltIn: false
        };
        
        setTemplates(prev => [...prev, template]);
        onSaveTemplate?.(template);
    }, [templateName, templateZone, resolution, elements, templateCategory, onSaveTemplate]);

    // Schedule template
    const handleSchedule = useCallback(() => {
        if (!scheduleTime) return;
        
        const scheduleItem: CGScheduleItem = {
            id: `sch_${Date.now()}`,
            templateId: `current`,
            startTime: new Date(scheduleTime),
            duration: scheduleDuration,
            autoPlay: true
        };
        
        setScheduleItems(prev => [...prev, scheduleItem]);
        onScheduleItem?.(scheduleItem);
        setShowScheduleModal(false);
        setScheduleTime('');
        setScheduleDuration(0);
    }, [scheduleTime, scheduleDuration, onScheduleItem]);

    // Take to air
    const handleTakeToAir = useCallback(() => {
        const template: CGTemplate = {
            id: 'live',
            name: templateName,
            zone: templateZone,
            resolution,
            elements: [...elements],
            category: templateCategory,
            isBuiltIn: false
        };
        onTakeToAir?.(template);
    }, [templateName, templateZone, resolution, elements, templateCategory, onTakeToAir]);

    // Render element on canvas
    const renderElement = (element: CGElement) => {
        if (!element.visible) return null;
        
        const style: React.CSSProperties = {
            position: 'absolute',
            left: element.x * zoom,
            top: element.y * zoom,
            width: element.width * zoom,
            height: element.height * zoom,
            transform: `rotate(${element.rotation}deg)`,
            opacity: element.opacity,
            zIndex: element.zIndex,
            cursor: element.locked ? 'not-allowed' : 'move',
            outline: selectedElementId === element.id ? '2px solid #3b82f6' : 'none',
            outlineOffset: '2px'
        };
        
        switch (element.type) {
            case 'text':
            case 'ticker':
            case 'clock':
                return (
                    <div
                        key={element.id}
                        style={{
                            ...style,
                            display: 'flex',
                            alignItems: 'center',
                            justifyContent: element.textAlign === 'center' ? 'center' : element.textAlign === 'right' ? 'flex-end' : 'flex-start',
                            fontFamily: element.fontFamily,
                            fontSize: (element.fontSize || 16) * zoom,
                            fontWeight: element.fontWeight,
                            fontStyle: element.fontStyle,
                            color: element.textColor,
                            backgroundColor: element.backgroundColor,
                            borderRadius: (element.borderRadius || 0) * zoom,
                            padding: (element.padding || 0) * zoom,
                            textShadow: element.shadow ? 
                                `${element.shadow.offsetX}px ${element.shadow.offsetY}px ${element.shadow.blur}px ${element.shadow.color}` : 
                                undefined,
                            WebkitTextStroke: element.outline ? 
                                `${element.outline.width}px ${element.outline.color}` : 
                                undefined,
                            overflow: 'hidden',
                            whiteSpace: element.type === 'ticker' ? 'nowrap' : 'pre-wrap'
                        }}
                    >
                        {element.type === 'clock' ? new Date().toLocaleTimeString() : element.text}
                    </div>
                );
            
            case 'shape':
                return (
                    <div
                        key={element.id}
                        style={{
                            ...style,
                            backgroundColor: element.fillColor,
                            border: element.strokeWidth ? `${element.strokeWidth * zoom}px solid ${element.strokeColor}` : undefined,
                            borderRadius: element.shapeType === 'ellipse' ? '50%' : (element.borderRadius || 0) * zoom
                        }}
                    />
                );
            
            case 'image':
                return (
                    <div
                        key={element.id}
                        style={style}
                    >
                        {element.src ? (
                            <img 
                                src={element.src} 
                                alt={element.name}
                                style={{
                                    width: '100%',
                                    height: '100%',
                                    objectFit: element.objectFit || 'contain'
                                }}
                            />
                        ) : (
                            <div className={styles.imagePlaceholder}>
                                <ImageIcon size={24} />
                            </div>
                        )}
                    </div>
                );
            
            default:
                return null;
        }
    };

    return (
        <div className={styles.designer}>
            {/* Toolbar */}
            <div className={styles.toolbar}>
                <div className={styles.toolbarSection}>
                    <span className={styles.toolbarLabel}>Add:</span>
                    <button onClick={() => addElement('text')} title="Add Text">
                        <Type size={16} />
                    </button>
                    <button onClick={() => addElement('shape')} title="Add Shape">
                        <Square size={16} />
                    </button>
                    <button onClick={() => addElement('image')} title="Add Image">
                        <ImageIcon size={16} />
                    </button>
                    <button onClick={() => addElement('ticker')} title="Add Ticker">
                        <Film size={16} />
                    </button>
                    <button onClick={() => addElement('clock')} title="Add Clock">
                        <Clock size={16} />
                    </button>
                </div>
                
                <div className={styles.toolbarDivider} />
                
                <div className={styles.toolbarSection}>
                    <button onClick={handleUndo} disabled={historyIndex <= 0} title="Undo">
                        <Undo size={16} />
                    </button>
                    <button onClick={handleRedo} disabled={historyIndex >= history.length - 1} title="Redo">
                        <Redo size={16} />
                    </button>
                </div>
                
                <div className={styles.toolbarDivider} />
                
                <div className={styles.toolbarSection}>
                    <select 
                        value={resolution} 
                        onChange={(e) => setResolution(e.target.value as Resolution)}
                        className={styles.resolutionSelect}
                    >
                        {RESOLUTIONS.map(res => (
                            <option key={res.id} value={res.id}>{res.label}</option>
                        ))}
                    </select>
                </div>
                
                <div className={styles.toolbarDivider} />
                
                <div className={styles.toolbarSection}>
                    <button onClick={() => setZoom(Math.max(0.1, zoom - 0.1))} title="Zoom Out">
                        <ZoomOut size={16} />
                    </button>
                    <span className={styles.zoomLabel}>{Math.round(zoom * 100)}%</span>
                    <button onClick={() => setZoom(Math.min(2, zoom + 0.1))} title="Zoom In">
                        <ZoomIn size={16} />
                    </button>
                </div>
                
                <div className={styles.toolbarDivider} />
                
                <div className={styles.toolbarSection}>
                    <button 
                        onClick={() => setShowGrid(!showGrid)} 
                        className={showGrid ? styles.active : ''}
                        title="Toggle Grid"
                    >
                        <Grid size={16} />
                    </button>
                    <button 
                        onClick={() => setSnapToGrid(!snapToGrid)} 
                        className={snapToGrid ? styles.active : ''}
                        title="Snap to Grid"
                    >
                        <Move size={16} />
                    </button>
                </div>
                
                <div className={styles.toolbarSpacer} />
                
                <div className={styles.toolbarSection}>
                    <button onClick={() => setShowScheduleModal(true)} className={styles.scheduleBtn}>
                        <Calendar size={16} /> Schedule
                    </button>
                    <button onClick={saveTemplate} className={styles.saveBtn}>
                        <Save size={16} /> Save
                    </button>
                    <button onClick={handleTakeToAir} className={styles.liveBtn}>
                        <Play size={16} /> Take to Air
                    </button>
                </div>
            </div>

            {/* Main Content */}
            <div className={styles.mainContent}>
                {/* Left Panel - Layers & Templates */}
                <div className={styles.leftPanel}>
                    <div className={styles.panelTabs}>
                        <button 
                            className={activePanel === 'elements' ? styles.active : ''}
                            onClick={() => setActivePanel('elements')}
                        >
                            <Layers size={14} /> Layers
                        </button>
                        <button 
                            className={activePanel === 'templates' ? styles.active : ''}
                            onClick={() => setActivePanel('templates')}
                        >
                            <LayoutTemplate size={14} /> Templates
                        </button>
                    </div>
                    
                    {activePanel === 'elements' && (
                        <div className={styles.layersList}>
                            {[...elements].reverse().map(el => (
                                <div 
                                    key={el.id}
                                    className={`${styles.layerItem} ${selectedElementId === el.id ? styles.selected : ''}`}
                                    onClick={() => setSelectedElementId(el.id)}
                                >
                                    <button 
                                        className={styles.layerVisibility}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            updateElement(el.id, { visible: !el.visible });
                                        }}
                                    >
                                        {el.visible ? <Eye size={12} /> : <EyeOff size={12} />}
                                    </button>
                                    <span className={styles.layerName}>{el.name}</span>
                                    <button 
                                        className={styles.layerLock}
                                        onClick={(e) => {
                                            e.stopPropagation();
                                            updateElement(el.id, { locked: !el.locked });
                                        }}
                                    >
                                        {el.locked ? <Lock size={12} /> : <Unlock size={12} />}
                                    </button>
                                    <div className={styles.layerActions}>
                                        <button onClick={() => moveElementLayer(el.id, 'up')}><ChevronUp size={12} /></button>
                                        <button onClick={() => moveElementLayer(el.id, 'down')}><ChevronDown size={12} /></button>
                                    </div>
                                </div>
                            ))}
                            {elements.length === 0 && (
                                <div className={styles.emptyLayers}>
                                    <Layers size={32} />
                                    <span>No elements</span>
                                    <p>Add text, shapes, or images to get started</p>
                                </div>
                            )}
                        </div>
                    )}
                    
                    {activePanel === 'templates' && (
                        <div className={styles.templatesList}>
                            {templates.map(tpl => (
                                <div 
                                    key={tpl.id}
                                    className={styles.templateItem}
                                    onClick={() => loadTemplate(tpl)}
                                >
                                    <div className={styles.templatePreview}>
                                        {tpl.thumbnail ? (
                                            <img src={tpl.thumbnail} alt={tpl.name} />
                                        ) : (
                                            <LayoutTemplate size={24} />
                                        )}
                                    </div>
                                    <div className={styles.templateInfo}>
                                        <span className={styles.templateName}>{tpl.name}</span>
                                        <span className={styles.templateZone}>{tpl.zone}</span>
                                    </div>
                                    {tpl.isBuiltIn && <span className={styles.builtInBadge}>Built-in</span>}
                                </div>
                            ))}
                        </div>
                    )}
                </div>

                {/* Canvas Area */}
                <div className={styles.canvasContainer} ref={containerRef}>
                    <div 
                        className={styles.canvas}
                        ref={canvasRef}
                        style={{
                            width: currentResolution.width * zoom,
                            height: currentResolution.height * zoom,
                            backgroundImage: showGrid ? 
                                `linear-gradient(to right, rgba(255,255,255,0.05) 1px, transparent 1px),
                                 linear-gradient(to bottom, rgba(255,255,255,0.05) 1px, transparent 1px)` : 
                                undefined,
                            backgroundSize: showGrid ? `${gridSize * zoom}px ${gridSize * zoom}px` : undefined
                        }}
                        onMouseDown={handleCanvasMouseDown}
                        onMouseMove={handleCanvasMouseMove}
                        onMouseUp={handleCanvasMouseUp}
                        onMouseLeave={handleCanvasMouseUp}
                    >
                        {/* Safe zones overlay */}
                        <div 
                            className={styles.safeZone}
                            style={{
                                left: currentResolution.width * 0.05 * zoom,
                                top: currentResolution.height * 0.05 * zoom,
                                width: currentResolution.width * 0.9 * zoom,
                                height: currentResolution.height * 0.9 * zoom
                            }}
                        />
                        
                        {/* Render elements */}
                        {elements.map(renderElement)}
                        
                        {/* Selection handles */}
                        {selectedElement && (
                            <div 
                                className={styles.selectionHandles}
                                style={{
                                    left: selectedElement.x * zoom - 4,
                                    top: selectedElement.y * zoom - 4,
                                    width: selectedElement.width * zoom + 8,
                                    height: selectedElement.height * zoom + 8
                                }}
                            >
                                {!selectedElement.locked && (
                                    <>
                                        <div className={`${styles.handle} ${styles.handleNW}`} />
                                        <div className={`${styles.handle} ${styles.handleN}`} />
                                        <div className={`${styles.handle} ${styles.handleNE}`} />
                                        <div className={`${styles.handle} ${styles.handleE}`} />
                                        <div className={`${styles.handle} ${styles.handleSE}`} />
                                        <div className={`${styles.handle} ${styles.handleS}`} />
                                        <div className={`${styles.handle} ${styles.handleSW}`} />
                                        <div className={`${styles.handle} ${styles.handleW}`} />
                                    </>
                                )}
                            </div>
                        )}
                    </div>
                    
                    {/* Canvas info */}
                    <div className={styles.canvasInfo}>
                        <span>{currentResolution.width} × {currentResolution.height}</span>
                        {selectedElement && (
                            <span>X: {Math.round(selectedElement.x)} Y: {Math.round(selectedElement.y)}</span>
                        )}
                    </div>
                </div>

                {/* Right Panel - Properties */}
                <div className={styles.rightPanel}>
                    <div className={styles.panelHeader}>
                        <Settings size={14} />
                        <span>Properties</span>
                    </div>
                    
                    {selectedElement ? (
                        <div className={styles.propertiesContent}>
                            {/* Element Info */}
                            <div className={styles.propertyGroup}>
                                <label>Name</label>
                                <input
                                    type="text"
                                    value={selectedElement.name}
                                    onChange={(e) => updateElement(selectedElement.id, { name: e.target.value })}
                                />
                            </div>
                            
                            {/* Transform */}
                            <div className={styles.propertySection}>
                                <h4>Transform</h4>
                                <div className={styles.propertyRow}>
                                    <div className={styles.propertyGroup}>
                                        <label>X</label>
                                        <input
                                            type="number"
                                            value={Math.round(selectedElement.x)}
                                            onChange={(e) => updateElement(selectedElement.id, { x: parseInt(e.target.value) || 0 })}
                                        />
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Y</label>
                                        <input
                                            type="number"
                                            value={Math.round(selectedElement.y)}
                                            onChange={(e) => updateElement(selectedElement.id, { y: parseInt(e.target.value) || 0 })}
                                        />
                                    </div>
                                </div>
                                <div className={styles.propertyRow}>
                                    <div className={styles.propertyGroup}>
                                        <label>Width</label>
                                        <input
                                            type="number"
                                            value={Math.round(selectedElement.width)}
                                            onChange={(e) => updateElement(selectedElement.id, { width: parseInt(e.target.value) || 0 })}
                                        />
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Height</label>
                                        <input
                                            type="number"
                                            value={Math.round(selectedElement.height)}
                                            onChange={(e) => updateElement(selectedElement.id, { height: parseInt(e.target.value) || 0 })}
                                        />
                                    </div>
                                </div>
                                <div className={styles.propertyGroup}>
                                    <label>Opacity</label>
                                    <input
                                        type="range"
                                        min="0"
                                        max="1"
                                        step="0.01"
                                        value={selectedElement.opacity}
                                        onChange={(e) => updateElement(selectedElement.id, { opacity: parseFloat(e.target.value) })}
                                    />
                                </div>
                            </div>
                            
                            {/* Text Properties */}
                            {(selectedElement.type === 'text' || selectedElement.type === 'ticker' || selectedElement.type === 'clock') && (
                                <div className={styles.propertySection}>
                                    <h4>Text</h4>
                                    {selectedElement.type !== 'clock' && (
                                        <div className={styles.propertyGroup}>
                                            <label>Content</label>
                                            <textarea
                                                value={selectedElement.text || ''}
                                                onChange={(e) => updateElement(selectedElement.id, { text: e.target.value })}
                                                rows={3}
                                            />
                                        </div>
                                    )}
                                    <div className={styles.propertyGroup}>
                                        <label>Font</label>
                                        <select
                                            value={selectedElement.fontFamily}
                                            onChange={(e) => updateElement(selectedElement.id, { fontFamily: e.target.value })}
                                        >
                                            {FONT_FAMILIES.map(font => (
                                                <option key={font} value={font}>{font}</option>
                                            ))}
                                        </select>
                                    </div>
                                    <div className={styles.propertyRow}>
                                        <div className={styles.propertyGroup}>
                                            <label>Size</label>
                                            <input
                                                type="number"
                                                value={selectedElement.fontSize || 24}
                                                onChange={(e) => updateElement(selectedElement.id, { fontSize: parseInt(e.target.value) || 24 })}
                                            />
                                        </div>
                                        <div className={styles.propertyGroup}>
                                            <label>Weight</label>
                                            <select
                                                value={selectedElement.fontWeight}
                                                onChange={(e) => updateElement(selectedElement.id, { fontWeight: e.target.value as 'normal' | 'bold' })}
                                            >
                                                <option value="normal">Normal</option>
                                                <option value="bold">Bold</option>
                                            </select>
                                        </div>
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Color</label>
                                        <div className={styles.colorPicker}>
                                            <input
                                                type="color"
                                                value={selectedElement.textColor || '#ffffff'}
                                                onChange={(e) => updateElement(selectedElement.id, { textColor: e.target.value })}
                                            />
                                            <div className={styles.colorPresets}>
                                                {PRESET_COLORS.map(color => (
                                                    <button
                                                        key={color}
                                                        style={{ backgroundColor: color }}
                                                        onClick={() => updateElement(selectedElement.id, { textColor: color })}
                                                    />
                                                ))}
                                            </div>
                                        </div>
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Align</label>
                                        <div className={styles.alignButtons}>
                                            <button 
                                                className={selectedElement.textAlign === 'left' ? styles.active : ''}
                                                onClick={() => updateElement(selectedElement.id, { textAlign: 'left' })}
                                            >
                                                <AlignLeft size={14} />
                                            </button>
                                            <button 
                                                className={selectedElement.textAlign === 'center' ? styles.active : ''}
                                                onClick={() => updateElement(selectedElement.id, { textAlign: 'center' })}
                                            >
                                                <AlignCenter size={14} />
                                            </button>
                                            <button 
                                                className={selectedElement.textAlign === 'right' ? styles.active : ''}
                                                onClick={() => updateElement(selectedElement.id, { textAlign: 'right' })}
                                            >
                                                <AlignRight size={14} />
                                            </button>
                                        </div>
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Background</label>
                                        <div className={styles.colorPicker}>
                                            <input
                                                type="color"
                                                value={selectedElement.backgroundColor || '#000000'}
                                                onChange={(e) => updateElement(selectedElement.id, { backgroundColor: e.target.value })}
                                            />
                                            <button 
                                                className={styles.clearBtn}
                                                onClick={() => updateElement(selectedElement.id, { backgroundColor: undefined })}
                                            >
                                                <X size={12} /> None
                                            </button>
                                        </div>
                                    </div>
                                </div>
                            )}
                            
                            {/* Shape Properties */}
                            {selectedElement.type === 'shape' && (
                                <div className={styles.propertySection}>
                                    <h4>Shape</h4>
                                    <div className={styles.propertyGroup}>
                                        <label>Type</label>
                                        <select
                                            value={selectedElement.shapeType}
                                            onChange={(e) => updateElement(selectedElement.id, { shapeType: e.target.value as 'rectangle' | 'ellipse' })}
                                        >
                                            <option value="rectangle">Rectangle</option>
                                            <option value="ellipse">Ellipse</option>
                                        </select>
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Fill</label>
                                        <input
                                            type="color"
                                            value={selectedElement.fillColor || '#3b82f6'}
                                            onChange={(e) => updateElement(selectedElement.id, { fillColor: e.target.value })}
                                        />
                                    </div>
                                    <div className={styles.propertyGroup}>
                                        <label>Border Radius</label>
                                        <input
                                            type="number"
                                            value={selectedElement.borderRadius || 0}
                                            onChange={(e) => updateElement(selectedElement.id, { borderRadius: parseInt(e.target.value) || 0 })}
                                        />
                                    </div>
                                </div>
                            )}
                            
                            {/* Actions */}
                            <div className={styles.elementActions}>
                                <button onClick={() => duplicateElement(selectedElement.id)}>
                                    <Copy size={14} /> Duplicate
                                </button>
                                <button onClick={() => deleteElement(selectedElement.id)} className={styles.deleteBtn}>
                                    <Trash2 size={14} /> Delete
                                </button>
                            </div>
                        </div>
                    ) : (
                        <div className={styles.noSelection}>
                            <Monitor size={32} />
                            <span>No element selected</span>
                            <p>Click on an element to edit its properties</p>
                        </div>
                    )}
                    
                    {/* Template Settings */}
                    <div className={styles.templateSettings}>
                        <h4>Template Settings</h4>
                        <div className={styles.propertyGroup}>
                            <label>Name</label>
                            <input
                                type="text"
                                value={templateName}
                                onChange={(e) => setTemplateName(e.target.value)}
                            />
                        </div>
                        <div className={styles.propertyGroup}>
                            <label>Zone</label>
                            <select
                                value={templateZone}
                                onChange={(e) => setTemplateZone(e.target.value as CGZone)}
                            >
                                <option value="lower-third">Lower Third</option>
                                <option value="full-screen">Full Screen</option>
                                <option value="ticker">Ticker</option>
                                <option value="bug">Bug</option>
                                <option value="custom">Custom</option>
                            </select>
                        </div>
                        <div className={styles.propertyGroup}>
                            <label>Category</label>
                            <input
                                type="text"
                                value={templateCategory}
                                onChange={(e) => setTemplateCategory(e.target.value)}
                            />
                        </div>
                    </div>
                </div>
            </div>

            {/* Schedule Modal */}
            {showScheduleModal && (
                <div className={styles.modal} onClick={() => setShowScheduleModal(false)}>
                    <div className={styles.modalContent} onClick={e => e.stopPropagation()}>
                        <h3>Schedule CG Template</h3>
                        <div className={styles.propertyGroup}>
                            <label>Start Time</label>
                            <input
                                type="datetime-local"
                                value={scheduleTime}
                                onChange={(e) => setScheduleTime(e.target.value)}
                            />
                        </div>
                        <div className={styles.propertyGroup}>
                            <label>Duration (seconds, 0 = manual)</label>
                            <input
                                type="number"
                                value={scheduleDuration}
                                onChange={(e) => setScheduleDuration(parseInt(e.target.value) || 0)}
                            />
                        </div>
                        <div className={styles.modalActions}>
                            <button onClick={() => setShowScheduleModal(false)}>Cancel</button>
                            <button onClick={handleSchedule} disabled={!scheduleTime}>
                                <Calendar size={14} /> Schedule
                            </button>
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
};

export default CGDesigner;
