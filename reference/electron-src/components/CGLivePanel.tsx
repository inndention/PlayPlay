import React, { useEffect, useMemo, useSyncExternalStore } from 'react';
import {
    ExternalLink,
    Layers,
    Palette,
    RefreshCw,
    Send,
    Settings2,
    Square,
    Tv,
    Type,
} from 'lucide-react';
import { useToast } from './Toast';
import { ContextMenu, useContextMenu } from './ContextMenu';
import { buildCGLayerMenuItems } from '../lib/contextMenuActions';
import { executeContextMenuAction } from '../lib/contextMenuRegistry';
import { dbLogAnalyticsEvent } from '../lib/mediaDbClient';
import {
    buildTemplateQuery,
    buildTemplateUrl,
    cloneLayerForChannel,
    createDefaultCoreFields,
    createElementDraftLayer,
    createElementFromLayer,
    createTemplateDraftLayer,
    normalizeTemplateToSource,
} from '../lib/cgUnified';
import type {
    CGElement,
    CGTemplate,
    UnifiedCGCoreFields,
    UnifiedCGLayerState,
    UnifiedCGSourceTemplate,
    UnifiedCGStateSnapshot,
} from '../lib/types';
import styles from './CGLivePanel.module.css';
import { CGThemeManager } from './CGThemeManager';
import { ANIMATION_PRESETS } from '../lib/cgTemplates';

type CGStoreState = UnifiedCGStateSnapshot & {
    draftLayer: UnifiedCGLayerState | null;
    initialized: boolean;
    urls: {
        previewApi: string;
        programApi: string;
        cgPreview: string;
        cgProgram: string;
        cgOutput: string;
        templateBase: string;
    };
};

const defaultUrls = {
    previewApi: 'http://localhost:8080/api/cg-preview-state',
    programApi: 'http://localhost:8080/api/cg-program-state',
    cgPreview: 'http://localhost:8080/cg-preview',
    cgProgram: 'http://localhost:8080/cg-program',
    cgOutput: 'http://localhost:8080/cg-output',
    templateBase: 'http://localhost:8080',
};

let storeState: CGStoreState = {
    previewLayers: [],
    programLayers: [],
    selectedLayer: 1,
    selectedLayerId: undefined,
    selectedTemplate: null,
    draftLayer: null,
    initialized: false,
    urls: defaultUrls,
};

const listeners = new Set<() => void>();
let initPromise: Promise<void> | null = null;

function emitStoreChange() {
    listeners.forEach((listener) => listener());
}

function setStoreState(next: Partial<CGStoreState>) {
    storeState = { ...storeState, ...next };
    emitStoreChange();
}

function subscribe(listener: () => void) {
    listeners.add(listener);
    return () => {
        listeners.delete(listener);
    };
}

function getState() {
    return storeState;
}

async function resolveCgUrls() {
    const api = (window as Window & { electronAPI?: any }).electronAPI;
    if (api?.getAllUrls) {
        try {
            const result = await api.getAllUrls();
            if (result?.urls) {
                return {
                    previewApi: `${result.urls.cgPreview.replace(/\/$/, '')}`.replace('/cg-preview', '/api/cg-preview-state'),
                    programApi: `${result.urls.cgProgram.replace(/\/$/, '')}`.replace('/cg-program', '/api/cg-program-state'),
                    cgPreview: result.urls.cgPreview || defaultUrls.cgPreview,
                    cgProgram: result.urls.cgProgram || defaultUrls.cgProgram,
                    cgOutput: result.urls.cgOutput || defaultUrls.cgOutput,
                    templateBase: result.urls.cgProgram
                        ? result.urls.cgProgram.replace(/\/cg-program$/, '')
                        : defaultUrls.templateBase,
                };
            }
        } catch (_) {
            // Fall through to browser defaults.
        }
    }

    if (
        typeof window !== 'undefined' &&
        window.location?.origin &&
        window.location.origin.startsWith('http') &&
        window.location.port !== '5173'
    ) {
        return {
            previewApi: `${window.location.origin}/api/cg-preview-state`,
            programApi: `${window.location.origin}/api/cg-program-state`,
            cgPreview: `${window.location.origin}/#/cg-preview`,
            cgProgram: `${window.location.origin}/#/cg-program`,
            cgOutput: `${window.location.origin}/#/cg-output`,
            templateBase: window.location.origin,
        };
    }

    return defaultUrls;
}

