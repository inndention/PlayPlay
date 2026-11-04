import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import styles from './EnhancedCGPanel.module.css';
import {
    Layers, Plus, Edit2, Trash2, Eye, EyeOff, Copy, ExternalLink, Play, Pause,
    Save, X, Settings, Palette, Zap, RefreshCw, Download, Upload, Globe, Link2,
    ChevronDown, ChevronUp, Maximize, Minimize, Monitor, Video, Image as ImageIcon,
    Type, Circle, Square, Triangle, Sparkles, Filter, Search, Grid, List
} from 'lucide-react';
import { CGElement, CGPreset, CGType, CGAnimationIn, CGAnimationOut, CGLoop } from '../lib/types';
import { loadCGElements, saveCGElements, loadCGPresets, saveCGPresets } from '../lib/storage';
import { generateId } from '../lib/utils';
import { CGOverlay } from './CGOverlay';
import { useToast } from './Toast';

interface EnhancedCGPanelProps {
    onElementsChange?: (elements: CGElement[]) => void;
    onPreview?: (element: CGElement) => void;
    previewMode?: boolean;
}

export function EnhancedCGPanel({ onElementsChange, onPreview, previewMode = false }: EnhancedCGPanelProps) {
    const { showToast } = useToast();
    const [elements, setElements] = useState<CGElement[]>([]);
    const [presets, setPresets] = useState<CGPreset[]>([]);
    const [selectedElement, setSelectedElement] = useState<CGElement | null>(null);
    const [isEditing, setIsEditing] = useState(false);
    const [searchQuery, setSearchQuery] = useState('');
    const [viewMode, setViewMode] = useState<'grid' | 'list'>('grid');
    const [showDeploymentUrls, setShowDeploymentUrls] = useState(false);
    const [previewElement, setPreviewElement] = useState<CGElement | null>(null);
    const [cgOutputUrl, setCgOutputUrl] = useState<string>('http://localhost:8080/cg-output');
    const previewRef = useRef<HTMLDivElement>(null);

    // Resolve CG output URL for OBS Browser Source (Electron: from getAllUrls)
    useEffect(() => {
        const api = (window as any).electronAPI;
        if (api?.getAllUrls) {
            api.getAllUrls().then((u: any) => {
                const url = u?.urls?.cgOutput;
                if (url) setCgOutputUrl(url);
            }).catch(() => {});
        } else if (typeof window !== 'undefined' && window.location?.origin) {
            setCgOutputUrl(`${window.location.origin.replace(/^https?:/, 'http:')}/#/cg-output`);
        }
    }, []);

    // Load elements and presets
    useEffect(() => {
        setElements(loadCGElements());
        setPresets(loadCGPresets());
    }, []);

    // Generate deployment URLs for CG layers
    const deploymentUrls = useMemo(() => {
        const baseUrl = window.location.origin;
        return elements.map(element => ({
            elementId: element.id,
            elementName: element.name,
            url: `${baseUrl}/cg/layer/${element.id}`,
            embedUrl: `${baseUrl}/cg/embed/${element.id}`,
            apiUrl: `${baseUrl}/api/cg/${element.id}`,
        }));
    }, [elements]);

    // Filter elements by search
    const filteredElements = useMemo(() => {
        if (!searchQuery) return elements;
        const query = searchQuery.toLowerCase();
        return elements.filter(el =>
            el.name.toLowerCase().includes(query) ||
            el.title.toLowerCase().includes(query) ||
            el.type.toLowerCase().includes(query)
        );
    }, [elements, searchQuery]);

    // Create new element
    const handleCreateElement = useCallback(() => {
        const newElement: CGElement = {
            id: generateId(),
            name: 'New CG Element',
            type: 'lower_third',
            title: 'Title',
            subtitle: 'Subtitle',
            colors: {
                primary: '#1a1a1a',
                secondary: '#333333',
                text: '#ffffff',
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowColor: '#000000',
                shadowBlur: 10,
                glow: false,
                borderRadius: 8,
                opacity: 0.95,
            },
            position: {
                x: 5,
                y: 75,
                width: 40,
                height: 15,
                anchor: 'bottom-left',
            },
            fontSize: 32,
            fontFamily: 'Arial',
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 500,
            loop: 'none',
            isVisible: false,
            isPreset: false,
        };
        setElements(prev => [...prev, newElement]);
        setSelectedElement(newElement);
        setIsEditing(true);
    }, []);

    // Update element
    const handleUpdateElement = useCallback((updates: Partial<CGElement>) => {
        if (!selectedElement) return;
        const updated = { ...selectedElement, ...updates };
        setElements(prev => prev.map(el => el.id === selectedElement.id ? updated : el));
        setSelectedElement(updated);
        onElementsChange?.(elements.map(el => el.id === selectedElement.id ? updated : el));
    }, [selectedElement, elements, onElementsChange]);

    // Delete element
    const handleDeleteElement = useCallback((id: string) => {
        const element = elements.find(el => el.id === id);
        setElements(prev => prev.filter(el => el.id !== id));
        if (selectedElement?.id === id) {
            setSelectedElement(null);
            setIsEditing(false);
        }
        if (element) {
            showToast(`Deleted "${element.name}"`, 'info', 2000);
        }
    }, [selectedElement, elements, showToast]);

    // Toggle visibility
    const handleToggleVisibility = useCallback((id: string) => {
        setElements(prev => prev.map(el =>
            el.id === id ? { ...el, isVisible: !el.isVisible } : el
        ));
    }, []);

    // Copy element
    const handleCopyElement = useCallback((element: CGElement) => {
        const copied: CGElement = {
            ...element,
            id: generateId(),
            name: `${element.name} (Copy)`,
            isVisible: false,
        };
        setElements(prev => [...prev, copied]);
    }, []);

    // Save to presets
    const handleSavePreset = useCallback((element: CGElement) => {
        const { id, isVisible, ...elementWithoutId } = element;
        const preset: CGPreset = {
            id: generateId(),
            name: element.name,
            category: 'Custom',
            element: elementWithoutId,
        };
        setPresets(prev => [...prev, preset]);
        saveCGPresets([...presets, preset]);
        showToast(`Saved "${element.name}" as preset`, 'success', 2000);
    }, [presets, showToast]);

    // Preview element
    const handlePreview = useCallback((element: CGElement) => {
        setPreviewElement(element);
        onPreview?.(element);
    }, [onPreview]);

    // Save all elements
    const handleSave = useCallback(() => {
        saveCGElements(elements);
        saveCGPresets(presets);
        showToast(`Saved ${elements.length} CG element${elements.length !== 1 ? 's' : ''}`, 'success', 2000);
    }, [elements, presets, showToast]);

    // Copy deployment URL
    const handleCopyUrl = useCallback((url: string) => {
        navigator.clipboard.writeText(url);
        showToast('URL copied to clipboard', 'success', 1500);
    }, [showToast]);

    return (
        <div className={styles.panel}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Layers size={20} />
                    <h2>Character Graphics</h2>
                    <span className={styles.badge}>{elements.length}</span>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.iconBtn}
                        onClick={() => setShowDeploymentUrls(!showDeploymentUrls)}
                        title="Show Deployment URLs"
                    >
                        <Globe size={16} />
                    </button>
                    <button
                        className={styles.iconBtn}
                        onClick={() => setViewMode(viewMode === 'grid' ? 'list' : 'grid')}
                        title="Toggle View"
                    >
                        {viewMode === 'grid' ? <List size={16} /> : <Grid size={16} />}
                    </button>
                    <button className={styles.iconBtn} onClick={handleSave} title="Save All">
                        <Save size={16} />
                    </button>
                    <button className={styles.primaryBtn} onClick={handleCreateElement}>
                        <Plus size={16} />
                        New Element
                    </button>
                </div>
            </div>

            {/* Deployment URLs Panel */}
            {showDeploymentUrls && (
                <div className={styles.deploymentPanel}>
                    <div className={styles.deploymentHeader}>
                        <Globe size={16} />
                        <span>CG Layer Deployment URLs</span>
                        <button className={styles.closeBtn} onClick={() => setShowDeploymentUrls(false)}>
                            <X size={14} />
                        </button>
                    </div>
                    <div className={styles.deploymentList}>
                        {deploymentUrls.length === 0 ? (
                            <div className={styles.empty}>No CG elements to deploy</div>
                        ) : (
                            deploymentUrls.map(({ elementId, elementName, url, embedUrl, apiUrl }) => (
                                <div key={elementId} className={styles.deploymentItem}>
                                    <div className={styles.deploymentName}>{elementName}</div>
                                    <div className={styles.deploymentUrls}>
                                        <div className={styles.urlRow}>
                                            <label>Layer URL:</label>
                                            <code>{url}</code>
                                            <button onClick={() => handleCopyUrl(url)} title="Copy">
                                                <Copy size={12} />
                                            </button>
                                            <a href={url} target="_blank" rel="noopener noreferrer" title="Open">
                                                <ExternalLink size={12} />
                                            </a>
                                        </div>
                                        <div className={styles.urlRow}>
                                            <label>Embed URL:</label>
                                            <code>{embedUrl}</code>
                                            <button onClick={() => handleCopyUrl(embedUrl)} title="Copy">
                                                <Copy size={12} />
                                            </button>
                                        </div>
                                        <div className={styles.urlRow}>
                                            <label>API URL:</label>
                                            <code>{apiUrl}</code>
                                            <button onClick={() => handleCopyUrl(apiUrl)} title="Copy">
                                                <Copy size={12} />
                                            </button>
                                        </div>
                                    </div>
                                </div>
                            ))
                        )}
                    </div>
                </div>
            )}

            {/* Search and Filters */}
            <div className={styles.toolbar}>
                <div className={styles.search}>
                    <Search size={16} />
                    <input
                        type="text"
                        placeholder="Search elements..."
                        value={searchQuery}
                        onChange={(e) => setSearchQuery(e.target.value)}
                    />
                </div>
                <div className={styles.filters}>
                    <select>
                        <option>All Types</option>
                        <option>Lower Third</option>
                        <option>Ticker</option>
                        <option>Bug</option>
                        <option>Clock</option>
                    </select>
                </div>
            </div>

            {/* Main Content */}
            <div className={styles.content}>
                {/* Elements List/Grid */}
                <div className={styles.elementsSection}>
                    {viewMode === 'grid' ? (
                        <div className={styles.elementsGrid}>
                            {filteredElements.map(element => (
                                <div
                                    key={element.id}
                                    className={`${styles.elementCard} ${selectedElement?.id === element.id ? styles.selected : ''}`}
                                    onClick={() => {
                                        setSelectedElement(element);
                                        setIsEditing(false);
                                    }}
                                >
                                    <div className={styles.elementCardHeader}>
                                        <div className={styles.elementCardTitle}>
                                            <span className={styles.elementType}>{element.type}</span>
                                            {element.name}
                                        </div>
                                        <div className={styles.elementCardActions}>
                                            <button
                                                className={styles.cardBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleToggleVisibility(element.id);
                                                }}
                                                title={element.isVisible ? 'Hide' : 'Show'}
                                            >
                                                {element.isVisible ? <Eye size={14} /> : <EyeOff size={14} />}
                                            </button>
                                            <button
                                                className={styles.cardBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handlePreview(element);
                                                }}
                                                title="Preview"
                                            >
                                                <Play size={14} />
                                            </button>
                                            <button
                                                className={styles.cardBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleCopyElement(element);
                                                }}
                                                title="Copy"
                                            >
                                                <Copy size={14} />
                                            </button>
                                            <button
                                                className={styles.cardBtn}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleDeleteElement(element.id);
                                                }}
                                                title="Delete"
                                            >
                                                <Trash2 size={14} />
                                            </button>
                                        </div>
                                    </div>
                                    <div className={styles.elementCardPreview}>
                                        <CGOverlay elements={[element]} isPreview={true} />
                                    </div>
                                    <div className={styles.elementCardMeta}>
                                        <span>{element.title}</span>
                                        {element.isVisible && <span className={styles.liveBadge}>LIVE</span>}
                                    </div>
                                </div>
                            ))}
                        </div>
                    ) : (
                        <div className={styles.elementsList}>
                            {filteredElements.map(element => (
                                <div
                                    key={element.id}
                                    className={`${styles.elementListItem} ${selectedElement?.id === element.id ? styles.selected : ''}`}
                                    onClick={() => {
                                        setSelectedElement(element);
                                        setIsEditing(false);
                                    }}
                                >
                                    <div className={styles.listItemIcon}>
                                        {element.isVisible ? <Eye size={16} /> : <EyeOff size={16} />}
                                    </div>
                                    <div className={styles.listItemContent}>
                                        <div className={styles.listItemName}>{element.name}</div>
                                        <div className={styles.listItemMeta}>
                                            <span>{element.type}</span>
                                            <span>•</span>
                                            <span>{element.title}</span>
                                        </div>
                                    </div>
                                    <div className={styles.listItemActions}>
                                        <button
                                            className={styles.cardBtn}
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                handlePreview(element);
                                            }}
                                        >
                                            <Play size={14} />
                                        </button>
                                        <button
                                            className={styles.cardBtn}
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                handleToggleVisibility(element.id);
                                            }}
                                        >
                                            {element.isVisible ? <Eye size={14} /> : <EyeOff size={14} />}
                                        </button>
                                        <button
                                            className={styles.cardBtn}
                                            onClick={(e) => {
                                                e.stopPropagation();
                                                handleDeleteElement(element.id);
                                            }}
                                        >
                                            <Trash2 size={14} />
                                        </button>
                                    </div>
                                </div>
                            ))}
                        </div>
                    )}
                </div>

                {/* Properties Panel */}
                {selectedElement && (
                    <div className={styles.propertiesPanel}>
                        <div className={styles.propertiesHeader}>
                            <span>Properties</span>
                            <div className={styles.propertiesActions}>
                                <button
                                    className={styles.iconBtn}
                                    onClick={() => setIsEditing(!isEditing)}
                                    title={isEditing ? 'Cancel Edit' : 'Edit'}
                                >
                                    {isEditing ? <X size={14} /> : <Edit2 size={14} />}
                                </button>
                                <button
                                    className={styles.iconBtn}
                                    onClick={() => handleSavePreset(selectedElement)}
                                    title="Save as Preset"
                                >
                                    <Save size={14} />
                                </button>
                            </div>
                        </div>
                        {isEditing ? (
                            <ElementEditor
                                element={selectedElement}
                                onUpdate={handleUpdateElement}
                                onClose={() => setIsEditing(false)}
                            />
                        ) : (
                            <ElementViewer element={selectedElement} />
                        )}
                    </div>
                )}
            </div>

            {/* Preview Overlay */}
            {previewElement && (
                <div className={styles.previewOverlay} onClick={() => setPreviewElement(null)}>
                    <div className={styles.previewContainer} onClick={(e) => e.stopPropagation()}>
                        <div className={styles.previewHeader}>
                            <span>Preview: {previewElement.name} (matches OBS browser output 16:9)</span>
                            <button onClick={() => setPreviewElement(null)}>
                                <X size={16} />
                            </button>
                        </div>
                        <div className={styles.previewContent} ref={previewRef}>
                            <CGOverlay elements={[previewElement]} isPreview={true} />
                        </div>
                    </div>
                </div>
            )}
        </div>
    );
}

