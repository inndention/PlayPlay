// ============================================
// CG CANVAS - Broadcast Graphics Canvas
// Renders CG elements with safe zones and grid
// ============================================

import React, { useRef, useEffect, useState, useCallback } from 'react';
import { CGElement } from '../lib/types';
import styles from './CGCanvas.module.css';

interface CGCanvasProps {
    elements: CGElement[];
    selectedElementId: string | null;
    onElementSelect: (id: string | null) => void;
    onElementMove: (id: string, x: number, y: number) => void;
    showGrid?: boolean;
    showSafeZone?: boolean;
    snapToGrid?: boolean;
    isPreviewMode?: boolean;
}

// Broadcast standard resolution
const CANVAS_WIDTH = 1920;
const CANVAS_HEIGHT = 1080;
const SAFE_ZONE_PERCENT = 5; // 5% from each edge (Title Safe)
const ACTION_SAFE_PERCENT = 3.5; // 3.5% from each edge (Action Safe)

export function CGCanvas({
    elements,
    selectedElementId,
    onElementSelect,
    onElementMove,
    showGrid = true,
    showSafeZone = true,
    snapToGrid = true,
    isPreviewMode = false,
}: CGCanvasProps) {
    const canvasRef = useRef<HTMLDivElement>(null);
    const [scale, setScale] = useState(1);
    const [dragging, setDragging] = useState<{
        elementId: string;
        startX: number;
        startY: number;
        elementStartX: number;
        elementStartY: number;
    } | null>(null);
    const [animationStates, setAnimationStates] = useState<Map<string, 'animating_in' | 'visible' | 'animating_out' | 'hidden'>>(new Map());

    // Calculate scale to fit canvas in container
    useEffect(() => {
        const updateScale = () => {
            if (!canvasRef.current?.parentElement) return;
            const container = canvasRef.current.parentElement;
            const containerWidth = container.clientWidth - 48;
            const containerHeight = container.clientHeight - 48;
            const scaleX = containerWidth / CANVAS_WIDTH;
            const scaleY = containerHeight / CANVAS_HEIGHT;
            setScale(Math.min(scaleX, scaleY, 1));
        };
        
        updateScale();
        window.addEventListener('resize', updateScale);
        return () => window.removeEventListener('resize', updateScale);
    }, []);

    // Handle preview mode animations
    useEffect(() => {
        if (isPreviewMode) {
            const newStates = new Map<string, 'animating_in' | 'visible' | 'animating_out' | 'hidden'>();
            elements.forEach((el, index) => {
                if (el.isVisible) {
                    // Stagger animations
                    setTimeout(() => {
                        setAnimationStates(prev => {
                            const updated = new Map(prev);
                            updated.set(el.id, 'animating_in');
                            return updated;
                        });
                        
                        // After animation duration, set to visible
                        setTimeout(() => {
                            setAnimationStates(prev => {
                                const updated = new Map(prev);
                                updated.set(el.id, 'visible');
                                return updated;
                            });
                        }, el.animationDuration || 500);
                    }, index * 200);
                }
            });
        } else {
            setAnimationStates(new Map());
        }
    }, [isPreviewMode, elements]);

    const handleMouseDown = useCallback((e: React.MouseEvent, elementId: string) => {
        if (isPreviewMode) return;
        e.stopPropagation();
        
        const element = elements.find(el => el.id === elementId);
        if (!element) return;
        
        onElementSelect(elementId);
        
        setDragging({
            elementId,
            startX: e.clientX,
            startY: e.clientY,
            elementStartX: (element.position.x / 100) * CANVAS_WIDTH,
            elementStartY: (element.position.y / 100) * CANVAS_HEIGHT,
        });
    }, [elements, onElementSelect, isPreviewMode]);

    useEffect(() => {
        if (!dragging) return;
        
        const handleMouseMove = (e: MouseEvent) => {
            const deltaX = (e.clientX - dragging.startX) / scale;
            const deltaY = (e.clientY - dragging.startY) / scale;
            
            let newX = dragging.elementStartX + deltaX;
            let newY = dragging.elementStartY + deltaY;
            
            // Snap to grid if enabled
            if (snapToGrid) {
                const gridSize = 20;
                newX = Math.round(newX / gridSize) * gridSize;
                newY = Math.round(newY / gridSize) * gridSize;
            }
            
            // Clamp to canvas bounds
            newX = Math.max(0, Math.min(CANVAS_WIDTH, newX));
            newY = Math.max(0, Math.min(CANVAS_HEIGHT, newY));
            
            onElementMove(dragging.elementId, newX, newY);
        };
        
        const handleMouseUp = () => {
            setDragging(null);
        };
        
        window.addEventListener('mousemove', handleMouseMove);
        window.addEventListener('mouseup', handleMouseUp);
        
        return () => {
            window.removeEventListener('mousemove', handleMouseMove);
            window.removeEventListener('mouseup', handleMouseUp);
        };
    }, [dragging, scale, snapToGrid, onElementMove]);

    const handleCanvasClick = useCallback((e: React.MouseEvent) => {
        if (e.target === canvasRef.current) {
            onElementSelect(null);
        }
    }, [onElementSelect]);

    const getAnimationStyle = (element: CGElement): React.CSSProperties => {
        const state = animationStates.get(element.id);
        const duration = element.animationDuration || 500;
        
        if (!isPreviewMode || !state) return {};
        
        const baseStyle: React.CSSProperties = {
            transition: `all ${duration}ms cubic-bezier(0.4, 0, 0.2, 1)`,
        };
        
        if (state === 'hidden') {
            return { ...baseStyle, opacity: 0 };
        }
        
        if (state === 'animating_in' || state === 'visible') {
            const animIn = element.animationIn || 'fade';
            
            if (state === 'animating_in') {
                switch (animIn) {
                    case 'slide_left':
                        return { ...baseStyle, transform: 'translateX(-100%)', opacity: 0 };
                    case 'slide_right':
                        return { ...baseStyle, transform: 'translateX(100%)', opacity: 0 };
                    case 'slide_up':
                        return { ...baseStyle, transform: 'translateY(100%)', opacity: 0 };
                    case 'slide_down':
                        return { ...baseStyle, transform: 'translateY(-100%)', opacity: 0 };
                    case 'zoom':
                        return { ...baseStyle, transform: 'scale(0)', opacity: 0 };
                    case 'fade':
                    default:
                        return { ...baseStyle, opacity: 0 };
                }
            }
            
            // Visible state with loop animations
            if (state === 'visible' && element.loop && element.loop !== 'none') {
                const loopDuration = element.loopDuration || 2000;
                switch (element.loop) {
                    case 'pulse':
                        return { 
                            ...baseStyle, 
                            animation: `pulse ${loopDuration}ms ease-in-out infinite`,
                        };
                    case 'glow_pulse':
                        return { 
                            ...baseStyle, 
                            animation: `glowPulse ${loopDuration}ms ease-in-out infinite`,
                        };
                    case 'shimmer':
                        return { 
                            ...baseStyle, 
                            animation: `shimmer ${loopDuration}ms linear infinite`,
                        };
                    case 'ticker_scroll':
                        return baseStyle; // Handled separately in ticker rendering
                }
            }
        }
        
        return baseStyle;
    };

    const renderElement = (element: CGElement) => {
        if (!element.isVisible && !isPreviewMode) return null;
        if (isPreviewMode && !element.isVisible) return null;
        
        const isSelected = selectedElementId === element.id && !isPreviewMode;
        const x = (element.position.x / 100) * CANVAS_WIDTH;
        const y = (element.position.y / 100) * CANVAS_HEIGHT;
        const width = (element.position.width / 100) * CANVAS_WIDTH;
        const height = (element.position.height / 100) * CANVAS_HEIGHT;
        
        const bgStyle: React.CSSProperties = {
            background: element.colors.gradient 
                ? element.colors.gradientDirection === 'vertical'
                    ? `linear-gradient(180deg, ${element.colors.primary} 0%, ${element.colors.secondary} 100%)`
                    : element.colors.gradientDirection === 'diagonal'
                        ? `linear-gradient(135deg, ${element.colors.primary} 0%, ${element.colors.secondary} 100%)`
                        : `linear-gradient(90deg, ${element.colors.primary} 0%, ${element.colors.secondary} 100%)`
                : element.colors.primary,
            color: element.colors.text,
            borderRadius: element.effects.borderRadius || 0,
            opacity: (element.effects.opacity || 100) / 100,
            ...(element.effects.shadow && {
                boxShadow: `0 4px ${element.effects.shadowBlur || 8}px ${element.effects.shadowColor || 'rgba(0,0,0,0.4)'}`,
            }),
            ...(element.effects.glow && {
                boxShadow: `0 0 ${element.effects.glowIntensity || 20}px ${element.effects.glowColor || element.colors.primary}`,
            }),
            ...(element.effects.glassMorphism && {
                backdropFilter: 'blur(10px)',
                backgroundColor: 'rgba(15, 23, 42, 0.7)',
            }),
        };
        
        const animStyle = getAnimationStyle(element);
        
        const elementStyle: React.CSSProperties = {
            position: 'absolute',
            left: x,
            top: y,
            width,
            height,
            cursor: isPreviewMode ? 'default' : 'move',
            zIndex: isSelected ? 100 : 10,
            ...bgStyle,
            ...animStyle,
        };
        
        // Render based on element type
        switch (element.type) {
            case 'lower_third':
                return (
                    <div
                        key={element.id}
                        className={`${styles.element} ${isSelected ? styles.selected : ''}`}
                        style={elementStyle}
                        onMouseDown={(e) => handleMouseDown(e, element.id)}
                    >
                        <div className={styles.lowerThird}>
                            <div 
                                className={styles.l3Title}
                                style={{ 
                                    fontSize: element.fontSize || 32,
                                    fontFamily: element.fontFamily || 'Arial',
                                }}
                            >
                                {element.title}
                            </div>
                            {element.subtitle && (
                                <div 
                                    className={styles.l3Subtitle}
                                    style={{ 
                                        fontSize: (element.fontSize || 32) * 0.6,
                                        fontFamily: element.fontFamily || 'Arial',
                                    }}
                                >
                                    {element.subtitle}
                                </div>
                            )}
                        </div>
                        {isSelected && <div className={styles.resizeHandle} />}
                    </div>
                );
            
            case 'ticker':
                return (
                    <div
                        key={element.id}
                        className={`${styles.element} ${isSelected ? styles.selected : ''}`}
                        style={{
                            ...elementStyle,
                            overflow: 'hidden',
                            display: 'flex',
                            alignItems: 'center',
                        }}
                        onMouseDown={(e) => handleMouseDown(e, element.id)}
                    >
                        <div 
                            className={styles.tickerLabel}
                            style={{ 
                                fontSize: element.fontSize || 20,
                                fontFamily: element.fontFamily || 'Arial',
                                backgroundColor: element.colors.primary,
                                padding: '0 12px',
                                flexShrink: 0,
                            }}
                        >
                            {element.title}
                        </div>
                        <div 
                            className={`${styles.tickerContent} ${isPreviewMode ? styles.tickerAnimated : ''}`}
                            style={{ 
                                fontSize: element.fontSize || 20,
                                fontFamily: element.fontFamily || 'Arial',
                                animationDuration: `${element.loopDuration || 30000}ms`,
                            }}
                        >
                            {element.tickerText}
                        </div>
                        {isSelected && <div className={styles.resizeHandle} />}
                    </div>
                );
            
            case 'bug':
                return (
                    <div
                        key={element.id}
                        className={`${styles.element} ${styles.bug} ${isSelected ? styles.selected : ''}`}
                        style={elementStyle}
                        onMouseDown={(e) => handleMouseDown(e, element.id)}
                    >
                        {element.imageSrc ? (
                            <img src={element.imageSrc} alt="" className={styles.bugImage} />
                        ) : (
                            <span 
                                style={{ 
                                    fontSize: element.fontSize || 16,
                                    fontFamily: element.fontFamily || 'Arial',
                                    fontWeight: 'bold',
                                }}
                            >
                                {element.title}
                            </span>
                        )}
                        {isSelected && <div className={styles.resizeHandle} />}
                    </div>
                );
            
            case 'clock':
                return (
                    <div
                        key={element.id}
                        className={`${styles.element} ${isSelected ? styles.selected : ''}`}
                        style={elementStyle}
                        onMouseDown={(e) => handleMouseDown(e, element.id)}
                    >
                        <ClockDisplay 
                            fontSize={element.fontSize || 20}
                            fontFamily={element.fontFamily || 'Roboto Mono'}
                            isPreviewMode={isPreviewMode}
                        />
                        {isSelected && <div className={styles.resizeHandle} />}
                    </div>
                );
            
            default:
                // Custom / generic element
                return (
                    <div
                        key={element.id}
                        className={`${styles.element} ${isSelected ? styles.selected : ''}`}
                        style={elementStyle}
                        onMouseDown={(e) => handleMouseDown(e, element.id)}
                    >
                        <div 
                            className={styles.customContent}
                            style={{ 
                                fontSize: element.fontSize || 48,
                                fontFamily: element.fontFamily || 'Arial',
                            }}
                        >
                            {element.title}
                            {element.subtitle && (
                                <div style={{ fontSize: (element.fontSize || 48) * 0.5 }}>
                                    {element.subtitle}
                                </div>
                            )}
                        </div>
                        {isSelected && <div className={styles.resizeHandle} />}
                    </div>
                );
        }
    };

    return (
        <div className={styles.canvasWrapper}>
            <div
                ref={canvasRef}
                className={`${styles.canvas} ${isPreviewMode ? styles.previewMode : ''}`}
                style={{
                    width: CANVAS_WIDTH,
                    height: CANVAS_HEIGHT,
                    transform: `scale(${scale})`,
                    transformOrigin: 'center center',
                }}
                onClick={handleCanvasClick}
            >
                {/* Grid Overlay */}
                {showGrid && !isPreviewMode && (
                    <div className={styles.gridOverlay}>
                        <svg width="100%" height="100%">
                            <defs>
                                <pattern id="smallGrid" width="20" height="20" patternUnits="userSpaceOnUse">
                                    <path d="M 20 0 L 0 0 0 20" fill="none" stroke="rgba(59,130,246,0.1)" strokeWidth="0.5" />
                                </pattern>
                                <pattern id="grid" width="100" height="100" patternUnits="userSpaceOnUse">
                                    <rect width="100" height="100" fill="url(#smallGrid)" />
                                    <path d="M 100 0 L 0 0 0 100" fill="none" stroke="rgba(59,130,246,0.2)" strokeWidth="1" />
                                </pattern>
                            </defs>
                            <rect width="100%" height="100%" fill="url(#grid)" />
                        </svg>
                    </div>
                )}
                
                {/* Safe Zone Overlay */}
                {showSafeZone && !isPreviewMode && (
                    <div className={styles.safeZoneOverlay}>
                        {/* Action Safe */}
                        <div 
                            className={styles.safeZone}
                            style={{
                                left: `${ACTION_SAFE_PERCENT}%`,
                                top: `${ACTION_SAFE_PERCENT}%`,
                                right: `${ACTION_SAFE_PERCENT}%`,
                                bottom: `${ACTION_SAFE_PERCENT}%`,
                                borderColor: 'rgba(239, 68, 68, 0.5)',
                            }}
                        >
                            <span className={styles.safeZoneLabel} style={{ color: 'rgba(239, 68, 68, 0.7)' }}>
                                Action Safe (3.5%)
                            </span>
                        </div>
                        {/* Title Safe */}
                        <div 
                            className={styles.safeZone}
                            style={{
                                left: `${SAFE_ZONE_PERCENT}%`,
                                top: `${SAFE_ZONE_PERCENT}%`,
                                right: `${SAFE_ZONE_PERCENT}%`,
                                bottom: `${SAFE_ZONE_PERCENT}%`,
                                borderColor: 'rgba(34, 197, 94, 0.5)',
                            }}
                        >
                            <span className={styles.safeZoneLabel} style={{ color: 'rgba(34, 197, 94, 0.7)' }}>
                                Title Safe (5%)
                            </span>
                        </div>
                        {/* Center crosshair */}
                        <div className={styles.centerCrosshair}>
                            <div className={styles.crosshairH} />
                            <div className={styles.crosshairV} />
                        </div>
                    </div>
                )}
                
                {/* Elements */}
                {elements.map(renderElement)}
            </div>
            
            {/* Resolution indicator */}
            <div className={styles.resolutionIndicator}>
                {CANVAS_WIDTH} × {CANVAS_HEIGHT} • {Math.round(scale * 100)}%
            </div>
        </div>
    );
}

// Live clock component for clock elements
function ClockDisplay({ 
    fontSize, 
    fontFamily, 
    isPreviewMode 
}: { 
    fontSize: number; 
    fontFamily: string; 
    isPreviewMode: boolean;
}) {
    const [time, setTime] = useState(new Date());
    
    useEffect(() => {
        if (!isPreviewMode) return;
        const interval = setInterval(() => setTime(new Date()), 1000);
        return () => clearInterval(interval);
    }, [isPreviewMode]);
    
    const formatTime = (date: Date) => {
        return date.toLocaleTimeString('en-US', { 
            hour12: false,
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    };
    
    return (
        <div 
            style={{ 
                fontSize,
                fontFamily,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                height: '100%',
            }}
        >
            {isPreviewMode ? formatTime(time) : '12:00:00'}
        </div>
    );
}

export default CGCanvas;