async function fetchChannelState(url: string): Promise<UnifiedCGLayerState[]> {
    try {
        const response = await fetch(url);
        if (!response.ok) {
            return [];
        }
        const data = await response.json();
        return Array.isArray(data?.layers) ? data.layers : [];
    } catch (_) {
        return [];
    }
}

function mergeDraftFromLayer(layer: UnifiedCGLayerState) {
    return {
        ...cloneLayerForChannel(layer, 'preview'),
        channel: 'preview' as const,
    };
}

function ensureDraftLayer(currentState: CGStoreState): UnifiedCGLayerState {
    if (currentState.draftLayer) {
        return currentState.draftLayer;
    }

    const fromPreview = currentState.previewLayers.find((layer) => layer.layer === currentState.selectedLayer);
    if (fromPreview) {
        return mergeDraftFromLayer(fromPreview);
    }

    const fromProgram = currentState.programLayers.find((layer) => layer.layer === currentState.selectedLayer);
    if (fromProgram) {
        return mergeDraftFromLayer(fromProgram);
    }

    return createElementDraftLayer({ layer: currentState.selectedLayer });
}

function upsertLayer(layers: UnifiedCGLayerState[], nextLayer: UnifiedCGLayerState) {
    const existingIndex = layers.findIndex((layer) => layer.id === nextLayer.id || layer.layer === nextLayer.layer);
    if (existingIndex === -1) {
        return [...layers, nextLayer].sort((left, right) => left.layer - right.layer);
    }

    const nextLayers = [...layers];
    nextLayers[existingIndex] = nextLayer;
    return nextLayers.sort((left, right) => left.layer - right.layer);
}

function removeLayer(layers: UnifiedCGLayerState[], layerId?: string, layerNumber?: number) {
    return layers.filter((layer) => layer.id !== layerId && layer.layer !== layerNumber);
}

function buildTemplateSourceFromLayer(layer: UnifiedCGLayerState): UnifiedCGSourceTemplate | null {
    if (layer.type !== 'template' || !layer.templateId) {
        return null;
    }

    return {
        templateId: layer.templateId,
        templateName: layer.displayName || layer.templateId,
        category: layer.templateCategory || 'lower_third',
        defaults: {
            ...(layer.advancedValues || {}),
        },
        fields: layer.fieldDefinitions || [],
        fieldMap: undefined,
    };
}

function resolveDraftLayer(layer: UnifiedCGLayerState, urls: CGStoreState['urls']): UnifiedCGLayerState {
    if (layer.type === 'template' && layer.templateId) {
        const source = storeState.selectedTemplate || buildTemplateSourceFromLayer(layer);
        const core = createDefaultCoreFields(layer.core || {});
        const query = source
            ? buildTemplateQuery(source, core, layer.advancedValues || {})
            : new URLSearchParams(layer.advancedValues || {});

        return {
            ...layer,
            channel: 'preview',
            core,
            templateUrl: buildTemplateUrl(urls.templateBase, layer.templateId, query),
            isVisible: true,
            element: undefined,
        };
    }

    return {
        ...layer,
        channel: 'preview',
        core: createDefaultCoreFields(layer.core || {}),
        element: createElementFromLayer(layer),
        templateUrl: undefined,
        isVisible: true,
    };
}

async function postChannelState(
    channel: 'preview' | 'program',
    layers: UnifiedCGLayerState[],
    action: string,
    metadata: Record<string, unknown> = {},
) {
    const target = channel === 'preview' ? storeState.urls.previewApi : storeState.urls.programApi;
    try {
        await fetch(target, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                action,
                layers,
                ...metadata,
            }),
        });
    } catch (_) {
        // The local browser source server may not be active yet.
    }
}

function buildLayerAnalyticsMetadata(layer?: UnifiedCGLayerState | null) {
    return {
        layerId: layer?.id,
        layerNumber: layer?.layer,
        templateId: layer?.templateId,
        entityLabel: layer?.displayName || layer?.templateId || layer?.element?.name || (typeof layer?.layer === 'number' ? `Layer ${layer.layer}` : undefined),
    };
}

