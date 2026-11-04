import React, { useEffect, useState, useRef } from 'react';
import { CGElement } from '../lib/types';
import styles from './CGOverlay.module.css';

// Old template element type (for backwards compatibility)
type CGTemplateElement = {
    id: string;
    templateId: string;
    templateUrl: string;
    values: Record<string, string>;
    position: CGElement['position'];
    isVisible: boolean;
};

// New CGLayer type (from CGLivePanel)
export interface CGLayerState {
    id: string;
    type: 'element' | 'template';
    element?: CGElement;
    templateId?: string;
    templateUrl?: string;
    values?: Record<string, string>;
    isVisible: boolean;
    transition: 'cut' | 'fade' | 'wipe_left' | 'wipe_right' | 'push_up' | 'push_down';
    transitionDuration: number;
    layer: number;
}

interface CGOverlayProps {
    elements: CGElement[];
    isPreview?: boolean;
    templateElements?: CGTemplateElement[];
    cgLayers?: CGLayerState[];
}

export function CGOverlay({ 
    elements, 
    isPreview = false, 
    templateElements = [],
    cgLayers = []
}: CGOverlayProps) {
    // Only render visible elements
    const visibleElements = elements.filter(el => el.isVisible);
    const visibleTemplates = templateElements.filter(el => el.isVisible);
    const visibleLayers = cgLayers.filter(l => l.isVisible).sort((a, b) => a.layer - b.layer);

    return (
        <div className={`${styles.overlay} ${isPreview ? styles.preview : ''}`}>
            {/* Render traditional CG elements */}
            {visibleElements.map(element => (
                <CGElementRenderer key={element.id} element={element} />
            ))}
            {/* Render HTML template-based CG elements (legacy) */}
            {visibleTemplates.map(template => (
                <CGTemplateRenderer key={template.id} template={template} />
            ))}
            {/* Render new CG Layers (templates and elements with transitions) */}
            {visibleLayers.map(layer => (
                <CGLayerRenderer key={layer.id} layer={layer} />
            ))}
        </div>
    );
}

