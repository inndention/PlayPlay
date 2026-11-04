import React, { useState, useEffect, useRef } from 'react';
import { CGTemplate, CGTemplatePreset, CGHistoryItem, CGScheduledEvent } from '../lib/types';
import { loadCGTemplatePresets, saveCGTemplatePresets, createCGTemplatePreset, loadCGHistory, addCGHistoryItem, clearCGHistory } from '../lib/storage';
import { safePrompt } from '../lib/promptPolyfill';
import { getEnvironment, clearEnvironmentCache, waitForElectronAPI } from '../lib/fileResolver';
import { cgStateManager, useCGState } from '../components/CGLivePanel';
import { normalizeTemplateToSource } from '../lib/cgUnified';
import { 
    Search, Play, X, Save, Trash2, History, Settings, 
    ChevronRight, Image as ImageIcon, FileText, Sparkles, Upload, FolderOpen,
    Eye, EyeOff, Copy, Edit2, Plus, ArrowLeft, Link2, Check, Layers, Square, RefreshCw
} from 'lucide-react';
import styles from './CGTemplatesPage.module.css';

type Category = 'lower_third' | 'ticker' | 'bug' | 'fullscreen' | 'social' | 'weather' | 'banner';
type SidebarTab = 'templates' | 'presets' | 'history';

const CATEGORIES: { id: Category; label: string; icon: React.ReactNode }[] = [
    { id: 'lower_third', label: 'L3', icon: <FileText size={14} /> },
    { id: 'ticker', label: 'Tick', icon: <ChevronRight size={14} /> },
    { id: 'bug', label: 'Bug', icon: <ImageIcon size={14} /> },
    { id: 'fullscreen', label: 'Full', icon: <Eye size={14} /> },
    { id: 'social', label: 'Soc', icon: <Sparkles size={14} /> },
    { id: 'weather', label: 'Wx', icon: <FileText size={14} /> },
    { id: 'banner', label: 'Ban', icon: <FileText size={14} /> },
];