async function initializeStore() {
    if (storeState.initialized) {
        return;
    }
    if (!initPromise) {
        initPromise = (async () => {
            const urls = await resolveCgUrls();
            const [previewLayers, programLayers] = await Promise.all([
                fetchChannelState(urls.previewApi),
                fetchChannelState(urls.programApi),
            ]);

            setStoreState({
                urls,
                previewLayers,
                programLayers,
                draftLayer: previewLayers[0]
                    ? mergeDraftFromLayer(previewLayers[0])
                    : programLayers[0]
                        ? mergeDraftFromLayer(programLayers[0])
                        : createElementDraftLayer({ layer: 1 }),
                selectedLayer: previewLayers[0]?.layer || programLayers[0]?.layer || 1,
                selectedLayerId: previewLayers[0]?.id || programLayers[0]?.id,
                selectedTemplate: previewLayers[0]?.type === 'template'
                    ? buildTemplateSourceFromLayer(previewLayers[0])
                    : programLayers[0]?.type === 'template'
                        ? buildTemplateSourceFromLayer(programLayers[0])
                        : null,
                initialized: true,
            });
        })().finally(() => {
            initPromise = null;
        });
    }

    return initPromise;
}

export const cgStateManager = {
    subscribe,
    getState,
    init: initializeStore,
    async selectTemplate(template: CGTemplate | UnifiedCGSourceTemplate) {
        await initializeStore();

        const source = 'parameters' in template
            ? normalizeTemplateToSource(template)
            : template;

        const draft = storeState.draftLayer && storeState.draftLayer.type === 'template' && storeState.draftLayer.templateId === source.templateId
            ? {
                ...storeState.draftLayer,
                displayName: source.templateName,
                templateCategory: source.category,
                fieldDefinitions: source.fields,
                advancedValues: {
                    ...Object.fromEntries(source.fields.map((field) => [field.key, source.defaults[field.key] || field.defaultValue || ''])),
                    ...(storeState.draftLayer.advancedValues || {}),
                },
            }
            : createTemplateDraftLayer(source, {
                id: storeState.selectedLayerId || storeState.draftLayer?.id,
                layer: storeState.selectedLayer,
                transition: storeState.draftLayer?.transition,
                transitionDuration: storeState.draftLayer?.transitionDuration,
            });

        setStoreState({
            selectedTemplate: source,
            draftLayer: draft,
            selectedLayerId: draft.id,
        });

        await dbLogAnalyticsEvent({
            family: 'cg',
            action: 'template_selected',
            entityType: 'cg_template',
            entityId: source.templateId,
            entityLabel: source.templateName,
            payload: { category: source.category },
        });
    },
    async createElementLayer(name = 'Element CG') {
        await initializeStore();
        const draft = createElementDraftLayer({
            id: storeState.selectedLayerId || storeState.draftLayer?.id,
            layer: storeState.selectedLayer,
            displayName: name,
            transition: storeState.draftLayer?.transition,
            transitionDuration: storeState.draftLayer?.transitionDuration,
        });

        setStoreState({
            draftLayer: draft,
            selectedTemplate: null,
            selectedLayerId: draft.id,
        });
    },
    async selectLayer(layerId: string, channel: 'preview' | 'program') {
        await initializeStore();
        const collection = channel === 'preview' ? storeState.previewLayers : storeState.programLayers;
        const layer = collection.find((item) => item.id === layerId);
        if (!layer) {
            return;
        }

        setStoreState({
            selectedLayer: layer.layer,
            selectedLayerId: layer.id,
            selectedTemplate: layer.type === 'template' ? buildTemplateSourceFromLayer(layer) : null,
            draftLayer: mergeDraftFromLayer(layer),
        });
    },
    async setSelectedLayerNumber(layerNumber: number) {
        await initializeStore();
        const matchingPreview = storeState.previewLayers.find((layer) => layer.layer === layerNumber);
        const matchingProgram = storeState.programLayers.find((layer) => layer.layer === layerNumber);
        const nextDraft = matchingPreview
            ? mergeDraftFromLayer(matchingPreview)
            : matchingProgram
                ? mergeDraftFromLayer(matchingProgram)
                : createElementDraftLayer({ layer: layerNumber });

        setStoreState({
            selectedLayer: layerNumber,
            selectedLayerId: nextDraft.id,
            selectedTemplate: nextDraft.type === 'template' ? buildTemplateSourceFromLayer(nextDraft) : null,
            draftLayer: nextDraft,
        });
    },
    async updateDraftCore<K extends keyof UnifiedCGCoreFields>(key: K, value: UnifiedCGCoreFields[K]) {
        await initializeStore();
        const draft = ensureDraftLayer(storeState);
        setStoreState({
            draftLayer: {
                ...draft,
                core: {
                    ...createDefaultCoreFields(draft.core || {}),
                    [key]: value,
                },
            },
        });
    },
    async updateDraftAdvanced(key: string, value: string) {
        await initializeStore();
        const draft = ensureDraftLayer(storeState);
        setStoreState({
            draftLayer: {
                ...draft,
                advancedValues: {
                    ...(draft.advancedValues || {}),
                    [key]: value,
                },
            },
        });
    },
    async commitPreview(action = 'preview_updated') {
        await initializeStore();
        const resolved = resolveDraftLayer(ensureDraftLayer(storeState), storeState.urls);
        const nextPreview = upsertLayer(storeState.previewLayers, resolved);

        setStoreState({
            previewLayers: nextPreview,
            draftLayer: resolved,
            selectedLayer: resolved.layer,
            selectedLayerId: resolved.id,
        });

        await postChannelState('preview', nextPreview, action, buildLayerAnalyticsMetadata(resolved));
        return resolved;
    },
    async takeToProgram() {
        const resolved = await this.commitPreview('preview_updated');
        const nextProgram = storeState.previewLayers.map((layer) => cloneLayerForChannel(layer, 'program'));
        const resolvedProgram = cloneLayerForChannel(resolved, 'program');

        setStoreState({
            programLayers: nextProgram,
            selectedLayer: resolved.layer,
            selectedLayerId: resolved.id,
        });

        await postChannelState('program', nextProgram, 'take_to_program', buildLayerAnalyticsMetadata(resolvedProgram));
    },
    async updateProgram() {
        const resolvedPreview = await this.commitPreview('preview_updated');
        const resolvedProgram = cloneLayerForChannel(resolvedPreview, 'program');
        const nextProgram = upsertLayer(storeState.programLayers, resolvedProgram);

        setStoreState({
            programLayers: nextProgram,
            selectedLayer: resolvedProgram.layer,
            selectedLayerId: resolvedProgram.id,
        });

        await postChannelState('program', nextProgram, 'update_program', buildLayerAnalyticsMetadata(resolvedProgram));
    },
    async clearPreview(layerId = storeState.selectedLayerId) {
        await initializeStore();
        const targetLayer = storeState.previewLayers.find((layer) => layer.id === layerId)
            || storeState.previewLayers.find((layer) => layer.layer === storeState.selectedLayer)
            || (storeState.draftLayer?.id === layerId || storeState.draftLayer?.layer === storeState.selectedLayer
                ? storeState.draftLayer
                : null);
        const nextPreview = removeLayer(storeState.previewLayers, layerId, storeState.selectedLayer);
        setStoreState({
            previewLayers: nextPreview,
        });
        await postChannelState('preview', nextPreview, 'clear_preview', buildLayerAnalyticsMetadata(targetLayer));
    },
    async clearProgram(layerId = storeState.selectedLayerId) {
        await initializeStore();
        const targetLayer = storeState.programLayers.find((layer) => layer.id === layerId)
            || storeState.programLayers.find((layer) => layer.layer === storeState.selectedLayer)
            || (storeState.draftLayer?.id === layerId || storeState.draftLayer?.layer === storeState.selectedLayer
                ? storeState.draftLayer
                : null);
        const nextProgram = removeLayer(storeState.programLayers, layerId, storeState.selectedLayer);
        setStoreState({
            programLayers: nextProgram,
        });
        await postChannelState('program', nextProgram, 'clear_program', buildLayerAnalyticsMetadata(targetLayer));
    },
};

