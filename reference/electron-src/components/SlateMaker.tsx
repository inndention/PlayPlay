// ============================================
// Slate Maker Component
// Generates promo filler content with weather, stories, cinema
// ============================================

import React, { useState, useEffect, useCallback } from 'react';
import { aggregateSlateContent, WeatherData, StoryData, CinemaData, TowerCamStream } from '../lib/slateContentAggregator';
import { generateTTSAudio } from '../lib/whisperTTS';
import { MediaItem } from '../lib/types';
import {
    Cloud, Newspaper, Film, Video, Play, Download,
    RefreshCw, Settings, Image as ImageIcon
} from 'lucide-react';
import styles from './SlateMaker.module.css';

interface SlateMakerProps {
    onGenerate?: (slate: MediaItem) => void;
    onClose?: () => void;
}

export function SlateMaker({ onGenerate, onClose }: SlateMakerProps) {
    const [template, setTemplate] = useState<'weather' | 'promo' | 'filler'>('weather');
    const [duration, setDuration] = useState(30);
    const [content, setContent] = useState<{
        weather: WeatherData | null;
        stories: StoryData[];
        cinema: CinemaData[];
        towerCams: TowerCamStream[];
        history?: Array<{ title: string; date: string; description: string }>;
    } | null>(null);
    const [loading, setLoading] = useState(false);
    const [selectedTowerCam, setSelectedTowerCam] = useState<string | null>(null);
    const [ttsText, setTtsText] = useState('');
    const [ttsAudio, setTtsAudio] = useState<HTMLAudioElement | null>(null);
    const [generatingTTS, setGeneratingTTS] = useState(false);

    // Load content on mount
    useEffect(() => {
        loadContent();
    }, []);

    const loadContent = useCallback(async () => {
        setLoading(true);
        try {
            const aggregated = await aggregateSlateContent();
            setContent(aggregated);
        } catch (error) {
            console.error('[SlateMaker] Error loading content:', error);
        } finally {
            setLoading(false);
        }
    }, []);

    const handleGenerateTTS = useCallback(async () => {
        if (!ttsText.trim()) return;
        
        setGeneratingTTS(true);
        try {
            const audio = await generateTTSAudio(ttsText);
            if (audio) {
                setTtsAudio(audio);
            }
        } catch (error) {
            console.error('[SlateMaker] Error generating TTS:', error);
        } finally {
            setGeneratingTTS(false);
        }
    }, [ttsText]);

    const handleGenerateSlate = useCallback(async () => {
        if (!content) return;
        
        // Create a slate media item
        const slate: MediaItem = {
            id: `slate_${Date.now()}`,
            title: `Slate - ${template} - ${new Date().toLocaleDateString()}`,
            duration: duration,
            src: '', // Will be generated
            type: 'promo',
            notes: `Generated slate: ${template}`,
        };

        // TODO: Generate actual video file with content
        // For now, just return the item structure
        onGenerate?.(slate);
    }, [content, template, duration, onGenerate]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <h3>Slate Maker</h3>
                <div className={styles.headerActions}>
                    <button className={styles.refreshBtn} onClick={loadContent} disabled={loading}>
                        <RefreshCw size={16} className={loading ? styles.spinning : ''} />
                    </button>
                    {onClose && (
                        <button className={styles.closeBtn} onClick={onClose}>
                            ×
                        </button>
                    )}
                </div>
            </div>

            <div className={styles.content}>
                {/* Template Selection */}
                <div className={styles.section}>
                    <label className={styles.label}>Template</label>
                    <div className={styles.templateOptions}>
                        <button
                            className={`${styles.templateBtn} ${template === 'weather' ? styles.active : ''}`}
                            onClick={() => setTemplate('weather')}
                        >
                            <Cloud size={16} />
                            Weather
                        </button>
                        <button
                            className={`${styles.templateBtn} ${template === 'promo' ? styles.active : ''}`}
                            onClick={() => setTemplate('promo')}
                        >
                            <Newspaper size={16} />
                            Promo
                        </button>
                        <button
                            className={`${styles.templateBtn} ${template === 'filler' ? styles.active : ''}`}
                            onClick={() => setTemplate('filler')}
                        >
                            <Film size={16} />
                            Filler
                        </button>
                    </div>
                </div>

                {/* Duration */}
                <div className={styles.section}>
                    <label className={styles.label}>Duration (seconds)</label>
                    <input
                        type="number"
                        value={duration}
                        onChange={(e) => setDuration(parseInt(e.target.value, 10) || 30)}
                        min="5"
                        max="300"
                        className={styles.durationInput}
                    />
                </div>

                {/* Content Preview */}
                {content && (
                    <div className={styles.section}>
                        <label className={styles.label}>Content Preview</label>
                        <div className={styles.contentPreview}>
                            {template === 'weather' && content.weather && (
                                <div className={styles.weatherPreview}>
                                    <div className={styles.weatherTemp}>
                                        {content.weather.temperature}°F
                                    </div>
                                    <div className={styles.weatherCondition}>
                                        {content.weather.condition}
                                    </div>
                                </div>
                            )}
                            
                            {template === 'promo' && content.stories.length > 0 && (
                                <div className={styles.storiesPreview}>
                                    {content.stories.slice(0, 3).map((story, i) => (
                                        <div key={i} className={styles.storyItem}>
                                            <h4>{story.title}</h4>
                                            <p>{story.summary}</p>
                                        </div>
                                    ))}
                                </div>
                            )}
                            
                            {template === 'filler' && content.cinema.length > 0 && (
                                <div className={styles.cinemaPreview}>
                                    {content.cinema.slice(0, 3).map((movie, i) => (
                                        <div key={i} className={styles.cinemaItem}>
                                            <strong>{movie.title}</strong>
                                            <span>{movie.showtime} @ {movie.theater}</span>
                                        </div>
                                    ))}
                                </div>
                            )}
                        </div>
                    </div>
                )}

                {/* Tower Cam Selection */}
                {content && content.towerCams.length > 0 && (
                    <div className={styles.section}>
                        <label className={styles.label}>Tower Cam Backdrop</label>
                        <select
                            value={selectedTowerCam || ''}
                            onChange={(e) => setSelectedTowerCam(e.target.value || null)}
                            className={styles.select}
                        >
                            <option value="">None</option>
                            {content.towerCams.map(cam => (
                                <option key={cam.id} value={cam.url}>
                                    {cam.name}
                                </option>
                            ))}
                        </select>
                    </div>
                )}

                {/* TTS Narration */}
                <div className={styles.section}>
                    <label className={styles.label}>Text-to-Speech Narration</label>
                    <textarea
                        value={ttsText}
                        onChange={(e) => setTtsText(e.target.value)}
                        className={styles.textarea}
                        placeholder="Enter text to narrate..."
                        rows={4}
                    />
                    <div className={styles.ttsActions}>
                        <button
                            className={styles.ttsBtn}
                            onClick={handleGenerateTTS}
                            disabled={!ttsText.trim() || generatingTTS}
                        >
                            {generatingTTS ? 'Generating...' : 'Generate TTS'}
                        </button>
                        {ttsAudio && (
                            <button
                                className={styles.playBtn}
                                onClick={() => ttsAudio.play()}
                            >
                                <Play size={14} /> Play
                            </button>
                        )}
                    </div>
                </div>
            </div>

            <div className={styles.footer}>
                <button className={styles.cancelBtn} onClick={onClose}>
                    Cancel
                </button>
                <button
                    className={styles.generateBtn}
                    onClick={handleGenerateSlate}
                    disabled={!content || loading}
                >
                    <Download size={16} />
                    Generate Slate
                </button>
            </div>
        </div>
    );
}