// Element Viewer Component
function ElementViewer({ element }: { element: CGElement }) {
    return (
        <div className={styles.viewer}>
            <div className={styles.viewerSection}>
                <label>Name</label>
                <div>{element.name}</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Type</label>
                <div>{element.type}</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Title</label>
                <div>{element.title}</div>
            </div>
            {element.subtitle && (
                <div className={styles.viewerSection}>
                    <label>Subtitle</label>
                    <div>{element.subtitle}</div>
                </div>
            )}
            <div className={styles.viewerSection}>
                <label>Position</label>
                <div>{element.position.anchor} ({element.position.x}%, {element.position.y}%)</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Size</label>
                <div>{element.position.width}% × {element.position.height}%</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Animation</label>
                <div>In: {element.animationIn}, Out: {element.animationOut}, Duration: {element.animationDuration}ms</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Loop</label>
                <div>{element.loop}</div>
            </div>
            <div className={styles.viewerSection}>
                <label>Visibility</label>
                <div>{element.isVisible ? 'Visible' : 'Hidden'}</div>
            </div>
        </div>
    );
}

// Element Editor Component
function ElementEditor({ element, onUpdate, onClose }: { element: CGElement, onUpdate: (updates: Partial<CGElement>) => void, onClose: () => void }) {
    const [formData, setFormData] = useState(element);

    useEffect(() => {
        setFormData(element);
    }, [element]);

    const handleChange = (field: keyof CGElement, value: any) => {
        const updated = { ...formData, [field]: value };
        setFormData(updated);
        onUpdate(updated);
    };

    const handleNestedChange = (path: string[], value: any) => {
        const updated = { ...formData };
        let current: any = updated;
        for (let i = 0; i < path.length - 1; i++) {
            current = current[path[i]];
        }
        current[path[path.length - 1]] = value;
        setFormData(updated);
        onUpdate(updated);
    };

    return (
        <div className={styles.editor}>
            <div className={styles.editorSection}>
                <label>Name</label>
                <input
                    type="text"
                    value={formData.name}
                    onChange={(e) => handleChange('name', e.target.value)}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Type</label>
                <select
                    value={formData.type}
                    onChange={(e) => handleChange('type', e.target.value as CGType)}
                >
                    <option value="lower_third">Lower Third</option>
                    <option value="ticker">Ticker</option>
                    <option value="bug">Bug</option>
                    <option value="clock">Clock</option>
                    <option value="weather">Weather</option>
                    <option value="custom">Custom</option>
                </select>
            </div>
            <div className={styles.editorSection}>
                <label>Title</label>
                <input
                    type="text"
                    value={formData.title}
                    onChange={(e) => handleChange('title', e.target.value)}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Subtitle</label>
                <input
                    type="text"
                    value={formData.subtitle || ''}
                    onChange={(e) => handleChange('subtitle', e.target.value)}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Font Size</label>
                <input
                    type="number"
                    value={formData.fontSize || 32}
                    onChange={(e) => handleChange('fontSize', parseInt(e.target.value))}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Primary Color</label>
                <input
                    type="color"
                    value={formData.colors.primary}
                    onChange={(e) => handleNestedChange(['colors', 'primary'], e.target.value)}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Text Color</label>
                <input
                    type="color"
                    value={formData.colors.text}
                    onChange={(e) => handleNestedChange(['colors', 'text'], e.target.value)}
                />
            </div>
            <div className={styles.editorSection}>
                <label>
                    <input
                        type="checkbox"
                        checked={formData.colors.gradient}
                        onChange={(e) => handleNestedChange(['colors', 'gradient'], e.target.checked)}
                    />
                    Enable Gradient
                </label>
            </div>
            {formData.colors.gradient && (
                <>
                    <div className={styles.editorSection}>
                        <label>Secondary Color</label>
                        <input
                            type="color"
                            value={formData.colors.secondary}
                            onChange={(e) => handleNestedChange(['colors', 'secondary'], e.target.value)}
                        />
                    </div>
                    <div className={styles.editorSection}>
                        <label>Gradient Direction</label>
                        <select
                            value={formData.colors.gradientDirection || 'horizontal'}
                            onChange={(e) => handleNestedChange(['colors', 'gradientDirection'], e.target.value)}
                        >
                            <option value="horizontal">Horizontal</option>
                            <option value="vertical">Vertical</option>
                            <option value="diagonal">Diagonal</option>
                        </select>
                    </div>
                </>
            )}
            <div className={styles.editorSection}>
                <label>
                    <input
                        type="checkbox"
                        checked={formData.effects.shadow}
                        onChange={(e) => handleNestedChange(['effects', 'shadow'], e.target.checked)}
                    />
                    Shadow
                </label>
            </div>
            <div className={styles.editorSection}>
                <label>
                    <input
                        type="checkbox"
                        checked={formData.effects.glow}
                        onChange={(e) => handleNestedChange(['effects', 'glow'], e.target.checked)}
                    />
                    Glow
                </label>
            </div>
            <div className={styles.editorSection}>
                <label>Border Radius</label>
                <input
                    type="number"
                    value={formData.effects.borderRadius || 0}
                    onChange={(e) => handleNestedChange(['effects', 'borderRadius'], parseInt(e.target.value))}
                />
            </div>
            <div className={styles.editorSection}>
                <label>Opacity</label>
                <input
                    type="range"
                    min="0"
                    max="1"
                    step="0.01"
                    value={formData.effects.opacity || 1}
                    onChange={(e) => handleNestedChange(['effects', 'opacity'], parseFloat(e.target.value))}
                />
                <span>{(formData.effects.opacity || 1) * 100}%</span>
            </div>
            <div className={styles.editorSection}>
                <label>Animation In</label>
                <select
                    value={formData.animationIn}
                    onChange={(e) => handleChange('animationIn', e.target.value as CGAnimationIn)}
                >
                    <option value="fade">Fade</option>
                    <option value="slide_up">Slide Up</option>
                    <option value="slide_down">Slide Down</option>
                    <option value="slide_left">Slide Left</option>
                    <option value="slide_right">Slide Right</option>
                    <option value="zoom">Zoom</option>
                    <option value="blur">Blur</option>
                </select>
            </div>
            <div className={styles.editorSection}>
                <label>Animation Out</label>
                <select
                    value={formData.animationOut}
                    onChange={(e) => handleChange('animationOut', e.target.value as CGAnimationOut)}
                >
                    <option value="fade">Fade</option>
                    <option value="slide_up">Slide Up</option>
                    <option value="slide_down">Slide Down</option>
                    <option value="slide_left">Slide Left</option>
                    <option value="slide_right">Slide Right</option>
                    <option value="zoom">Zoom</option>
                    <option value="blur">Blur</option>
                </select>
            </div>
            <div className={styles.editorSection}>
                <label>Loop Animation</label>
                <select
                    value={formData.loop}
                    onChange={(e) => handleChange('loop', e.target.value as CGLoop)}
                >
                    <option value="none">None</option>
                    <option value="pulse">Pulse</option>
                    <option value="glow">Glow</option>
                    <option value="slide">Slide</option>
                    <option value="rotate">Rotate</option>
                </select>
            </div>
            <div className={styles.editorSection}>
                <label>Animation Duration (ms)</label>
                <input
                    type="number"
                    value={formData.animationDuration}
                    onChange={(e) => handleChange('animationDuration', parseInt(e.target.value))}
                />
            </div>
            <div className={styles.editorActions}>
                <button className={styles.saveBtn} onClick={onClose}>Done</button>
            </div>
        </div>
    );
}

