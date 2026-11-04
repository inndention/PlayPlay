/**
 * ResizableColumns - Draggable column resizer for multi-column layouts
 * 
 * Features:
 * - Drag-to-resize columns
 * - Min/max width constraints
 * - Persists sizes to localStorage
 * - Double-click to reset to default
 * - Visual feedback during drag
 */

import React, { useState, useRef, useEffect, useCallback, ReactNode } from 'react';
import styles from './ResizableColumns.module.css';

export interface ColumnConfig {
    id: string;
    minWidth: number;
    maxWidth: number;
    defaultWidth: number;
    content: ReactNode;
}

interface ResizableColumnsProps {
    columns: ColumnConfig[];
    storageKey?: string;
    className?: string;
    gap?: number;
}

interface ColumnWidths {
    [key: string]: number;
}

export function ResizableColumns({
    columns,
    storageKey = 'resizable-columns',
    className = '',
    gap = 0,
}: ResizableColumnsProps) {
    // Initialize widths from localStorage or defaults
    const getInitialWidths = (): ColumnWidths => {
        if (storageKey) {
            try {
                const saved = localStorage.getItem(storageKey);
                if (saved) {
                    return JSON.parse(saved);
                }
            } catch (e) {
                console.warn('Failed to load column widths from localStorage');
            }
        }
        
        const defaults: ColumnWidths = {};
        columns.forEach(col => {
            defaults[col.id] = col.defaultWidth;
        });
        return defaults;
    };
    
    const [widths, setWidths] = useState<ColumnWidths>(getInitialWidths);
    const [isDragging, setIsDragging] = useState<string | null>(null);
    const containerRef = useRef<HTMLDivElement>(null);
    const startXRef = useRef<number>(0);
    const startWidthsRef = useRef<ColumnWidths>({});
    
    // Save widths to localStorage
    useEffect(() => {
        if (storageKey) {
            try {
                localStorage.setItem(storageKey, JSON.stringify(widths));
            } catch (e) {
                console.warn('Failed to save column widths to localStorage');
            }
        }
    }, [widths, storageKey]);
    
    // Handle mouse down on resizer
    const handleMouseDown = useCallback((e: React.MouseEvent, resizerId: string) => {
        e.preventDefault();
        setIsDragging(resizerId);
        startXRef.current = e.clientX;
        startWidthsRef.current = { ...widths };
        
        // Add grabbing cursor to body
        document.body.style.cursor = 'col-resize';
        document.body.style.userSelect = 'none';
    }, [widths]);
    
    // Handle mouse move during drag
    useEffect(() => {
        if (!isDragging) return;
        
        const handleMouseMove = (e: MouseEvent) => {
            const delta = e.clientX - startXRef.current;
            const resizerIndex = parseInt(isDragging.split('-')[1], 10);
            
            // Get the columns on either side of the resizer
            const leftColId = columns[resizerIndex].id;
            const rightColId = columns[resizerIndex + 1]?.id;
            
            if (!rightColId) return; // No column to the right
            
            const leftCol = columns[resizerIndex];
            const rightCol = columns[resizerIndex + 1];
            
            // Calculate new widths
            let newLeftWidth = startWidthsRef.current[leftColId] + delta;
            let newRightWidth = startWidthsRef.current[rightColId] - delta;
            
            // Apply constraints
            if (newLeftWidth < leftCol.minWidth) {
                const diff = leftCol.minWidth - newLeftWidth;
                newLeftWidth = leftCol.minWidth;
                newRightWidth -= diff;
            }
            if (newLeftWidth > leftCol.maxWidth) {
                const diff = newLeftWidth - leftCol.maxWidth;
                newLeftWidth = leftCol.maxWidth;
                newRightWidth += diff;
            }
            if (newRightWidth < rightCol.minWidth) {
                const diff = rightCol.minWidth - newRightWidth;
                newRightWidth = rightCol.minWidth;
                newLeftWidth -= diff;
            }
            if (newRightWidth > rightCol.maxWidth) {
                const diff = newRightWidth - rightCol.maxWidth;
                newRightWidth = rightCol.maxWidth;
                newLeftWidth += diff;
            }
            
            // Final constraint check
            newLeftWidth = Math.max(leftCol.minWidth, Math.min(leftCol.maxWidth, newLeftWidth));
            newRightWidth = Math.max(rightCol.minWidth, Math.min(rightCol.maxWidth, newRightWidth));
            
            setWidths(prev => ({
                ...prev,
                [leftColId]: newLeftWidth,
                [rightColId]: newRightWidth,
            }));
        };
        
        const handleMouseUp = () => {
            setIsDragging(null);
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        };
        
        window.addEventListener('mousemove', handleMouseMove);
        window.addEventListener('mouseup', handleMouseUp);
        
        return () => {
            window.removeEventListener('mousemove', handleMouseMove);
            window.removeEventListener('mouseup', handleMouseUp);
        };
    }, [isDragging, columns]);
    
    // Double-click to reset to default
    const handleDoubleClick = useCallback((resizerIndex: number) => {
        const leftCol = columns[resizerIndex];
        const rightCol = columns[resizerIndex + 1];
        
        if (!rightCol) return;
        
        setWidths(prev => ({
            ...prev,
            [leftCol.id]: leftCol.defaultWidth,
            [rightCol.id]: rightCol.defaultWidth,
        }));
    }, [columns]);
    
    // Build grid template
    const gridTemplateColumns = columns
        .map((col, i) => {
            // For middle flexible columns, use minmax with the stored width as preferred
            if (col.maxWidth === Infinity || col.maxWidth > 2000) {
                return `minmax(${col.minWidth}px, 1fr)`;
            }
            return `${widths[col.id] || col.defaultWidth}px`;
        })
        .join(' ');
    
    return (
        <div
            ref={containerRef}
            className={`${styles.container} ${className} ${isDragging ? styles.dragging : ''}`}
            style={{
                display: 'grid',
                gridTemplateColumns,
                gap: `${gap}px`,
            }}
        >
            {columns.map((col, index) => (
                <React.Fragment key={col.id}>
                    <div 
                        className={styles.column}
                        style={{
                            minWidth: col.minWidth,
                            maxWidth: col.maxWidth === Infinity ? undefined : col.maxWidth,
                        }}
                    >
                        {col.content}
                    </div>
                    
                    {/* Resizer between columns (not after the last one) */}
                    {index < columns.length - 1 && (
                        <div
                            className={`${styles.resizer} ${isDragging === `resizer-${index}` ? styles.active : ''}`}
                            onMouseDown={(e) => handleMouseDown(e, `resizer-${index}`)}
                            onDoubleClick={() => handleDoubleClick(index)}
                            title="Drag to resize, double-click to reset"
                        >
                            <div className={styles.resizerHandle} />
                        </div>
                    )}
                </React.Fragment>
            ))}
        </div>
    );
}

