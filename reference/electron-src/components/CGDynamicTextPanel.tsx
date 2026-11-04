/**
 * CG Quick Edit - Set dynamic text and position for CG from the main interface.
 */
import React, { useState, useEffect } from 'react';
import { Type, Move, ExternalLink, ChevronDown, ChevronUp } from 'lucide-react';
import { CGElement } from '../lib/types';
import styles from './CGDynamicTextPanel.module.css';

interface CGDynamicTextPanelProps {
    elements: CGElement[];
    onUpdateElement: (id: string, updates: Partial<CGElement>) => void;
    cgOutputUrl?: string;
    defaultCollapsed?: boolean;
}

export function CGDynamicTextPanel({
    elements,
    onUpdateElement,
    cgOutputUrl = 'http://localhost:8080/cg-output',
    defaultCollapsed = false,
}: CGDynamicTextPanelProps) {
    const [collapsed, setCollapsed] = useState(defaultCollapsed);
    const [selectedId, setSelectedId] = useState<string | null>(() => elements[0]?.id ?? null);
    const [title, setTitle] = useState('');
    const [subtitle, setSubtitle] = useState('');
    const [x, setX] = useState(5);
    const [y, setY] = useState(75);
    const [width, setWidth] = useState(40);
    const [height, setHeight] = useState(15);

    const selected = elements.find(e => e.id === selectedId) ?? elements[0];

    // Sync from selected element
    useEffect(() => {
        if (!selected) return;
        setTitle(selected.title ?? '');
        setSubtitle(selected.subtitle ?? '');
        setX(selected.position?.x ?? 5);
        setY(selected.position?.y ?? 75);
        setWidth(selected.position?.width ?? 40);
        setHeight(selected.position?.height ?? 15);
    }, [selected?.id, selected?.title, selected?.subtitle, selected?.position?.x, selected?.position?.y, selected?.position?.width, selected?.position?.height]);

    const handleApply = () => {
        if (!selected) return;
        onUpdateElement(selected.id, {
            title,
            subtitle: subtitle || undefined,
            position: {
                ...selected.position,
                x,
                y,
                width,
                height,
            },
        });
    };

    const handleOpenCgOutput = () => {
        window.open(cgOutputUrl, '_blank', 'noopener');
    };

    if (elements.length === 0) {
        return (
            <div className={styles.panel}>
                <button type="button" className={styles.header} onClick={() => setCollapsed(!collapsed)}>
                    <Type size={14} /> CG Dynamic Text
                    {collapsed ? <ChevronDown size={14} /> : <ChevronUp size={14} />}
                </button>
                {!collapsed && (
                    <div className={styles.body}>
                        <p className={styles.hint}>Add a CG element from the CG panel (Layers icon) to edit text and position here.</p>
                        <div className={styles.browserOut}>
                            <a href={cgOutputUrl} target="_blank" rel="noopener noreferrer" className={styles.link}>
                                <ExternalLink size={12} /> CG Browser Output
                            </a>
                        </div>
                    </div>
                )}
            </div>
        );
    }

    return (
        <div className={styles.panel}>
            <button type="button" className={styles.header} onClick={() => setCollapsed(!collapsed)}>
                <Type size={14} /> CG Dynamic Text
                {collapsed ? <ChevronDown size={14} /> : <ChevronUp size={14} />}
            </button>
            {!collapsed && (
                <div className={styles.body}>
                    <div className={styles.field}>
                        <label>Element</label>
                        <select value={selectedId ?? ''} onChange={(e) => setSelectedId(e.target.value || null)}>
                            {elements.map(el => (
                                <option key={el.id} value={el.id}>{el.name}</option>
                            ))}
                        </select>
                    </div>
                    <div className={styles.field}>
                        <label>Title</label>
                        <input type="text" value={title} onChange={(e) => setTitle(e.target.value)} placeholder="Dynamic title" />
                    </div>
                    <div className={styles.field}>
                        <label>Subtitle</label>
                        <input type="text" value={subtitle} onChange={(e) => setSubtitle(e.target.value)} placeholder="Dynamic subtitle" />
                    </div>
                    <div className={styles.positionRow}>
                        <div className={styles.field}>
                            <label>X %</label>
                            <input type="number" min={0} max={100} value={x} onChange={(e) => setX(Number(e.target.value))} />
                        </div>
                        <div className={styles.field}>
                            <label>Y %</label>
                            <input type="number" min={0} max={100} value={y} onChange={(e) => setY(Number(e.target.value))} />
                        </div>
                        <div className={styles.field}>
                            <label>W %</label>
                            <input type="number" min={5} max={100} value={width} onChange={(e) => setWidth(Number(e.target.value))} />
                        </div>
                        <div className={styles.field}>
                            <label>H %</label>
                            <input type="number" min={2} max={50} value={height} onChange={(e) => setHeight(Number(e.target.value))} />
                        </div>
                    </div>
                    <div className={styles.actions}>
                        <button type="button" className={styles.applyBtn} onClick={handleApply}>
                            <Move size={12} /> Apply
                        </button>
                        <button type="button" className={styles.browserBtn} onClick={handleOpenCgOutput} title="Open CG in new window (OBS Browser Source)">
                            <ExternalLink size={12} /> Browser Out
                        </button>
                    </div>
                </div>
            )}
        </div>
    );
}
