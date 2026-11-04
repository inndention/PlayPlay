// ============================================
// News Slate Panel
// UI component for news slate management
// ============================================

import React, { useState, useEffect, useCallback, useMemo } from 'react';
import styles from './NewsSlatePanel.module.css';
import {
    Newspaper, Play, Pause, Volume2, VolumeX, Settings, RefreshCw,
    AlertTriangle, TrendingUp, Globe, Clock, Zap, Eye, EyeOff,
    Download, Upload, Save, Edit2, Trash2, Plus, Filter, Search,
    Radio, Tv, Bell, CheckCircle, XCircle, ChevronDown, ChevronUp
} from 'lucide-react';
import { getNewsFeedAggregator, NewsItem } from '../lib/newsFeedAggregator';
import { getNewsSlateGenerator, GeneratedSlate, SlateTemplate } from '../lib/newsSlateGenerator';
import { getOpenAITTS, TTSVoice } from '../lib/openaiTTS';
import { CGElement } from '../lib/types';
import { useToast } from './Toast';

interface NewsSlatePanelProps {
    onDeploySlate?: (slate: GeneratedSlate) => void;
    onPreviewSlate?: (slate: GeneratedSlate) => void;
}

export function NewsSlatePanel({ onDeploySlate, onPreviewSlate }: NewsSlatePanelProps) {
    const { showToast } = useToast();
    const [newsItems, setNewsItems] = useState<NewsItem[]>([]);
    const [selectedItem, setSelectedItem] = useState<NewsItem | null>(null);
    const [selectedTemplate, setSelectedTemplate] = useState<string>('');
    const [generatedSlate, setGeneratedSlate] = useState<GeneratedSlate | null>(null);
    const [ttsVoice, setTtsVoice] = useState<TTSVoice>('nova');
    const [generateTTS, setGenerateTTS] = useState(true);
    const [isGenerating, setIsGenerating] = useState(false);
    const [filterUrgency, setFilterUrgency] = useState<'all' | NewsItem['urgency']>('all');
    const [searchQuery, setSearchQuery] = useState('');
    const [autoDeploy, setAutoDeploy] = useState(false);

    const aggregator = useMemo(() => getNewsFeedAggregator(), []);
    const slateGenerator = useMemo(() => getNewsSlateGenerator(), []);
    const tts = useMemo(() => getOpenAITTS(), []);

    // Load news items
    useEffect(() => {
        const unsubscribe = aggregator.onUpdate((items) => {
            setNewsItems(items);
        });

        aggregator.start();
        setNewsItems(aggregator.getItems());

        return () => {
            unsubscribe();
            aggregator.stop();
        };
    }, [aggregator]);

    // Listen for breaking news
    useEffect(() => {
        const unsubscribe = aggregator.onBreakingNews((item) => {
            showToast(`BREAKING NEWS: ${item.title}`, 'warning', 5000);
            
            if (autoDeploy) {
                handleGenerateAndDeploy(item);
            }
        });

        return unsubscribe;
    }, [aggregator, autoDeploy, showToast]);

    // Get templates
    const templates = useMemo(() => slateGenerator.getTemplates(), [slateGenerator]);

    // Filtered news items
    const filteredItems = useMemo(() => {
        let items = newsItems;

        // Filter by urgency
        if (filterUrgency !== 'all') {
            items = items.filter(item => item.urgency === filterUrgency);
        }

        // Filter by search query
        if (searchQuery) {
            const query = searchQuery.toLowerCase();
            items = items.filter(item =>
                item.title.toLowerCase().includes(query) ||
                item.content.toLowerCase().includes(query) ||
                item.source.toLowerCase().includes(query)
            );
        }

        return items;
    }, [newsItems, filterUrgency, searchQuery]);

    // Generate slate
    const handleGenerateSlate = useCallback(async (item: NewsItem) => {
        setIsGenerating(true);
        try {
            let audioUrl: string | undefined;

            // Generate TTS if enabled
            if (generateTTS) {
                try {
                    const ttsResult = await tts.generateSpeech(
                        `${item.title}. ${item.summary || item.content.substring(0, 200)}`,
                        { voice: ttsVoice }
                    );
                    audioUrl = ttsResult.audioUrl;
                } catch (error) {
                    console.error('[NewsSlate] TTS generation failed:', error);
                    showToast('TTS generation failed, continuing without audio', 'warning', 3000);
                }
            }

            // Generate slate
            const slate = slateGenerator.generateSlate(item, selectedTemplate || undefined, audioUrl);
            setGeneratedSlate(slate);
            setSelectedItem(item);

            showToast('Slate generated successfully', 'success', 2000);
        } catch (error) {
            console.error('[NewsSlate] Failed to generate slate:', error);
            showToast(`Failed to generate slate: ${error instanceof Error ? error.message : 'Unknown error'}`, 'error', 3000);
        } finally {
            setIsGenerating(false);
        }
    }, [generateTTS, ttsVoice, selectedTemplate, slateGenerator, tts, showToast]);

    // Generate and deploy
    const handleGenerateAndDeploy = useCallback(async (item?: NewsItem) => {
        const targetItem = item || selectedItem;
        if (!targetItem) return;

        await handleGenerateSlate(targetItem);
        
        // Wait a moment for slate to be set
        setTimeout(() => {
            const slate = generatedSlate || slateGenerator.generateSlate(targetItem);
            if (onDeploySlate) {
                onDeploySlate(slate);
                showToast('Slate deployed to broadcast', 'success', 2000);
            }
        }, 100);
    }, [selectedItem, handleGenerateSlate, generatedSlate, slateGenerator, onDeploySlate, showToast]);

    // Preview slate
    const handlePreview = useCallback(() => {
        if (generatedSlate && onPreviewSlate) {
            onPreviewSlate(generatedSlate);
        }
    }, [generatedSlate, onPreviewSlate]);

    // Get urgency color
    const getUrgencyColor = (urgency: NewsItem['urgency']): string => {
        switch (urgency) {
            case 'breaking': return '#ff0000';
            case 'high': return '#ffaa00';
            case 'normal': return '#3b82f6';
            case 'low': return '#6b7280';
            default: return '#6b7280';
        }
    };

    // Get urgency icon
    const getUrgencyIcon = (urgency: NewsItem['urgency']) => {
        switch (urgency) {
            case 'breaking': return <AlertTriangle size={16} />;
            case 'high': return <TrendingUp size={16} />;
            default: return <Newspaper size={16} />;
        }
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Newspaper size={20} />
                    <h2>News Slate System</h2>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.iconBtn}
                        onClick={() => aggregator.start()}
                        title="Refresh feeds"
                    >
                        <RefreshCw size={16} />
                    </button>
                    <label className={styles.toggle}>
                        <input
                            type="checkbox"
                            checked={autoDeploy}
                            onChange={(e) => setAutoDeploy(e.target.checked)}
                        />
                        <span>Auto-deploy breaking news</span>
                    </label>
                </div>
            </div>

            <div className={styles.content}>
                {/* News Feed List */}
                <div className={styles.newsList}>
                    <div className={styles.filters}>
                        <div className={styles.searchBox}>
                            <Search size={16} />
                            <input
                                type="text"
                                placeholder="Search news..."
                                value={searchQuery}
                                onChange={(e) => setSearchQuery(e.target.value)}
                            />
                        </div>
                        <select
                            value={filterUrgency}
                            onChange={(e) => setFilterUrgency(e.target.value as any)}
                        >
                            <option value="all">All Urgency</option>
                            <option value="breaking">Breaking</option>
                            <option value="high">High</option>
                            <option value="normal">Normal</option>
                            <option value="low">Low</option>
                        </select>
                    </div>

                    <div className={styles.itemsList}>
                        {filteredItems.map(item => (
                            <div
                                key={item.id}
                                className={`${styles.newsItem} ${selectedItem?.id === item.id ? styles.selected : ''}`}
                                onClick={() => setSelectedItem(item)}
                            >
                                <div className={styles.itemHeader}>
                                    <div
                                        className={styles.urgencyBadge}
                                        style={{ backgroundColor: getUrgencyColor(item.urgency) }}
                                    >
                                        {getUrgencyIcon(item.urgency)}
                                        <span>{item.urgency.toUpperCase()}</span>
                                    </div>
                                    <span className={styles.source}>{item.source}</span>
                                    <span className={styles.time}>
                                        {new Date(item.publishedAt).toLocaleTimeString()}
                                    </span>
                                </div>
                                <h3 className={styles.itemTitle}>{item.title}</h3>
                                {item.summary && (
                                    <p className={styles.itemSummary}>{item.summary}</p>
                                )}
                                {item.slateGenerated && (
                                    <div className={styles.slateGenerated}>
                                        <CheckCircle size={14} />
                                        <span>Slate generated</span>
                                    </div>
                                )}
                            </div>
                        ))}
                    </div>
                </div>

                {/* Slate Generator */}
                <div className={styles.generator}>
                    <div className={styles.generatorHeader}>
                        <h3>Slate Generator</h3>
                        {selectedItem && (
                            <button
                                className={styles.generateBtn}
                                onClick={() => handleGenerateSlate(selectedItem)}
                                disabled={isGenerating}
                            >
                                {isGenerating ? <RefreshCw size={16} className={styles.spinning} /> : <Zap size={16} />}
                                Generate Slate
                            </button>
                        )}
                    </div>

                    {selectedItem && (
                        <div className={styles.generatorContent}>
                            <div className={styles.settings}>
                                <div className={styles.settingGroup}>
                                    <label>Template</label>
                                    <select
                                        value={selectedTemplate}
                                        onChange={(e) => setSelectedTemplate(e.target.value)}
                                    >
                                        <option value="">Auto-select by urgency</option>
                                        {templates.map(template => (
                                            <option key={template.id} value={template.id}>
                                                {template.name} ({template.style})
                                            </option>
                                        ))}
                                    </select>
                                </div>

                                <div className={styles.settingGroup}>
                                    <label>
                                        <input
                                            type="checkbox"
                                            checked={generateTTS}
                                            onChange={(e) => setGenerateTTS(e.target.checked)}
                                        />
                                        Generate TTS Audio
                                    </label>
                                </div>

                                {generateTTS && (
                                    <div className={styles.settingGroup}>
                                        <label>TTS Voice</label>
                                        <select
                                            value={ttsVoice}
                                            onChange={(e) => setTtsVoice(e.target.value as TTSVoice)}
                                        >
                                            {tts.getVoices().map(voice => (
                                                <option key={voice} value={voice}>
                                                    {voice} - {tts.getVoiceDescription(voice)}
                                                </option>
                                            ))}
                                        </select>
                                    </div>
                                )}
                            </div>

                            {generatedSlate && (
                                <div className={styles.slatePreview}>
                                    <div className={styles.slateInfo}>
                                        <h4>Generated Slate</h4>
                                        <div className={styles.slateMeta}>
                                            <span>Duration: {generatedSlate.duration}s</span>
                                            <span>Elements: {generatedSlate.cgElements.length}</span>
                                            {generatedSlate.audioUrl && (
                                                <span className={styles.hasAudio}>
                                                    <Volume2 size={14} />
                                                    Audio included
                                                </span>
                                            )}
                                        </div>
                                    </div>
                                    <div className={styles.slateActions}>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={handlePreview}
                                        >
                                            <Eye size={16} />
                                            Preview
                                        </button>
                                        <button
                                            className={styles.actionBtn}
                                            onClick={() => handleGenerateAndDeploy()}
                                        >
                                            <Tv size={16} />
                                            Deploy to Broadcast
                                        </button>
                                    </div>
                                </div>
                            )}
                        </div>
                    )}

                    {!selectedItem && (
                        <div className={styles.noSelection}>
                            <Newspaper size={48} />
                            <p>Select a news item to generate a slate</p>
                        </div>
                    )}
                </div>
            </div>
        </div>
    );
}