function CGElementRenderer({ element }: { element: CGElement }) {
    const [animState, setAnimState] = useState<'entering' | 'visible' | 'looping' | 'exiting'>('entering');

    useEffect(() => {
        // Entrance animation
        const enterTimer = setTimeout(() => {
            setAnimState('looping');
        }, element.animationDuration);

        return () => clearTimeout(enterTimer);
    }, [element.animationDuration]);

    // Build inline styles: fixed % size so element scales with viewport; height (not minHeight) keeps box exact
    const positionStyle: React.CSSProperties = {
        position: 'absolute',
        left: element.position.anchor.includes('left') ? `${element.position.x}%` :
            element.position.anchor.includes('right') ? 'auto' : `${element.position.x}%`,
        right: element.position.anchor.includes('right') ? `${100 - element.position.x - element.position.width}%` : 'auto',
        top: element.position.anchor.includes('top') ? `${element.position.y}%` :
            element.position.anchor.includes('bottom') ? 'auto' : `${element.position.y}%`,
        bottom: element.position.anchor.includes('bottom') ? `${100 - element.position.y - element.position.height}%` : 'auto',
        width: `${element.position.width}%`,
        height: `${element.position.height}%`,
        overflow: 'hidden',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
    };

    const colorStyle: React.CSSProperties = {
        background: element.colors.gradient
            ? `linear-gradient(${element.colors.gradientDirection === 'vertical' ? '180deg' :
                element.colors.gradientDirection === 'diagonal' ? '135deg' : '90deg'}, 
               ${element.colors.primary}, ${element.colors.secondary})`
            : element.colors.primary,
        color: element.colors.text,
    };

    // Glass morphism effect
    const hasGlassMorphism = element.effects.glassMorphism || false;
    
    const effectStyle: React.CSSProperties = {
        borderRadius: element.effects.borderRadius,
        opacity: element.effects.opacity,
        boxShadow: [
            element.effects.shadow ? `0 4px 20px ${element.effects.shadowColor || '#000000'}40` : '',
            element.effects.glow ? `0 0 ${element.effects.glowIntensity || 10}px ${element.effects.glowColor || element.colors.primary}` : '',
        ].filter(Boolean).join(', ') || 'none',
        ...(hasGlassMorphism && {
            background: element.colors.primary.includes('rgba') 
                ? element.colors.primary 
                : `${element.colors.primary}80`,
            backdropFilter: 'blur(20px) saturate(180%)',
            WebkitBackdropFilter: 'blur(20px) saturate(180%)',
            border: '1px solid rgba(255, 255, 255, 0.2)',
        }),
    };

    // Animation classes
    const animClass = animState === 'entering' ? styles[`anim_${element.animationIn}`] :
        animState === 'looping' && element.loop !== 'none' ? styles[`loop_${element.loop}`] : '';

    // Scale font to element size: base from design, clamp to fit viewport (element.position.height % of 100vh)
    const heightVh = element.position.height;
    const scaledFontPx = Math.min(element.fontSize || 32, (heightVh * 0.35)); // ~3.5% of viewport height per 10% element height
    const fontSize = heightVh <= 0 ? (element.fontSize || 32) : `clamp(10px, ${scaledFontPx}vh, ${element.fontSize || 48}px)`;

    const combinedStyle: React.CSSProperties = {
        ...positionStyle,
        ...colorStyle,
        ...effectStyle,
        fontSize,
        fontFamily: element.fontFamily || 'inherit',
        animationDuration: `${element.animationDuration}ms`,
    };

    // Render based on type
    const renderContent = () => {
        switch (element.type) {
            case 'lower_third':
                return (
                    <div className={styles.lowerThird}>
                        <div className={styles.ltTitle}>{element.title}</div>
                        {element.subtitle && <div className={styles.ltSubtitle}>{element.subtitle}</div>}
                    </div>
                );
            case 'ticker':
                return (
                    <div className={styles.ticker}>
                        <span className={styles.tickerLabel}>{element.title}</span>
                        <div className={styles.tickerScroll}>
                            <span className={styles.tickerText}>{element.tickerText}</span>
                        </div>
                    </div>
                );
            case 'bug':
                return (
                    <div className={styles.bug}>
                        {element.imageSrc ? (
                            <img src={element.imageSrc} alt={element.title} className={styles.bugImage} />
                        ) : (
                            <span className={styles.bugText}>{element.title}</span>
                        )}
                    </div>
                );
            case 'clock':
                return <LiveClock colors={element.colors} fontSize={fontSize} />;
            case 'weather':
                return (
                    <div className={styles.weather}>
                        <span className={styles.weatherIcon}>☀️</span>
                        <span className={styles.weatherTemp}>{element.title}</span>
                    </div>
                );
            default:
                return <div className={styles.custom}>{element.title}</div>;
        }
    };

    return (
        <div
            className={`${styles.element} ${styles[element.type]} ${animClass}`}
            style={combinedStyle}
            data-cg-id={element.id}
        >
            {renderContent()}
        </div>
    );
}

// Live Clock Sub-component
function LiveClock({ colors, fontSize }: { colors: CGElement['colors']; fontSize?: number | string }) {
    const [time, setTime] = useState('');

    useEffect(() => {
        const update = () => {
            setTime(new Date().toLocaleTimeString('en-US', {
                hour12: false,
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit'
            }));
        };
        update();
        const timer = setInterval(update, 1000);
        return () => clearInterval(timer);
    }, []);

    return (
        <div className={styles.clock} style={{ color: colors.text, fontSize: fontSize ?? undefined }}>
            {time}
        </div>
    );
}

// Template-based CG Renderer (legacy)
function CGTemplateRenderer({ template }: { template: CGTemplateElement }) {
    const iframeRef = useRef<HTMLIFrameElement>(null);
    const [loaded, setLoaded] = useState(false);

    useEffect(() => {
        if (iframeRef.current && template.templateUrl) {
            iframeRef.current.src = template.templateUrl;
            setLoaded(true);
        }
    }, [template.templateUrl]);

    const positionStyle: React.CSSProperties = {
        position: 'absolute',
        left: template.position.anchor.includes('left') ? `${template.position.x}%` :
            template.position.anchor.includes('right') ? 'auto' : `${template.position.x}%`,
        right: template.position.anchor.includes('right') ? `${100 - template.position.x - template.position.width}%` : 'auto',
        top: template.position.anchor.includes('top') ? `${template.position.y}%` :
            template.position.anchor.includes('bottom') ? 'auto' : `${template.position.y}%`,
        bottom: template.position.anchor.includes('bottom') ? `${100 - template.position.y - template.position.height}%` : 'auto',
        width: `${template.position.width}%`,
        height: `${template.position.height}%`,
        pointerEvents: 'none', // Allow clicks to pass through
    };

    return (
        <div
            className={styles.templateContainer}
            style={positionStyle}
            data-cg-template-id={template.id}
        >
            <iframe
                ref={iframeRef}
                className={styles.templateIframe}
                title={`CG Template ${template.templateId}`}
                style={{
                    width: '100%',
                    height: '100%',
                    border: 'none',
                    background: 'transparent',
                }}
                sandbox="allow-scripts allow-same-origin"
            />
        </div>
    );
}