export function CGTemplatesPage() {
    const [envState, setEnvState] = useState(() => getEnvironment());
    const [electronWarningDismissed, setElectronWarningDismissed] = useState(false);
    const [templates, setTemplates] = useState<CGTemplate[]>([]);
    const [loadError, setLoadError] = useState<string>('');
    const [selectedCategory, setSelectedCategory] = useState<Category>('lower_third');
    const [selectedTemplate, setSelectedTemplate] = useState<CGTemplate | null>(null);
    const [templateContent, setTemplateContent] = useState<string>('');
    const [parameterValues, setParameterValues] = useState<Record<string, string>>({});
    const [presets, setPresets] = useState<CGTemplatePreset[]>(() => loadCGTemplatePresets());
    const [history, setHistory] = useState<CGHistoryItem[]>(() => loadCGHistory());
    const [searchQuery, setSearchQuery] = useState('');
    const [previewUrl, setPreviewUrl] = useState<string>('');
    const [isPreviewVisible, setIsPreviewVisible] = useState(true);
    const [previewScale, setPreviewScale] = useState(0.35);
    const [isEditing, setIsEditing] = useState(false);
    const [urlCopied, setUrlCopied] = useState(false);
    const [selectedLayer, setSelectedLayer] = useState<number>(1);
    const [transition, setTransition] = useState<'cut' | 'fade' | 'wipe_left' | 'wipe_right'>('cut');
    const [sidebarTab, setSidebarTab] = useState<SidebarTab>('templates');
    const [scheduleStart, setScheduleStart] = useState('');
    const [scheduleDuration, setScheduleDuration] = useState(30);
    const [scheduledEvents, setScheduledEvents] = useState<CGScheduledEvent[]>([]);
    const previewIframeRef = useRef<HTMLIFrameElement>(null);
    const env = envState;
    const electronAPI = (window as any).electronAPI;
    const { layers, clearLayer, clearAll } = useCGState();

    const externalUrl = `${window.location.origin}/#/cg-templates`;
    const [serverBaseUrl, setServerBaseUrl] = useState<string>('http://localhost:8080');

    const handleCopyUrl = () => {
        navigator.clipboard.writeText(externalUrl).then(() => {
            setUrlCopied(true);
            setTimeout(() => setUrlCopied(false), 2000);
        });
    };

    const handleBack = () => {
        // If opened as a popup/window from playout, close instead of navigating (keeps playout running)
        if (window.opener) {
            window.close();
            return;
        }
        window.location.hash = '#/playout';
    };

    // On mount: clear env cache so we re-detect (e.g. when opened via window.open() and preload attaches later)
    useEffect(() => {
        clearEnvironmentCache();
        setEnvState(getEnvironment());
    }, []);

    // Refresh environment when preload API becomes available (new window may get electronAPI a moment after load)
    useEffect(() => {
        let cancelled = false;
        (async () => {
            if (env.isElectron) return;
            const ok = await waitForElectronAPI(2000);
            if (!cancelled && ok) {
                clearEnvironmentCache();
                setEnvState(getEnvironment());
            }
        })();
        return () => { cancelled = true; };
    }, [env.isElectron]);

    // Load templates on mount
    useEffect(() => {
        if (env.isElectron && electronAPI) {
            // Use actual runtime port (8080 may be in-use and auto-incremented)
            electronAPI.getAllUrls?.().then((u: any) => {
                if (u?.port) setServerBaseUrl(`http://localhost:${u.port}`);
            }).catch(() => {});
            loadTemplates();
            electronAPI.cgListScheduledEvents?.().then((result: any) => {
                if (result?.success && Array.isArray(result.events)) setScheduledEvents(result.events);
            }).catch(() => {});
        }
    }, [env.isElectron]);

    // Update preview when parameters change
    useEffect(() => {
        if (selectedTemplate && templateContent) {
            updatePreview();
        }
    }, [parameterValues, selectedTemplate, templateContent]);

    const loadTemplates = async () => {
        if (!electronAPI) return;
        try {
            const result = await electronAPI.listCGTemplates();
            if (result.success) {
                const loadedTemplates = result.templates;
                setTemplates(loadedTemplates);
                if (loadedTemplates.length > 0 && !selectedTemplate) {
                    const firstTemplate = loadedTemplates.find((t: CGTemplate) => t.category === selectedCategory) || loadedTemplates[0];
                    setSelectedTemplate(firstTemplate);
                    setTimeout(() => loadTemplateContent(firstTemplate.id), 0);
                }
                setLoadError('');
            } else {
                setLoadError(result.error || 'Failed to load templates');
            }
        } catch (error) {
            console.error('Failed to load templates:', error);
            setLoadError(error instanceof Error ? error.message : 'Failed to load templates');
        }
    };

    const loadTemplateContent = async (templateId: string) => {
        if (!electronAPI) return;
        try {
            const result = await electronAPI.getCGTemplate(templateId);
            if (result.success && result.content) {
                setTemplateContent(result.content);
            }
        } catch (error) {
            console.error('Failed to load template content:', error);
        }
    };

    const updatePreview = () => {
        if (!selectedTemplate || !templateContent) return;
        
        const params = new URLSearchParams();
        Object.entries(parameterValues).forEach(([key, value]) => {
            if (value) params.set(key, value);
        });
        
        const htmlWithParams = templateContent.replace(
            /<script>/,
            `<script>
                const params = new URLSearchParams(window.location.search);
                ${selectedTemplate.parameters.map(p => 
                    `if (!params.get('${p.name}')) params.set('${p.name}', '${p.defaultValue || ''}');`
                ).join('\n                ')}
            </script>`
        );
        
        const blob = new Blob([htmlWithParams], { type: 'text/html' });
        const url = URL.createObjectURL(blob);
        setPreviewUrl(`${url}?${params.toString()}`);
    };

    const handleTemplateSelect = async (template: CGTemplate) => {
        setSelectedTemplate(template);
        setIsEditing(false);
        await loadTemplateContent(template.id);
        const preset = presets.find(p => p.templateId === template.id);
        if (preset) {
            setParameterValues(preset.values);
        } else {
            const initialValues: Record<string, string> = {};
            template.parameters.forEach(param => {
                initialValues[param.name] = param.defaultValue || '';
            });
            setParameterValues(initialValues);
        }
    };

    const handleParameterChange = (name: string, value: string) => {
        setParameterValues(prev => ({ ...prev, [name]: value }));
    };

    const handleSavePreset = async () => {
        if (!selectedTemplate) return;
        const name = await safePrompt('Preset name:');
        if (!name) return;
        
        const preset = createCGTemplatePreset(name, selectedTemplate.id, parameterValues);
        const updated = [...presets, preset];
        setPresets(updated);
        saveCGTemplatePresets(updated);
    };

    const handleLoadPreset = (preset: CGTemplatePreset) => {
        if (preset.templateId !== selectedTemplate?.id) {
            const template = templates.find(t => t.id === preset.templateId);
            if (template) {
                handleTemplateSelect(template);
            }
        }
        setParameterValues(preset.values);
    };

    const handleDeletePreset = (id: string) => {
        if (confirm('Delete this preset?')) {
            const updated = presets.filter(p => p.id !== id);
            setPresets(updated);
            saveCGTemplatePresets(updated);
        }
    };

    const handleTake = async () => {
        if (!selectedTemplate) return;
        
        // Add to history
        addCGHistoryItem(selectedTemplate.id, parameterValues);
        setHistory(loadCGHistory());
        
        // Build the template URL with parameters
        const params = new URLSearchParams(parameterValues);
        const templateUrl = `${serverBaseUrl}/cg-template/${selectedTemplate.id}?${params.toString()}`;
        
        // Drive the unified preview/program workflow through the shared CG store.
        await cgStateManager.setSelectedLayerNumber(selectedLayer);
        await cgStateManager.selectTemplate(selectedTemplate);

        const normalizedTemplate = normalizeTemplateToSource(selectedTemplate);
        const mappedCoreEntries = Object.entries(normalizedTemplate.fieldMap || {});
        const mappedFieldKeys = new Set(mappedCoreEntries.map(([, fieldKey]) => fieldKey));

        for (const [coreField, fieldKey] of mappedCoreEntries) {
            const value = parameterValues[fieldKey];
            if (value !== undefined) {
                await cgStateManager.updateDraftCore(coreField as 'text1' | 'text2' | 'text3' | 'style1' | 'style2' | 'style3', value);
            }
        }

        for (const [fieldKey, value] of Object.entries(parameterValues)) {
            if (!mappedFieldKeys.has(fieldKey)) {
                await cgStateManager.updateDraftAdvanced(fieldKey, value);
            }
        }

        await cgStateManager.takeToProgram();
        
        console.log('[CGTemplatesPage] Take:', selectedTemplate.id, 'Layer:', selectedLayer, 'URL:', templateUrl);
    };

    const handleClear = () => {
        clearLayer(selectedLayer);
        console.log('[CGTemplatesPage] Clear Layer:', selectedLayer);
    };

    const handleClearAll = () => {
        clearAll();
        console.log('[CGTemplatesPage] Clear All CG');
    };

    const handleSchedule = async () => {
        if (!selectedTemplate || !scheduleStart) return;
        const event: CGScheduledEvent = {
            id: `cg_evt_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`,
            templateId: selectedTemplate.id,
            templateName: selectedTemplate.name,
            values: parameterValues,
            layer: selectedLayer,
            channel: 'program',
            startTime: new Date(scheduleStart).toISOString(),
            durationSeconds: scheduleDuration,
            enabled: true,
            status: 'scheduled',
            createdAt: new Date().toISOString(),
        };
        const result = await electronAPI?.cgScheduleEvent?.(event);
        if (result?.success) {
            setScheduledEvents(result.events || [event, ...scheduledEvents]);
            setScheduleStart('');
        }
    };

    const handleLoadFromHistory = (item: CGHistoryItem) => {
        const template = templates.find(t => t.id === item.templateId);
        if (template) {
            handleTemplateSelect(template);
            setParameterValues(item.values);
        }
    };

    const handleCopyTemplate = () => {
        if (!selectedTemplate) return;
        const newTemplate = {
            ...selectedTemplate,
            id: `${selectedTemplate.id}_copy_${Date.now()}`,
            name: `${selectedTemplate.name} (Copy)`,
        };
        // TODO: Save new template
        console.log('Copy template:', newTemplate);
    };

    const filteredTemplates = templates.filter(t => {
        if (t.category !== selectedCategory) return false;
        if (searchQuery && !t.name.toLowerCase().includes(searchQuery.toLowerCase())) return false;
        return true;
    });

    const categoryPresets = presets.filter(p => {
        const template = templates.find(t => t.id === p.templateId);
        return template?.category === selectedCategory;
    });

    return (
        <div className={styles.page}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <button className={styles.backBtn} onClick={handleBack} title="Back to Playout">
                        <ArrowLeft size={18} />
                    </button>
                    <div>
                        <h1>CG Templates</h1>
                        <p className={styles.subtitle}>Manage and preview HTML-based CG templates</p>
                    </div>
                </div>
                <div className={styles.headerActions}>
                    <div className={styles.urlDisplay}>
                        <span className={styles.urlLabel}>External URL:</span>
                        <code className={styles.urlCode}>{externalUrl}</code>
                        <button
                            className={styles.btnIcon}
                            onClick={handleCopyUrl}
                            title="Copy URL"
                        >
                            {urlCopied ? <Check size={16} /> : <Copy size={16} />}
                        </button>
                    </div>
                    <button className={styles.btnSecondary} onClick={async () => {
                        if (!electronAPI?.importCGTemplateFolder) return;
                        const res = await electronAPI.importCGTemplateFolder();
                        if (res?.success) {
                            await loadTemplates();
                        }
                    }}>
                        <Upload size={16} />
                        Import Folder
                    </button>
                    <button className={styles.btnSecondary} onClick={() => electronAPI?.openCGTemplateFolder?.()}>
                        <FolderOpen size={16} />
                        Open Template Folder
                    </button>
                </div>
            </div>
            {!env.isElectron && !electronWarningDismissed && (
                <div className={styles.electronRequired}>
                    <div className={styles.electronRequiredContent}>
                        <button
                            type="button"
                            className={styles.electronRequiredClose}
                            onClick={() => setElectronWarningDismissed(true)}
                            title="Dismiss (you can still use Presets)"
                            aria-label="Close"
                        >
                            <X size={20} />
                        </button>
                        <Layers size={64} />
                        <h2>Electron App Required</h2>
                        <p>CG Templates require the desktop Electron app to function properly.</p>
                        <p className={styles.electronRequiredDetails}>
                            The browser mode cannot access the local file system to load and serve HTML templates.
                            Please launch the Play Play desktop application to use this feature.
                        </p>
                        <button className={styles.btnSecondary} onClick={handleBack}>
                            <ArrowLeft size={16} />
                            Back to Playout
                        </button>
                        <button
                            type="button"
                            className={styles.electronRequiredDismiss}
                            onClick={() => setElectronWarningDismissed(true)}
                        >
                            Dismiss and use Presets
                        </button>
                    </div>
                </div>
            )}
            {!env.isElectron && electronWarningDismissed && (
                <div className={styles.electronRequiredBanner}>
                    <Layers size={16} />
                    <span>Templates need the desktop app. You can still use Presets and History.</span>
                </div>
            )}
            {loadError && (
                <div style={{ padding: '10px 14px', color: 'var(--danger)' }}>
                    Failed to load templates: {loadError}
                </div>
            )}

            <div className={styles.layout}>
                {/* Left: Tabbed Sidebar */}
                <div className={styles.sidebar}>
                    {/* Main Tabs */}
                    <div className={styles.sidebarTabs}>
                        <button
                            className={`${styles.sidebarTab} ${sidebarTab === 'templates' ? styles.active : ''}`}
                            onClick={() => setSidebarTab('templates')}
                        >
                            <Layers size={14} /> Templates
                        </button>
                        <button
                            className={`${styles.sidebarTab} ${sidebarTab === 'presets' ? styles.active : ''}`}
                            onClick={() => setSidebarTab('presets')}
                        >
                            <Save size={14} /> Presets
                        </button>
                        <button
                            className={`${styles.sidebarTab} ${sidebarTab === 'history' ? styles.active : ''}`}
                            onClick={() => setSidebarTab('history')}
                        >
                            <History size={14} /> History
                        </button>
                    </div>

                    {/* Templates Tab */}
                    {sidebarTab === 'templates' && (
                        <>
                            <div className={styles.categoryTabs}>
                                {CATEGORIES.map(cat => (
                                    <button
                                        key={cat.id}
                                        className={`${styles.categoryTab} ${selectedCategory === cat.id ? styles.active : ''}`}
                                        onClick={() => {
                                            setSelectedCategory(cat.id);
                                            const firstInCategory = templates.find(t => t.category === cat.id);
                                            if (firstInCategory) {
                                                handleTemplateSelect(firstInCategory);
                                            }
                                        }}
                                        title={cat.label}
                                    >
                                        {cat.icon}
                                    </button>
                                ))}
                            </div>

                            <div className={styles.searchBox}>
                                <Search size={14} />
                                <input
                                    type="text"
                                    placeholder="Search..."
                                    value={searchQuery}
                                    onChange={(e) => setSearchQuery(e.target.value)}
                                />
                            </div>

                            {templates.length === 0 && env.isElectron && (
                                <div className={styles.emptyImport}>
                                    <p>No templates</p>
                                    <button
                                        className={styles.btnSmall}
                                        onClick={async () => {
                                            const res = await electronAPI?.importCGTemplateFolder?.();
                                            if (res?.success) await loadTemplates();
                                        }}
                                    >
                                        <Upload size={12} /> Import
                                    </button>
                                </div>
                            )}

                            <div className={styles.templateList}>
                                {filteredTemplates.map(template => (
                                    <div
                                        key={template.id}
                                        className={`${styles.templateCard} ${selectedTemplate?.id === template.id ? styles.selected : ''}`}
                                        onClick={() => handleTemplateSelect(template)}
                                    >
                                        <span className={styles.templateName}>{template.name}</span>
                                        <span className={styles.templateParams}>{template.parameters.length}p</span>
                                    </div>
                                ))}
                            </div>
                        </>
                    )}

                    {/* Presets Tab */}
                    {sidebarTab === 'presets' && (
                        <div className={styles.tabContent}>
                            <div className={styles.tabActions}>
                                <button
                                    className={styles.btnSmall}
                                    onClick={handleSavePreset}
                                    disabled={!selectedTemplate}
                                >
                                    <Plus size={12} /> Save Current
                                </button>
                            </div>
                            <div className={styles.presetListCompact}>
                                {presets.map(preset => (
                                    <div key={preset.id} className={styles.presetItemCompact}>
                                        <div className={styles.presetInfo} onClick={() => handleLoadPreset(preset)}>
                                            <span>{preset.name}</span>
                                            <span className={styles.presetMeta}>
                                                {templates.find(t => t.id === preset.templateId)?.name || 'Unknown'}
                                            </span>
                                        </div>
                                        <button
                                            className={styles.btnIconTiny}
                                            onClick={() => handleDeletePreset(preset.id)}
                                        >
                                            <Trash2 size={12} />
                                        </button>
                                    </div>
                                ))}
                                {presets.length === 0 && (
                                    <div className={styles.emptySmall}>No presets saved</div>
                                )}
                            </div>
                        </div>
                    )}

                    {/* History Tab */}
                    {sidebarTab === 'history' && (
                        <div className={styles.tabContent}>
                            <div className={styles.tabActions}>
                                <button
                                    className={styles.btnSmall}
                                    onClick={() => {
                                        if (confirm('Clear history?')) {
                                            clearCGHistory();
                                            setHistory([]);
                                        }
                                    }}
                                >
                                    <Trash2 size={12} /> Clear
                                </button>
                            </div>
                            <div className={styles.historyListCompact}>
                                {history.slice(0, 30).map(item => (
                                    <div
                                        key={item.id}
                                        className={styles.historyItemCompact}
                                        onClick={() => handleLoadFromHistory(item)}
                                    >
                                        <span>{templates.find(t => t.id === item.templateId)?.name || 'Unknown'}</span>
                                        <span className={styles.historyTime}>
                                            {new Date(item.usedAt).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
                                        </span>
                                    </div>
                                ))}
                                {history.length === 0 && (
                                    <div className={styles.emptySmall}>No history</div>
                                )}
                            </div>
                        </div>
                    )}
                </div>

                {/* Main Content: Preview + Controls */}
                <div className={styles.mainContent}>
                    {selectedTemplate ? (
                        <div className={styles.mainGrid}>
                            {/* Top: Preview Monitor */}
                            <div className={styles.previewPanel}>
                                <div className={styles.previewMonitor}>
                                    <div className={styles.monitorFrame}>
                                        <iframe
                                            ref={previewIframeRef}
                                            src={previewUrl}
                                            className={styles.previewScaled}
                                            style={{ transform: `scale(${previewScale})` }}
                                            title="CG Preview"
                                        />
                                    </div>
                                    <div className={styles.monitorBar}>
                                        <span className={styles.templateTitle}>{selectedTemplate.name}</span>
                                        <div className={styles.monitorControls}>
                                            <button className={styles.btnMini} onClick={() => updatePreview()} title="Refresh">
                                                <RefreshCw size={12} />
                                            </button>
                                            <button className={styles.btnMini} onClick={() => setPreviewScale(s => Math.max(0.1, +(s - 0.05).toFixed(2)))}>−</button>
                                            <span className={styles.scaleLabel}>{Math.round(previewScale * 100)}%</span>
                                            <button className={styles.btnMini} onClick={() => setPreviewScale(s => Math.min(1, +(s + 0.05).toFixed(2)))}>+</button>
                                        </div>
                                    </div>
                                </div>

                                {/* Layer Status Bar */}
                                <div className={styles.layerStatus}>
                                    {[1, 2, 3, 4, 5].map(layer => {
                                        const layerData = layers.find(l => l.layer === layer);
                                        const isActive = layerData?.isVisible;
                                        return (
                                            <div
                                                key={layer}
                                                className={`${styles.layerIndicator} ${selectedLayer === layer ? styles.selected : ''} ${isActive ? styles.live : ''}`}
                                                onClick={() => setSelectedLayer(layer)}
                                            >
                                                <span className={styles.layerNum}>L{layer}</span>
                                                {isActive && <span className={styles.liveTag}>LIVE</span>}
                                            </div>
                                        );
                                    })}
                                </div>
                            </div>

                            {/* Bottom: Controls Split */}
                            <div className={styles.controlsPanel}>
                                {/* Parameters Grid */}
                                <div className={styles.paramsPanel}>
                                    <div className={styles.panelHeader}>
                                        <span>Parameters</span>
                                        <button className={styles.btnMini} onClick={handleSavePreset} title="Save Preset">
                                            <Save size={12} />
                                        </button>
                                    </div>
                                    <div className={styles.paramsGrid}>
                                        {selectedTemplate.parameters.length === 0 ? (
                                            <div className={styles.noParams}>No parameters</div>
                                        ) : (
                                            selectedTemplate.parameters.map(param => (
                                                <div key={param.name} className={styles.paramRow}>
                                                    <label title={param.defaultValue ? `Default: ${param.defaultValue}` : ''}>
                                                        {param.label || param.name}
                                                    </label>
                                                    {param.type === 'select' && param.options ? (
                                                        <select
                                                            value={parameterValues[param.name] || ''}
                                                            onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                                            className={styles.paramInput}
                                                        >
                                                            <option value="">...</option>
                                                            {param.options.map(opt => (
                                                                <option key={opt} value={opt}>{opt}</option>
                                                            ))}
                                                        </select>
                                                    ) : param.type === 'color' ? (
                                                        <div className={styles.colorRow}>
                                                            <input
                                                                type="color"
                                                                value={parameterValues[param.name] || '#000000'}
                                                                onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                                                className={styles.colorPicker}
                                                            />
                                                            <input
                                                                type="text"
                                                                value={parameterValues[param.name] || ''}
                                                                onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                                                className={styles.paramInput}
                                                                placeholder="#000000"
                                                            />
                                                        </div>
                                                    ) : (
                                                        <input
                                                            type={param.type === 'number' ? 'number' : 'text'}
                                                            value={parameterValues[param.name] || ''}
                                                            onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                                            className={styles.paramInput}
                                                            placeholder={param.defaultValue || ''}
                                                        />
                                                    )}
                                                </div>
                                            ))
                                        )}
                                    </div>
                                </div>

                                {/* On-Air Controls */}
                                <div className={styles.onAirPanel}>
                                    <div className={styles.transitionRow}>
                                        <select
                                            className={styles.transitionSelect}
                                            value={transition}
                                            onChange={(e) => setTransition(e.target.value as typeof transition)}
                                        >
                                            <option value="cut">Cut</option>
                                            <option value="fade">Fade</option>
                                            <option value="wipe_left">Wipe ←</option>
                                            <option value="wipe_right">Wipe →</option>
                                        </select>
                                    </div>
                                    <button className={styles.takeBtn} onClick={handleTake}>
                                        <Play size={18} />
                                        TAKE L{selectedLayer}
                                    </button>
                                    <button className={styles.clearBtn} onClick={handleClear}>
                                        <X size={16} />
                                        CLEAR L{selectedLayer}
                                    </button>
                                    <button
                                        className={styles.clearAllBtn}
                                        onClick={handleClearAll}
                                        disabled={layers.filter(l => l.isVisible).length === 0}
                                    >
                                        <Square size={14} />
                                        CLEAR ALL
                                    </button>
                                    <div style={{ display: 'grid', gridTemplateColumns: '1fr 74px', gap: '0.5rem' }}>
                                        <input
                                            type="datetime-local"
                                            value={scheduleStart}
                                            onChange={(e) => setScheduleStart(e.target.value)}
                                            title="CG event start"
                                        />
                                        <input
                                            type="number"
                                            min={1}
                                            value={scheduleDuration}
                                            onChange={(e) => setScheduleDuration(Math.max(1, Number(e.target.value) || 30))}
                                            title="Seconds"
                                        />
                                    </div>
                                    <button className={styles.clearBtn} onClick={handleSchedule} disabled={!scheduleStart}>
                                        SCHEDULE CG
                                    </button>
                                    {scheduledEvents.length > 0 && (
                                        <div style={{ color: 'var(--text-muted)', fontSize: '0.75rem' }}>
                                            {scheduledEvents.filter(event => event.enabled).length} enabled CG event(s)
                                        </div>
                                    )}
                                </div>
                            </div>
                        </div>
                    ) : (
                        <div className={styles.emptyState}>
                            <Layers size={48} />
                            <p>Select a template</p>
                        </div>
                    )}
                </div>

            </div>
        </div>
    );
}