/**
 * Simple hook for single resizer between two elements
 */
export function useResizer(
    initialWidth: number,
    minWidth: number,
    maxWidth: number,
    storageKey?: string
) {
    const [width, setWidth] = useState(() => {
        if (storageKey) {
            const saved = localStorage.getItem(storageKey);
            if (saved) return parseInt(saved, 10);
        }
        return initialWidth;
    });
    
    const [isDragging, setIsDragging] = useState(false);
    const startXRef = useRef(0);
    const startWidthRef = useRef(0);
    
    useEffect(() => {
        if (storageKey) {
            localStorage.setItem(storageKey, width.toString());
        }
    }, [width, storageKey]);
    
    const handleMouseDown = useCallback((e: React.MouseEvent) => {
        e.preventDefault();
        setIsDragging(true);
        startXRef.current = e.clientX;
        startWidthRef.current = width;
        document.body.style.cursor = 'col-resize';
        document.body.style.userSelect = 'none';
    }, [width]);
    
    useEffect(() => {
        if (!isDragging) return;
        
        const handleMouseMove = (e: MouseEvent) => {
            const delta = e.clientX - startXRef.current;
            const newWidth = Math.max(minWidth, Math.min(maxWidth, startWidthRef.current + delta));
            setWidth(newWidth);
        };
        
        const handleMouseUp = () => {
            setIsDragging(false);
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        };
        
        window.addEventListener('mousemove', handleMouseMove);
        window.addEventListener('mouseup', handleMouseUp);
        
        return () => {
            window.removeEventListener('mousemove', handleMouseMove);
            window.removeEventListener('mouseup', handleMouseUp);
        };
    }, [isDragging, minWidth, maxWidth]);
    
    const reset = useCallback(() => {
        setWidth(initialWidth);
    }, [initialWidth]);
    
    return {
        width,
        isDragging,
        handleMouseDown,
        reset,
    };
}