// New CG Layer Renderer - supports both elements and templates with transitions
function CGLayerRenderer({ layer }: { layer: CGLayerState }) {
    const iframeRef = useRef<HTMLIFrameElement>(null);
    const [animState, setAnimState] = useState<'entering' | 'visible' | 'exiting'>('entering');

    // Handle entrance animation
    useEffect(() => {
        setAnimState('entering');
        const enterTimer = setTimeout(() => {
            setAnimState('visible');
        }, layer.transitionDuration);
        return () => clearTimeout(enterTimer);
    }, [layer.id, layer.transitionDuration]);

    // Get transition class based on type
    const getTransitionClass = () => {
        if (animState !== 'entering') return '';
        switch (layer.transition) {
            case 'fade': return styles.transitionFade;
            case 'wipe_left': return styles.transitionWipeLeft;
            case 'wipe_right': return styles.transitionWipeRight;
            case 'push_up': return styles.transitionPushUp;
            case 'push_down': return styles.transitionPushDown;
            default: return '';
        }
    };

    const layerStyle: React.CSSProperties = {
        position: 'absolute',
        inset: 0,
        zIndex: layer.layer * 10,
        pointerEvents: 'none',
        animationDuration: `${layer.transitionDuration}ms`,
    };

    // Render template-based CG
    if (layer.type === 'template' && layer.templateUrl) {
        return (
            <div
                className={`${styles.cgLayer} ${getTransitionClass()}`}
                style={layerStyle}
                data-cg-layer={layer.layer}
            >
                <iframe
                    ref={iframeRef}
                    src={layer.templateUrl}
                    className={styles.layerIframe}
                    title={`CG Layer ${layer.layer}`}
                    style={{
                        width: '100%',
                        height: '100%',
                        border: 'none',
                        background: 'transparent',
                    }}
                    sandbox="allow-scripts allow-same-origin"
                />
            </div>
        );
    }

    // Render element-based CG
    if (layer.type === 'element' && layer.element) {
        return (
            <div
                className={`${styles.cgLayer} ${getTransitionClass()}`}
                style={layerStyle}
                data-cg-layer={layer.layer}
            >
                <CGElementRenderer element={layer.element} />
            </div>
        );
    }

    return null;
}

// Export a hook for CG control
export function useCGController(initialElements: CGElement[] = []) {
    const [elements, setElements] = useState<CGElement[]>(initialElements);

    const showElement = (id: string) => {
        setElements(prev => prev.map(el =>
            el.id === id ? { ...el, isVisible: true } : el
        ));
    };

    const hideElement = (id: string) => {
        setElements(prev => prev.map(el =>
            el.id === id ? { ...el, isVisible: false } : el
        ));
    };

    const toggleElement = (id: string) => {
        setElements(prev => prev.map(el =>
            el.id === id ? { ...el, isVisible: !el.isVisible } : el
        ));
    };

    const updateElement = (id: string, updates: Partial<CGElement>) => {
        setElements(prev => prev.map(el =>
            el.id === id ? { ...el, ...updates } : el
        ));
    };

    const addElement = (element: CGElement) => {
        setElements(prev => [...prev, element]);
    };

    const removeElement = (id: string) => {
        setElements(prev => prev.filter(el => el.id !== id));
    };

    const hideAll = () => {
        setElements(prev => prev.map(el => ({ ...el, isVisible: false })));
    };

    return {
        elements,
        setElements,
        showElement,
        hideElement,
        toggleElement,
        updateElement,
        addElement,
        removeElement,
        hideAll,
    };
}