export function useCGState() {
    const state = useSyncExternalStore(cgStateManager.subscribe, cgStateManager.getState, cgStateManager.getState);

    useEffect(() => {
        void cgStateManager.init();
    }, []);

    return {
        ...state,
        selectTemplate: cgStateManager.selectTemplate.bind(cgStateManager),
        createElementLayer: cgStateManager.createElementLayer.bind(cgStateManager),
        selectLayer: cgStateManager.selectLayer.bind(cgStateManager),
        setSelectedLayerNumber: cgStateManager.setSelectedLayerNumber.bind(cgStateManager),
        updateDraftCore: cgStateManager.updateDraftCore.bind(cgStateManager),
        updateDraftAdvanced: cgStateManager.updateDraftAdvanced.bind(cgStateManager),
        commitPreview: cgStateManager.commitPreview.bind(cgStateManager),
        takeToProgram: cgStateManager.takeToProgram.bind(cgStateManager),
        updateProgram: cgStateManager.updateProgram.bind(cgStateManager),
        clearPreview: cgStateManager.clearPreview.bind(cgStateManager),
        clearProgram: cgStateManager.clearProgram.bind(cgStateManager),
        clearLayer: async (layerNumber: number) => {
            const layer = state.programLayers.find((item) => item.layer === layerNumber) || state.previewLayers.find((item) => item.layer === layerNumber);
            if (layer) {
                await cgStateManager.clearPreview(layer.id);
                await cgStateManager.clearProgram(layer.id);
            }
        },
        clearAll: async () => {
            setStoreState({ previewLayers: [], programLayers: [] });
            await Promise.all([
                postChannelState('preview', [], 'clear_preview'),
                postChannelState('program', [], 'clear_program'),
            ]);
        },
        layers: state.programLayers,
    };
}

