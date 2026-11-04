import React, { useState, useRef } from 'react';
import { CGElement, CGAnimationIn, CGAnimationOut, CGLoop, CGType } from '../lib/types';
import { loadSettings } from '../lib/storage';
import { Save, X, Move, Palette, Wand2, Play, Eye } from 'lucide-react';
import styles from './CGBuilder.module.css';

interface CGBuilderProps {
    element: CGElement;
    onSave: (element: CGElement) => void;
    onCancel: () => void;
    onPreview?: () => void;
}

export function CGBuilder({ element, onSave, onCancel, onPreview }: CGBuilderProps) {
    const [editedElement, setEditedElement] = useState<CGElement>({ ...element });
    const [activeTab, setActiveTab] = useState<'content' | 'style' | 'animation' | 'position'>('content');
    const previewRef = useRef<HTMLDivElement>(null);

    const handleChange = <K extends keyof CGElement>(key: K, value: CGElement[K]) => {
        setEditedElement(prev => ({ ...prev, [key]: value }));
    };

    const handleColorChange = (key: keyof CGElement['colors'], value: string | boolean) => {
        setEditedElement(prev => ({
            ...prev,
            colors: { ...prev.colors, [key]: value }
        }));
    };

    const handleEffectChange = (key: keyof CGElement['effects'], value: unknown) => {
        setEditedElement(prev => ({
            ...prev,
            effects: { ...prev.effects, [key]: value }
        }));
    };

    const handlePositionChange = (key: keyof CGElement['position'], value: number | string) => {
        setEditedElement(prev => ({
            ...prev,
            position: { ...prev.position, [key]: value }
        }));
    };

    const handleSave = () => {
        onSave(editedElement);
    };

    // Live preview style
    const previewStyle: React.CSSProperties = {
        position: 'absolute',
        left: `${editedElement.position.x}%`,
        top: `${editedElement.position.y}%`,
        width: `${editedElement.position.width}%`,
        minHeight: `${editedElement.position.height}%`,
        background: editedElement.colors.gradient
            ? `linear-gradient(90deg, ${editedElement.colors.primary}, ${editedElement.colors.secondary})`
            : editedElement.colors.primary,
        color: editedElement.colors.text,
        padding: '0.5rem 1rem',
        borderRadius: editedElement.effects.borderRadius,
        opacity: editedElement.effects.opacity,
        boxShadow: editedElement.effects.shadow
            ? `0 4px 15px ${editedElement.effects.shadowColor || '#000'}40`
            : 'none',
        fontSize: editedElement.fontSize,
        display: 'flex',
        flexDirection: 'column',
        justifyContent: 'center',
    };

    return (
        <div className={styles.container}>
            {/* Header */}
            <div className={styles.header}>
                <input
                    type="text"
                    className={styles.nameInput}
                    value={editedElement.name}
                    onChange={(e) => handleChange('name', e.target.value)}
                    placeholder="Element Name"
                />
                <div className={styles.headerActions}>
                    <button className={styles.cancelBtn} onClick={onCancel}>
                        <X size={14} /> Cancel
                    </button>
                    <button className={styles.saveBtn} onClick={handleSave}>
                        <Save size={14} /> Save
                    </button>
                </div>
            </div>

            <div className={styles.body}>
                {/* Preview Area */}
                <div className={styles.previewArea} ref={previewRef}>
                    <div className={styles.previewAspect}>
                        <div className={styles.previewLabel}>Preview (16:9)</div>
                        <div style={previewStyle}>
                            <div style={{ fontWeight: 700 }}>{editedElement.title}</div>
                            {editedElement.subtitle && (
                                <div style={{ fontSize: '0.8em', opacity: 0.9 }}>{editedElement.subtitle}</div>
                            )}
                        </div>
                    </div>
                </div>

                {/* Tabs */}
                <div className={styles.tabs}>
                    <button
                        className={`${styles.tab} ${activeTab === 'content' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('content')}
                    >
                        Content
                    </button>
                    <button
                        className={`${styles.tab} ${activeTab === 'style' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('style')}
                    >
                        <Palette size={12} /> Style
                    </button>
                    <button
                        className={`${styles.tab} ${activeTab === 'animation' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('animation')}
                    >
                        <Wand2 size={12} /> Animation
                    </button>
                    <button
                        className={`${styles.tab} ${activeTab === 'position' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('position')}
                    >
                        <Move size={12} /> Position
                    </button>
                </div>

                {/* Tab Content */}
                <div className={styles.tabContent}>
                    {activeTab === 'content' && (
                        <div className={styles.section}>
                            <div className={styles.sectionLabel}>Dynamic text</div>
                            <div className={styles.field}>
                                <label>Title</label>
                                <input
                                    type="text"
                                    value={editedElement.title}
                                    onChange={(e) => handleChange('title', e.target.value)}
                                    placeholder="e.g. Guest Name"
                                />
                            </div>
                            <div className={styles.field}>
                                <label>Subtitle</label>
                                <input
                                    type="text"
                                    value={editedElement.subtitle || ''}
                                    onChange={(e) => handleChange('subtitle', e.target.value)}
                                    placeholder="e.g. Title / Description"
                                />
                            </div>
                            {editedElement.type === 'ticker' && (
                                <div className={styles.field}>
                                    <label>Ticker Text</label>
                                    <textarea
                                        value={editedElement.tickerText || ''}
                                        onChange={(e) => handleChange('tickerText', e.target.value)}
                                        rows={3}
                                    />
                                </div>
                            )}
                            <div className={styles.field}>
                                <label>Font Size (px)</label>
                                <input
                                    type="number"
                                    value={editedElement.fontSize || 24}
                                    onChange={(e) => handleChange('fontSize', parseInt(e.target.value))}
                                    min={10}
                                    max={72}
                                />
                            </div>
                        </div>
                    )}

                    {activeTab === 'style' && (
                        <div className={styles.section}>
                            <div className={styles.colorRow}>
                                <div className={styles.field}>
                                    <label>Primary Color</label>
                                    <input
                                        type="color"
                                        value={editedElement.colors.primary}
                                        onChange={(e) => handleColorChange('primary', e.target.value)}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Secondary Color</label>
                                    <input
                                        type="color"
                                        value={editedElement.colors.secondary}
                                        onChange={(e) => handleColorChange('secondary', e.target.value)}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Text Color</label>
                                    <input
                                        type="color"
                                        value={editedElement.colors.text}
                                        onChange={(e) => handleColorChange('text', e.target.value)}
                                    />
                                </div>
                            </div>
                            <label className={styles.checkbox}>
                                <input
                                    type="checkbox"
                                    checked={editedElement.colors.gradient}
                                    onChange={(e) => handleColorChange('gradient', e.target.checked)}
                                />
                                Enable Gradient
                            </label>
                            <div className={styles.field}>
                                <label>Border Radius</label>
                                <input
                                    type="range"
                                    min={0}
                                    max={20}
                                    value={editedElement.effects.borderRadius || 0}
                                    onChange={(e) => handleEffectChange('borderRadius', parseInt(e.target.value))}
                                />
                                <span>{editedElement.effects.borderRadius}px</span>
                            </div>
                            <div className={styles.field}>
                                <label>Opacity</label>
                                <input
                                    type="range"
                                    min={0}
                                    max={100}
                                    value={(editedElement.effects.opacity || 1) * 100}
                                    onChange={(e) => handleEffectChange('opacity', parseInt(e.target.value) / 100)}
                                />
                                <span>{Math.round((editedElement.effects.opacity || 1) * 100)}%</span>
                            </div>
                            <label className={styles.checkbox}>
                                <input
                                    type="checkbox"
                                    checked={editedElement.effects.shadow}
                                    onChange={(e) => handleEffectChange('shadow', e.target.checked)}
                                />
                                Enable Shadow
                            </label>
                            <label className={styles.checkbox}>
                                <input
                                    type="checkbox"
                                    checked={editedElement.effects.glow}
                                    onChange={(e) => handleEffectChange('glow', e.target.checked)}
                                />
                                Enable Glow
                            </label>
                        </div>
                    )}

                    {activeTab === 'animation' && (
                        <div className={styles.section}>
                            <div className={styles.field}>
                                <label>Animation In</label>
                                <select
                                    value={editedElement.animationIn}
                                    onChange={(e) => handleChange('animationIn', e.target.value as CGAnimationIn)}
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
                            <div className={styles.field}>
                                <label>Animation Out</label>
                                <select
                                    value={editedElement.animationOut}
                                    onChange={(e) => handleChange('animationOut', e.target.value as CGAnimationOut)}
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
                            <div className={styles.field}>
                                <label>Duration (ms)</label>
                                <input
                                    type="number"
                                    value={editedElement.animationDuration}
                                    onChange={(e) => handleChange('animationDuration', parseInt(e.target.value))}
                                    min={100}
                                    max={3000}
                                    step={100}
                                />
                            </div>
                            <div className={styles.field}>
                                <label>Loop Effect</label>
                                <select
                                    value={editedElement.loop}
                                    onChange={(e) => handleChange('loop', e.target.value as CGLoop)}
                                >
                                    <option value="none">None</option>
                                    <option value="pulse">Pulse</option>
                                    <option value="sheen">Sheen</option>
                                    <option value="shimmer">Shimmer</option>
                                    <option value="glow_pulse">Glow Pulse</option>
                                    <option value="bounce">Bounce</option>
                                    <option value="ticker_scroll">Ticker Scroll</option>
                                </select>
                            </div>
                            <div className={styles.field}>
                                <label>Auto-hide after (ms, 0 = manual)</label>
                                <input
                                    type="number"
                                    value={editedElement.autoHideAfter || 0}
                                    onChange={(e) => handleChange('autoHideAfter', parseInt(e.target.value))}
                                    min={0}
                                    max={60000}
                                    step={1000}
                                />
                            </div>
                        </div>
                    )}

                    {activeTab === 'position' && (
                        <div className={styles.section}>
                            <div className={styles.field}>
                                <label>Anchor</label>
                                <select
                                    value={editedElement.position.anchor}
                                    onChange={(e) => handlePositionChange('anchor', e.target.value)}
                                >
                                    <option value="top-left">Top Left</option>
                                    <option value="top-center">Top Center</option>
                                    <option value="top-right">Top Right</option>
                                    <option value="center-left">Center Left</option>
                                    <option value="center">Center</option>
                                    <option value="center-right">Center Right</option>
                                    <option value="bottom-left">Bottom Left</option>
                                    <option value="bottom-center">Bottom Center</option>
                                    <option value="bottom-right">Bottom Right</option>
                                </select>
                            </div>
                            <div className={styles.positionGrid}>
                                <div className={styles.field}>
                                    <label>X Position (%)</label>
                                    <input
                                        type="number"
                                        value={editedElement.position.x}
                                        onChange={(e) => handlePositionChange('x', parseInt(e.target.value))}
                                        min={0}
                                        max={100}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Y Position (%)</label>
                                    <input
                                        type="number"
                                        value={editedElement.position.y}
                                        onChange={(e) => handlePositionChange('y', parseInt(e.target.value))}
                                        min={0}
                                        max={100}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Width (%)</label>
                                    <input
                                        type="number"
                                        value={editedElement.position.width}
                                        onChange={(e) => handlePositionChange('width', parseInt(e.target.value))}
                                        min={5}
                                        max={100}
                                    />
                                </div>
                                <div className={styles.field}>
                                    <label>Height (%)</label>
                                    <input
                                        type="number"
                                        value={editedElement.position.height}
                                        onChange={(e) => handlePositionChange('height', parseInt(e.target.value))}
                                        min={2}
                                        max={50}
                                    />
                                </div>
                            </div>
                        </div>
                    )}
                </div>
            </div>
        </div>
    );
}
