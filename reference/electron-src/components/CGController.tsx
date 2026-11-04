import React, { useState } from 'react';
import { CGElement, CGPreset, CGType } from '../lib/types';
import { loadCGElements, saveCGElements, loadCGPresets, createCGElement } from '../lib/storage';
import { Eye, EyeOff, Plus, Trash2, Edit2, Copy, Layers } from 'lucide-react';
import styles from './CGController.module.css';

interface CGControllerProps {
    elements: CGElement[];
    onToggle: (id: string) => void;
    onUpdate: (id: string, updates: Partial<CGElement>) => void;
    onAdd: (element: CGElement) => void;
    onRemove: (id: string) => void;
    onEdit: (element: CGElement) => void;
    onHideAll: () => void;
}

export function CGController({
    elements,
    onToggle,
    onUpdate,
    onAdd,
    onRemove,
    onEdit,
    onHideAll,
}: CGControllerProps) {
    const [showPresets, setShowPresets] = useState(false);
    const [presets] = useState<CGPreset[]>(() => loadCGPresets());
    const [newType, setNewType] = useState<CGType>('lower_third');

    const handleAddNew = () => {
        const element = createCGElement(newType, `New ${newType.replace('_', ' ')}`);
        onAdd(element);
    };

    const handleApplyPreset = (preset: CGPreset) => {
        const element: CGElement = {
            ...preset.element,
            id: `cg_${Date.now()}`,
            isVisible: false,
        } as CGElement;
        onAdd(element);
        setShowPresets(false);
    };

    const handleDuplicate = (element: CGElement) => {
        const copy: CGElement = {
            ...element,
            id: `cg_${Date.now()}`,
            name: `${element.name} (Copy)`,
            isVisible: false,
        };
        onAdd(copy);
    };

    // Group presets by category
    const presetsByCategory = presets.reduce((acc, preset) => {
        if (!acc[preset.category]) acc[preset.category] = [];
        acc[preset.category].push(preset);
        return acc;
    }, {} as Record<string, CGPreset[]>);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <Layers size={16} />
                    <span>CG Controller</span>
                </div>
                <button className={styles.hideAllBtn} onClick={onHideAll}>
                    <EyeOff size={14} /> Hide All
                </button>
            </div>

            {/* Quick Add */}
            <div className={styles.quickAdd}>
                <select
                    className={styles.typeSelect}
                    value={newType}
                    onChange={(e) => setNewType(e.target.value as CGType)}
                >
                    <option value="lower_third">Lower Third</option>
                    <option value="ticker">Ticker</option>
                    <option value="bug">Bug</option>
                    <option value="clock">Clock</option>
                    <option value="weather">Weather</option>
                    <option value="custom">Custom</option>
                </select>
                <button className={styles.addBtn} onClick={handleAddNew}>
                    <Plus size={14} /> New
                </button>
                <button
                    className={`${styles.presetsBtn} ${showPresets ? styles.active : ''}`}
                    onClick={() => setShowPresets(!showPresets)}
                >
                    Presets
                </button>
            </div>

            {/* Presets Panel */}
            {showPresets && (
                <div className={styles.presetsPanel}>
                    {Object.entries(presetsByCategory).map(([category, categoryPresets]) => (
                        <div key={category} className={styles.presetCategory}>
                            <div className={styles.presetCategoryTitle}>{category}</div>
                            <div className={styles.presetList}>
                                {categoryPresets.map(preset => (
                                    <button
                                        key={preset.id}
                                        className={styles.presetItem}
                                        onClick={() => handleApplyPreset(preset)}
                                    >
                                        {preset.name}
                                    </button>
                                ))}
                            </div>
                        </div>
                    ))}
                </div>
            )}

            {/* Elements List */}
            <div className={styles.elementsList}>
                {elements.map(element => (
                    <div
                        key={element.id}
                        className={`${styles.elementItem} ${element.isVisible ? styles.visible : ''}`}
                    >
                        <button
                            className={`${styles.toggleBtn} ${element.isVisible ? styles.on : ''}`}
                            onClick={() => onToggle(element.id)}
                            title={element.isVisible ? 'Hide' : 'Show'}
                        >
                            {element.isVisible ? <Eye size={16} /> : <EyeOff size={16} />}
                        </button>

                        <div className={styles.elementInfo}>
                            <span className={styles.elementName}>{element.name}</span>
                            <span className={styles.elementType}>{element.type.replace('_', ' ')}</span>
                        </div>

                        <div className={styles.elementActions}>
                            <button
                                className={styles.actionBtn}
                                onClick={() => onEdit(element)}
                                title="Edit"
                            >
                                <Edit2 size={12} />
                            </button>
                            <button
                                className={styles.actionBtn}
                                onClick={() => handleDuplicate(element)}
                                title="Duplicate"
                            >
                                <Copy size={12} />
                            </button>
                            <button
                                className={`${styles.actionBtn} ${styles.deleteBtn}`}
                                onClick={() => onRemove(element.id)}
                                title="Delete"
                            >
                                <Trash2 size={12} />
                            </button>
                        </div>
                    </div>
                ))}

                {elements.length === 0 && (
                    <div className={styles.empty}>
                        No CG elements. Add one above or use a preset.
                    </div>
                )}
            </div>

            {/* Quick Triggers */}
            {elements.length > 0 && (
                <div className={styles.quickTriggers}>
                    <div className={styles.triggerLabel}>Quick Trigger:</div>
                    <div className={styles.triggerButtons}>
                        {elements.slice(0, 6).map(el => (
                            <button
                                key={el.id}
                                className={`${styles.triggerBtn} ${el.isVisible ? styles.active : ''}`}
                                onClick={() => onToggle(el.id)}
                            >
                                {el.name.substring(0, 8)}
                            </button>
                        ))}
                    </div>
                </div>
            )}
        </div>
    );
}