function formatLayerLabel(layer: UnifiedCGLayerState) {
    return `L${layer.layer} · ${layer.displayName || layer.templateId || layer.element?.name || 'Untitled'}`;
}

function buildLegacyElementDraft(element: CGElement, layer: number): UnifiedCGLayerState {
    const draft = createElementDraftLayer({
        layer,
        displayName: element.name,
        sourceElementId: element.id,
    });

    return {
        ...draft,
        core: createDefaultCoreFields({
            text1: element.title || '',
            text2: element.subtitle || '',
            text3: element.tickerText || '',
            style1: element.colors?.primary || '#0f172a',
            style2: element.colors?.secondary || '#2563eb',
            style3: element.colors?.text || '#ffffff',
            animationIn: element.animationIn || 'fade',
            animationOut: element.animationOut || 'fade',
            animationLoop: element.loop || 'none',
            anchorPreset: element.type === 'ticker'
                ? 'bottom-ticker'
                : element.type === 'bug'
                    ? 'bug-top-right'
                    : 'lower-third',
            margin: 4,
            width: element.position?.width || 42,
            height: element.position?.height || 14,
        }),
    };
}

interface CGLivePanelProps {
    legacyElements?: CGElement[];
    onOpenAdvancedTools?: () => void;
    onFocusAnalytics?: (payload?: Record<string, unknown>) => void;
}

