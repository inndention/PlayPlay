import React, { useState, useEffect, useRef } from 'react';
import { CGTemplate, CGTemplatePreset, CGHistoryItem } from '../lib/types';
import { loadCGTemplatePresets, saveCGTemplatePresets, createCGTemplatePreset, loadCGHistory, addCGHistoryItem, clearCGHistory } from '../lib/storage';
import { getEnvironment } from '../lib/fileResolver';
import { 
    Search, Play, X, Save, Trash2, History, Settings, 
    ChevronRight, Image as ImageIcon, FileText, Sparkles
} from 'lucide-react';
import styles from './CGManager.module.css';

type Category = 'lower_third' | 'ticker' | 'bug' | 'fullscreen' | 'social' | 'weather' | 'banner';

const CATEGORIES: { id: Category; label: string }[] = [
    { id: 'lower_third', label: 'Lower Thirds' },
    { id: 'ticker', label: 'Tickers' },
    { id: 'bug', label: 'Bugs' },
    { id: 'fullscreen', label: 'Fullscreen' },
    { id: 'social', label: 'Social' },
    { id: 'weather', label: 'Weather' },
    { id: 'banner', label: 'Banners' },
];

export function CGManager() {
    const [templates, setTemplates] = useState<CGTemplate[]>([]);
    const [selectedCategory, setSelectedCategory] = useState<Category>('lower_third');
    const [selectedTemplate, setSelectedTemplate] = useState<CGTemplate | null>(null);
    const [templateContent, setTemplateContent] = useState<string>('');
    const [parameterValues, setParameterValues] = useState<Record<string, string>>({});
    const [presets, setPresets] = useState<CGTemplatePreset[]>(() => loadCGTemplatePresets());
    const [history, setHistory] = useState<CGHistoryItem[]>(() => loadCGHistory());
    const [searchQuery, setSearchQuery] = useState('');
    const [previewUrl, setPreviewUrl] = useState<string>('');
    const [isPreviewVisible, setIsPreviewVisible] = useState(false);
    const previewIframeRef = useRef<HTMLIFrameElement>(null);
    const env = getEnvironment();
    const electronAPI = (window as any).electronAPI;

    // Load templates on mount
    useEffect(() => {
        if (env.isElectron && electronAPI) {
            loadTemplates();
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
                    const firstTemplate = loadedTemplates[0];
                    setSelectedTemplate(firstTemplate);
                    // Load content after setting template
                    setTimeout(() => loadTemplateContent(firstTemplate.id), 0);
                }
            }
        } catch (error) {
            console.error('Failed to load templates:', error);
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
        
        // Create a data URL with the template HTML and parameters
        const params = new URLSearchParams();
        Object.entries(parameterValues).forEach(([key, value]) => {
            if (value) params.set(key, value);
        });
        
        // Create a blob URL for the template
        const htmlWithParams = templateContent.replace(
            /<script>/,
            `<script>
                // Inject parameters from URL
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
        await loadTemplateContent(template.id);
        // Load existing preset values if available
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
        const { safePrompt } = await import('../lib/promptPolyfill');
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

    const handleTake = () => {
        if (!selectedTemplate) return;
        
        // Add to history
        addCGHistoryItem(selectedTemplate.id, parameterValues);
        setHistory(loadCGHistory());
        
        // Send to output (this would integrate with CGOverlay)
        // For now, just log
        console.log('[CGManager] Take:', selectedTemplate.id, parameterValues);
        
        // TODO: Integrate with CGOverlay component
    };

    const handleClear = () => {
        console.log('[CGManager] Clear CG');
        // TODO: Integrate with CGOverlay component
    };

    const handleLoadFromHistory = (item: CGHistoryItem) => {
        const template = templates.find(t => t.id === item.templateId);
        if (template) {
            handleTemplateSelect(template);
            setParameterValues(item.values);
        }
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
        <div className={styles.cgManager}>
            <div className={styles.header}>
                <h2>CG Template Manager</h2>
            </div>

            <div className={styles.layout}>
                {/* Left: Template Browser */}
                <div className={styles.templateBrowser}>
                    <div className={styles.categoryTabs}>
                        {CATEGORIES.map(cat => (
                            <button
                                key={cat.id}
                                className={`${styles.categoryTab} ${selectedCategory === cat.id ? styles.active : ''}`}
                                onClick={() => setSelectedCategory(cat.id)}
                            >
                                {cat.label}
                            </button>
                        ))}
                    </div>

                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input
                            type="text"
                            placeholder="Search templates..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>

                    <div className={styles.templateGrid}>
                        {filteredTemplates.map(template => (
                            <div
                                key={template.id}
                                className={`${styles.templateCard} ${selectedTemplate?.id === template.id ? styles.selected : ''}`}
                                onClick={() => handleTemplateSelect(template)}
                            >
                                <FileText size={24} />
                                <span className={styles.templateName}>{template.name}</span>
                                <span className={styles.templateParams}>{template.parameters.length} params</span>
                            </div>
                        ))}
                        {filteredTemplates.length === 0 && (
                            <div className={styles.empty}>
                                No templates found
                            </div>
                        )}
                    </div>
                </div>

                {/* Center: Preview and Parameters */}
                <div className={styles.previewPanel}>
                    {selectedTemplate ? (
                        <>
                            <div className={styles.previewHeader}>
                                <h3>{selectedTemplate.name}</h3>
                                <div className={styles.previewControls}>
                                    <button
                                        className={styles.previewToggle}
                                        onClick={() => setIsPreviewVisible(!isPreviewVisible)}
                                    >
                                        {isPreviewVisible ? 'Hide' : 'Show'} Preview
                                    </button>
                                </div>
                            </div>

                            {isPreviewVisible && (
                                <div className={styles.previewContainer}>
                                    <iframe
                                        ref={previewIframeRef}
                                        src={previewUrl}
                                        className={styles.previewIframe}
                                        title="CG Preview"
                                    />
                                </div>
                            )}

                            <div className={styles.parameters}>
                                <h4>Parameters</h4>
                                {selectedTemplate.parameters.map(param => (
                                    <div key={param.name} className={styles.parameterField}>
                                        <label>{param.label || param.name}</label>
                                        {param.type === 'select' && param.options ? (
                                            <select
                                                value={parameterValues[param.name] || ''}
                                                onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                            >
                                                <option value="">Select...</option>
                                                {param.options.map(opt => (
                                                    <option key={opt} value={opt}>{opt}</option>
                                                ))}
                                            </select>
                                        ) : param.type === 'color' ? (
                                            <input
                                                type="color"
                                                value={parameterValues[param.name] || '#000000'}
                                                onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                            />
                                        ) : (
                                            <input
                                                type="text"
                                                value={parameterValues[param.name] || ''}
                                                onChange={(e) => handleParameterChange(param.name, e.target.value)}
                                                placeholder={param.defaultValue || ''}
                                            />
                                        )}
                                    </div>
                                ))}
                            </div>

                            <div className={styles.onAirControls}>
                                <button className={styles.takeBtn} onClick={handleTake}>
                                    <Play size={16} />
                                    Take
                                </button>
                                <button className={styles.clearBtn} onClick={handleClear}>
                                    <X size={16} />
                                    Clear
                                </button>
                            </div>
                        </>
                    ) : (
                        <div className={styles.empty}>
                            Select a template to get started
                        </div>
                    )}
                </div>

                {/* Right: Presets and History */}
                <div className={styles.sidebar}>
                    <div className={styles.sidebarSection}>
                        <div className={styles.sidebarHeader}>
                            <h4>Presets</h4>
                            <button className={styles.savePresetBtn} onClick={handleSavePreset} disabled={!selectedTemplate}>
                                <Save size={14} />
                            </button>
                        </div>
                        <div className={styles.presetList}>
                            {categoryPresets.map(preset => (
                                <div key={preset.id} className={styles.presetItem}>
                                    <span className={styles.presetName}>{preset.name}</span>
                                    <div className={styles.presetActions}>
                                        <button onClick={() => handleLoadPreset(preset)} className={styles.iconBtn}>
                                            <Play size={12} />
                                        </button>
                                        <button onClick={() => handleDeletePreset(preset.id)} className={styles.iconBtn}>
                                            <Trash2 size={12} />
                                        </button>
                                    </div>
                                </div>
                            ))}
                            {categoryPresets.length === 0 && (
                                <div className={styles.emptySmall}>No presets</div>
                            )}
                        </div>
                    </div>

                    <div className={styles.sidebarSection}>
                        <div className={styles.sidebarHeader}>
                            <h4>History</h4>
                            <button className={styles.clearHistoryBtn} onClick={() => {
                                if (confirm('Clear history?')) {
                                    clearCGHistory();
                                    setHistory([]);
                                }
                            }}>
                                <Trash2 size={14} />
                            </button>
                        </div>
                        <div className={styles.historyList}>
                            {history.slice(0, 20).map(item => (
                                <div
                                    key={item.id}
                                    className={styles.historyItem}
                                    onClick={() => handleLoadFromHistory(item)}
                                >
                                    <span className={styles.historyTemplate}>
                                        {templates.find(t => t.id === item.templateId)?.name || 'Unknown'}
                                    </span>
                                    <span className={styles.historyTime}>
                                        {new Date(item.usedAt).toLocaleTimeString()}
                                    </span>
                                </div>
                            ))}
                            {history.length === 0 && (
                                <div className={styles.emptySmall}>No history</div>
                            )}
                        </div>
                    </div>
                </div>
            </div>
        </div>
    );
}