export function CGLivePanel({
    legacyElements = [],
    onOpenAdvancedTools,
    onFocusAnalytics,
}: CGLivePanelProps) {
    const {
        previewLayers,
        programLayers,
        selectedLayer,
        selectedLayerId,
        selectedTemplate,
        draftLayer,
        urls,
        setSelectedLayerNumber,
        updateDraftCore,
        updateDraftAdvanced,
        takeToProgram,
        updateProgram,
        commitPreview,
        clearPreview,
        clearProgram,
        createElementLayer,
        selectLayer,
    } = useCGState();
    const { showToast } = useToast();
    const { menuState, showMenu, hideMenu } = useContextMenu();

    useEffect(() => {
        const handleMessage = (event: MessageEvent) => {
            const payload = event.data;
            if (payload?.type === 'playplay:cg-template-selected' && payload.template) {
                void cgStateManager.selectTemplate(payload.template as UnifiedCGSourceTemplate);
            }
        };

        window.addEventListener('message', handleMessage);
        return () => window.removeEventListener('message', handleMessage);
    }, []);

    const previewLabel = useMemo(
        () => previewLayers.find((layer) => layer.id === selectedLayerId) || previewLayers.find((layer) => layer.layer === selectedLayer),
        [previewLayers, selectedLayer, selectedLayerId],
    );
    const programLabel = useMemo(
        () => programLayers.find((layer) => layer.id === selectedLayerId) || programLayers.find((layer) => layer.layer === selectedLayer),
        [programLayers, selectedLayer, selectedLayerId],
    );

    const handleLayerMenu = (event: React.MouseEvent, layer: UnifiedCGLayerState, channel: 'preview' | 'program') => {
        showMenu(event, buildCGLayerMenuItems(layer, channel));
    };

    const handleMenuAction = async (actionId: any, payload?: Record<string, unknown>) => {
        await executeContextMenuAction(actionId, payload, {
            onTakeToProgram: async () => {
                await takeToProgram();
                showToast('Preview taken to program', 'success');
            },
            onUpdateProgram: async () => {
                await updateProgram();
                showToast('Program layer updated', 'success');
            },
            onClearPreview: async (nextPayload) => {
                await clearPreview(String(nextPayload?.layerId || selectedLayerId || ''));
                showToast('Preview layer cleared', 'info');
            },
            onClearProgram: async (nextPayload) => {
                await clearProgram(String(nextPayload?.layerId || selectedLayerId || ''));
                showToast('Program layer cleared', 'info');
            },
            onViewAnalytics: async (nextPayload) => {
                onFocusAnalytics?.(nextPayload);
            },
        });
    };

    const draftCore = createDefaultCoreFields(draftLayer?.core || {});
    const advancedFields = draftLayer?.fieldDefinitions || [];
    const advancedValues = draftLayer?.advancedValues || {};

    return (
        <div className={styles.panel}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <Layers size={16} />
                    <span>Live CG Console</span>
                </div>
                <div className={styles.headerActions}>
                    <a className={styles.linkButton} href={urls.cgPreview} target="_blank" rel="noreferrer">
                        <Tv size={14} />
                        Preview
                    </a>
                    <a className={styles.linkButton} href={urls.cgOutput} target="_blank" rel="noreferrer">
                        <ExternalLink size={14} />
                        Program
                    </a>
                    <button
                        type="button"
                        className={styles.settingsBtn}
                        onClick={onOpenAdvancedTools}
                        title="Advanced CG tools"
                    >
                        <Settings2 size={14} />
                    </button>
                </div>
            </div>

            <div className={styles.workspace}>
                <div className={styles.layerRail}>
                    <div className={styles.sectionLabel}>Layers</div>
                    <div className={styles.layerButtons}>
                        {[1, 2, 3, 4, 5].map((layerNumber) => (
                            <button
                                key={layerNumber}
                                type="button"
                                className={`${styles.layerButton} ${selectedLayer === layerNumber ? styles.layerButtonActive : ''}`}
                                onClick={() => void setSelectedLayerNumber(layerNumber)}
                            >
                                L{layerNumber}
                            </button>
                        ))}
                    </div>

                    <div className={styles.channelColumn}>
                        <div className={styles.channelHeader}>
                            <span>Preview</span>
                            <button type="button" className={styles.channelAction} onClick={() => void commitPreview()}>
                                <RefreshCw size={13} />
                            </button>
                        </div>
                        <div className={styles.layerList}>
                            {previewLayers.map((layer) => (
                                <button
                                    key={`preview-${layer.id}`}
                                    type="button"
                                    className={`${styles.layerCard} ${selectedLayerId === layer.id ? styles.layerCardActive : ''}`}
                                    onClick={() => void selectLayer(layer.id, 'preview')}
                                    onContextMenu={(event) => handleLayerMenu(event, layer, 'preview')}
                                >
                                    <span>{formatLayerLabel(layer)}</span>
                                </button>
                            ))}
                            {previewLayers.length === 0 && <div className={styles.emptyState}>Preview is clear</div>}
                        </div>
                    </div>

                    <div className={styles.channelColumn}>
                        <div className={styles.channelHeader}>
                            <span>Program</span>
                            <button type="button" className={styles.channelAction} onClick={() => void updateProgram()}>
                                <Send size={13} />
                            </button>
                        </div>
                        <div className={styles.layerList}>
                            {programLayers.map((layer) => (
                                <button
                                    key={`program-${layer.id}`}
                                    type="button"
                                    className={`${styles.layerCard} ${selectedLayerId === layer.id ? styles.layerCardActive : ''}`}
                                    onClick={() => void selectLayer(layer.id, 'program')}
                                    onContextMenu={(event) => handleLayerMenu(event, layer, 'program')}
                                >
                                    <span>{formatLayerLabel(layer)}</span>
                                </button>
                            ))}
                            {programLayers.length === 0 && <div className={styles.emptyState}>Program is clear</div>}
                        </div>
                    </div>
                </div>

                <div className={styles.editorColumn}>
                    <div className={styles.editorHeader}>
                        <div>
                            <div className={styles.sectionLabel}>Source</div>
                            <div className={styles.sourceTitle}>
                                {selectedTemplate?.templateName || draftLayer?.displayName || 'Element CG'}
                            </div>
                        </div>
                        <div className={styles.editorActions}>
                            <button
                                type="button"
                                className={styles.secondaryButton}
                                onClick={() => void createElementLayer()}
                            >
                                <Type size={14} />
                                Blank Element
                            </button>
                        </div>
                    </div>

                    {legacyElements.length > 0 && (
                        <div className={styles.legacyStrip}>
                            <span className={styles.sectionLabel}>Existing Elements</span>
                            <div className={styles.legacyButtons}>
                                {legacyElements.slice(0, 6).map((element) => (
                                    <button
                                        key={element.id}
                                        type="button"
                                        className={styles.legacyButton}
                                        onClick={() => {
                                            setStoreState({
                                                selectedLayerId: selectedLayerId || draftLayer?.id || element.id,
                                                draftLayer: buildLegacyElementDraft(element, selectedLayer),
                                                selectedTemplate: null,
                                            });
                                        }}
                                    >
                                        {element.name}
                                    </button>
                                ))}
                            </div>
                        </div>
                    )}

                    <div className={styles.formGrid}>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Text 1</span>
                            <input value={draftCore.text1} onChange={(event) => void updateDraftCore('text1', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Text 2</span>
                            <input value={draftCore.text2} onChange={(event) => void updateDraftCore('text2', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Text 3</span>
                            <input value={draftCore.text3} onChange={(event) => void updateDraftCore('text3', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Style 1</span>
                            <input type="color" value={draftCore.style1} onChange={(event) => void updateDraftCore('style1', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Style 2</span>
                            <input type="color" value={draftCore.style2} onChange={(event) => void updateDraftCore('style2', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Style 3</span>
                            <input type="color" value={draftCore.style3} onChange={(event) => void updateDraftCore('style3', event.target.value)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Animation In</span>
                            <select value={draftCore.animationIn} onChange={(event) => void updateDraftCore('animationIn', event.target.value)}>
                                {Object.entries(ANIMATION_PRESETS).map(([id, p]) => (
                                    <option key={id} value={id}>{p.name}</option>
                                ))}
                            </select>
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Loop</span>
                            <select value={draftCore.animationLoop} onChange={(event) => void updateDraftCore('animationLoop', event.target.value)}>
                                <option value="none">None</option>
                                <option value="pulse">Pulse</option>
                                <option value="ticker_scroll">Ticker Scroll</option>
                                <option value="glow">Glow</option>
                            </select>
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Animation Out</span>
                            <select value={draftCore.animationOut} onChange={(event) => void updateDraftCore('animationOut', event.target.value)}>
                                {Object.entries(ANIMATION_PRESETS).map(([id, p]) => (
                                    <option key={id} value={id}>{p.name}</option>
                                ))}
                            </select>
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Anchor</span>
                            <select value={draftCore.anchorPreset} onChange={(event) => void updateDraftCore('anchorPreset', event.target.value as UnifiedCGCoreFields['anchorPreset'])}>
                                <option value="top-left">Top Left</option>
                                <option value="top-center">Top Center</option>
                                <option value="top-right">Top Right</option>
                                <option value="bottom-left">Bottom Left</option>
                                <option value="bottom-center">Bottom Center</option>
                                <option value="bottom-right">Bottom Right</option>
                                <option value="lower-third">Lower Third</option>
                                <option value="top-ticker">Top Ticker</option>
                                <option value="bottom-ticker">Bottom Ticker</option>
                                <option value="bug-top-left">Bug Top Left</option>
                                <option value="bug-top-right">Bug Top Right</option>
                                <option value="bug-bottom-left">Bug Bottom Left</option>
                                <option value="bug-bottom-right">Bug Bottom Right</option>
                            </select>
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Margin</span>
                            <input type="number" min={0} max={30} value={draftCore.margin} onChange={(event) => void updateDraftCore('margin', Number(event.target.value) || 0)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Width</span>
                            <input type="number" min={8} max={100} value={draftCore.width} onChange={(event) => void updateDraftCore('width', Number(event.target.value) || 8)} />
                        </label>
                        <label className={styles.field}>
                            <span className={styles.fieldLabel}>Height</span>
                            <input type="number" min={4} max={100} value={draftCore.height} onChange={(event) => void updateDraftCore('height', Number(event.target.value) || 4)} />
                        </label>
                    </div>

                    {advancedFields.length > 0 && (
                        <div className={styles.advancedBlock}>
                            <div className={styles.advancedHeader}>
                                <Palette size={14} />
                                <span>Advanced Extras</span>
                            </div>
                            <div className={styles.advancedGrid}>
                                {advancedFields.map((field) => (
                                    <label key={field.key} className={styles.field}>
                                        <span className={styles.fieldLabel}>{field.label}</span>
                                        {field.type === 'select' ? (
                                            <select
                                                value={advancedValues[field.key] || ''}
                                                onChange={(event) => void updateDraftAdvanced(field.key, event.target.value)}
                                            >
                                                <option value="">Default</option>
                                                {(field.options || []).map((option) => (
                                                    <option key={option} value={option}>
                                                        {option}
                                                    </option>
                                                ))}
                                            </select>
                                        ) : field.type === 'color' ? (
                                            <input
                                                type="color"
                                                value={advancedValues[field.key] || '#000000'}
                                                onChange={(event) => void updateDraftAdvanced(field.key, event.target.value)}
                                            />
                                        ) : (
                                            <input
                                                type={field.type === 'number' ? 'number' : 'text'}
                                                value={advancedValues[field.key] || ''}
                                                onChange={(event) => void updateDraftAdvanced(field.key, event.target.value)}
                                            />
                                        )}
                                    </label>
                                ))}
                            </div>
                        </div>
                    )}

                    <div className={styles.themeSection}>
                        <CGThemeManager />
                    </div>

                    <div className={styles.transportRow}>
                        <div className={styles.transportSummary}>
                            <span>Selected Preview</span>
                            <strong>{previewLabel ? formatLayerLabel(previewLabel) : `L${selectedLayer} ready`}</strong>
                        </div>
                        <div className={styles.transportSummary}>
                            <span>On Program</span>
                            <strong>{programLabel ? formatLayerLabel(programLabel) : 'Program clear'}</strong>
                        </div>
                    </div>

                    <div className={styles.actionRow}>
                        <button
                            type="button"
                            className={styles.secondaryButton}
                            onClick={() => void commitPreview().then(() => showToast('Preview updated', 'success'))}
                        >
                            <RefreshCw size={14} />
                            Update Preview
                        </button>
                        <button
                            type="button"
                            className={styles.primaryButton}
                            onClick={() => void takeToProgram().then(() => showToast('Taken to program', 'success'))}
                        >
                            <Send size={14} />
                            Take / Cut
                        </button>
                        <button
                            type="button"
                            className={styles.secondaryButton}
                            onClick={() => void updateProgram().then(() => showToast('Program updated', 'success'))}
                        >
                            <Tv size={14} />
                            Update Program
                        </button>
                        <button
                            type="button"
                            className={styles.destructiveButton}
                            onClick={() => void clearPreview().then(() => showToast('Preview cleared', 'info'))}
                        >
                            <Square size={14} />
                            Clear Preview
                        </button>
                        <button
                            type="button"
                            className={styles.destructiveButton}
                            onClick={() => void clearProgram().then(() => showToast('Program cleared', 'info'))}
                        >
                            <Square size={14} />
                            Clear Program
                        </button>
                    </div>
                </div>
            </div>

            {menuState.visible && (
                <ContextMenu
                    x={menuState.x}
                    y={menuState.y}
                    items={menuState.items}
                    onClose={hideMenu}
                    onAction={handleMenuAction}
                />
            )}
        </div>
    );
}
