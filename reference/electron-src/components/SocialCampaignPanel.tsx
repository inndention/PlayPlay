import React, { useMemo, useState } from 'react';
import styles from './SocialCampaignPanel.module.css';
import { Send, CalendarClock, Image as ImageIcon, Loader2, Sparkles, Trash2 } from 'lucide-react';
import { persistentLogger } from '../lib/persistentLogger';
import { logError, logInfo } from '../lib/errorBus';

type Channel = 'facebook' | 'website';

interface Suggestion {
    id: string;
    text: string;
    mediaHint?: string;
}

const STORAGE_KEY = 'social_post_log';

const STARNEWS_API_KEY = 'starnews_api_key';
const STARNEWS_API_URL = 'starnews_api_url';

function saveLog(entry: any) {
    try {
        const existing = JSON.parse(localStorage.getItem(STORAGE_KEY) || '[]');
        existing.push(entry);
        localStorage.setItem(STORAGE_KEY, JSON.stringify(existing.slice(-200)));
    } catch (e) {
        persistentLogger.error('Failed to save social post log', { error: String(e) }, 'Social');
    }
}

interface SocialCampaignPanelProps {
    onClose?: () => void;
    embedded?: boolean;
}

export function SocialCampaignPanel({ onClose, embedded = false }: SocialCampaignPanelProps) {
    const [client, setClient] = useState('');
    const [message, setMessage] = useState('');
    const [mediaUrl, setMediaUrl] = useState('');
    const [mediaList, setMediaList] = useState<string[]>([]);
    const [channels, setChannels] = useState<Channel[]>(['facebook', 'website']);
    const [scheduledAt, setScheduledAt] = useState<string>('');
    const [apiUrl, setApiUrl] = useState(() => localStorage.getItem(STARNEWS_API_URL) || 'https://getstarnews.com/router.php?endpoint=posts');
    const [apiKey, setApiKey] = useState(() => localStorage.getItem(STARNEWS_API_KEY) || '');
    const [suggestions, setSuggestions] = useState<Suggestion[]>([]);
    const [selectedSuggestion, setSelectedSuggestion] = useState<string | null>(null);
    const [busy, setBusy] = useState(false);
    const isEmbedded = !!embedded;

    const toggleChannel = (ch: Channel) => {
        setChannels(prev => prev.includes(ch) ? prev.filter(c => c !== ch) : [...prev, ch]);
    };

    const addMedia = () => {
        if (!mediaUrl.trim()) return;
        setMediaList(prev => [...prev, mediaUrl.trim()]);
        setMediaUrl('');
    };

    const handleGenerate = () => {
        const base = message.trim() || 'Draft campaign message';
        const safeClient = client || 'Your brand';
        const options = [
            `${safeClient} spotlight: ${base}. Add a 15s clip to tease the story.`,
            `Sneak peek for ${safeClient}: ${base}. Pair with a vertical image.`,
            `${safeClient} update: ${base}. Include a link to the latest page.`,
            `${safeClient} behind-the-scenes: ${base}. Drop 2-3 photos.`,
            `${safeClient} CTA: ${base}. Add a short caption and emoji for Facebook.`,
        ];
        const gen = options.slice(0, 5).map((t, i) => ({
            id: `${Date.now()}_${i}`,
            text: t,
            mediaHint: i % 2 === 0 ? 'Attach a clip' : 'Attach 1-3 images',
        }));
        setSuggestions(gen);
        setSelectedSuggestion(gen[0]?.id || null);
        logInfo('Generated social post suggestions', { client: safeClient, count: gen.length });
    };

    const selectedText = useMemo(() => {
        const found = suggestions.find(s => s.id === selectedSuggestion);
        return found?.text || message;
    }, [suggestions, selectedSuggestion, message]);

    const handleSubmit = (mode: 'now' | 'schedule') => {
        if (channels.length === 0) {
            logError('Select at least one channel for posting', { client, mode });
            return;
        }
        setBusy(true);
        const payload = {
            id: `${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
            client: client || 'General',
            message: selectedText || message,
            media: mediaList,
            channels,
            scheduledAt: mode === 'schedule' ? scheduledAt || null : null,
            ts: Date.now(),
            status: mode === 'schedule' ? 'scheduled' : 'submitted',
        };
        saveLog(payload);
        persistentLogger.info('Social post queued', payload, 'Social');
        // Fire website post immediately if selected and not scheduled
        if (channels.includes('website') && (mode === 'now' || !scheduledAt)) {
            if (!apiKey) {
                logError('Star News API key missing; post logged but not sent', { client });
            } else {
                postToStarNews(payload).catch(err => {
                    logError('Star News post failed', { error: String(err) });
                });
            }
        }
        // Persist API config
        localStorage.setItem(STARNEWS_API_KEY, apiKey);
        localStorage.setItem(STARNEWS_API_URL, apiUrl);
        setBusy(false);
        onClose?.();
    };

    const postToStarNews = async (payload: any) => {
        const title = payload.client ? `${payload.client} Update` : 'Campaign Post';
        const content = payload.message;
        const url = apiUrl || 'https://getstarnews.com/router.php?endpoint=posts';
        const res = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${apiKey}`,
            },
            body: JSON.stringify({ title, content }),
        });
        if (!res.ok) {
            const text = await res.text();
            throw new Error(`HTTP ${res.status}: ${text}`);
        }
        const data = await res.json().catch(() => ({}));
        persistentLogger.info('Star News post sent', { status: res.status, data }, 'Social');
    };

    return (
        <div className={isEmbedded ? styles.embeddedShell : styles.overlay}>
            <div className={isEmbedded ? styles.panelEmbedded : styles.panel}>
                <div className={styles.header}>
                    <div>
                        <h3>Campaign Post</h3>
                        <p>Create FB + website posts, preview, schedule, and log them.</p>
                    </div>
                    {!isEmbedded && onClose && (
                        <button className={styles.closeBtn} onClick={onClose} aria-label="Close">
                            ×
                        </button>
                    )}
                </div>

                <div className={styles.form}>
                    <div className={styles.fieldRow}>
                        <label>Client / Campaign</label>
                        <input
                            type="text"
                            value={client}
                            onChange={(e) => setClient(e.target.value)}
                            placeholder="e.g., ACME Sports"
                        />
                    </div>

                    <div className={styles.fieldRow}>
                        <label>Base Message</label>
                        <textarea
                            rows={3}
                            value={message}
                            onChange={(e) => setMessage(e.target.value)}
                            placeholder="Core message or notes for AI"
                        />
                    </div>

                    <div className={styles.channels}>
                        <label>Channels</label>
                        <div className={styles.channelToggles}>
                            <button
                                className={`${styles.toggle} ${channels.includes('facebook') ? styles.active : ''}`}
                                onClick={() => toggleChannel('facebook')}
                                type="button"
                            >
                                Facebook
                            </button>
                            <button
                                className={`${styles.toggle} ${channels.includes('website') ? styles.active : ''}`}
                                onClick={() => toggleChannel('website')}
                                type="button"
                            >
                                Website
                            </button>
                        </div>
                        <div className={styles.apiConfig}>
                            <div className={styles.fieldRow}>
                                <label>Star News API URL</label>
                                <input
                                    type="text"
                                    value={apiUrl}
                                    onChange={(e) => setApiUrl(e.target.value)}
                                    placeholder="https://getstarnews.com/router.php?endpoint=posts"
                                />
                            </div>
                            <div className={styles.fieldRow}>
                                <label>Star News API Key (Bearer)</label>
                                <input
                                    type="password"
                                    value={apiKey}
                                    onChange={(e) => setApiKey(e.target.value)}
                                    placeholder="Authorization: Bearer ..."
                                />
                            </div>
                        </div>
                    </div>

                    <div className={styles.media}>
                        <div className={styles.mediaInput}>
                            <label>Attach Media (URL or path)</label>
                            <div className={styles.mediaRow}>
                                <input
                                    type="text"
                                    value={mediaUrl}
                                    onChange={(e) => setMediaUrl(e.target.value)}
                                    placeholder="https://... or C:\\media\\clip.mp4"
                                />
                                <button onClick={addMedia} type="button">
                                    <ImageIcon size={16} /> Add
                                </button>
                            </div>
                        </div>
                        <div className={styles.mediaList}>
                            {mediaList.length === 0 ? (
                                <div className={styles.empty}>No media attached</div>
                            ) : (
                                mediaList.map((m, idx) => (
                                    <div key={idx} className={styles.mediaItem}>
                                        <span>{m}</span>
                                        <button onClick={() => setMediaList(prev => prev.filter((_, i) => i !== idx))}>
                                            <Trash2 size={14} />
                                        </button>
                                    </div>
                                ))
                            )}
                        </div>
                    </div>

                    <div className={styles.aiRow}>
                        <div>
                            <label>AI Suggestions</label>
                            <p className={styles.hint}>Generate 2-5 drafts; pick one and refine if needed.</p>
                        </div>
                        <button className={styles.sparkBtn} onClick={handleGenerate} type="button">
                            <Sparkles size={16} /> Suggest Posts
                        </button>
                    </div>

                    {suggestions.length > 0 && (
                        <div className={styles.suggestions}>
                            {suggestions.map(s => (
                                <button
                                    key={s.id}
                                    className={`${styles.suggestion} ${selectedSuggestion === s.id ? styles.active : ''}`}
                                    onClick={() => setSelectedSuggestion(s.id)}
                                    type="button"
                                >
                                    <div>{s.text}</div>
                                    {s.mediaHint && <small>{s.mediaHint}</small>}
                                </button>
                            ))}
                        </div>
                    )}

                    <div className={styles.preview}>
                        <div className={styles.previewHeader}>Preview</div>
                        <div className={styles.previewBody}>
                            <div className={styles.previewChannels}>{channels.join(' â€¢ ') || 'Select channels'}</div>
                            <div className={styles.previewText}>{selectedText || 'Your message will appear here.'}</div>
                            {mediaList.length > 0 && (
                                <div className={styles.previewMedia}>Media attached: {mediaList.length}</div>
                            )}
                        </div>
                    </div>

                    <div className={styles.scheduleRow}>
                        <div>
                            <label>Schedule (optional)</label>
                            <div className={styles.scheduleInput}>
                                <CalendarClock size={16} />
                                <input
                                    type="datetime-local"
                                    value={scheduledAt}
                                    onChange={(e) => setScheduledAt(e.target.value)}
                                />
                            </div>
                        </div>
                        <div className={styles.actions}>
                            <button
                                className={styles.secondary}
                                onClick={() => handleSubmit('now')}
                                disabled={busy}
                                type="button"
                            >
                                {busy ? <Loader2 className={styles.spin} size={16} /> : <Send size={16} />} Post Now
                            </button>
                            <button
                                className={styles.primary}
                                onClick={() => handleSubmit('schedule')}
                                disabled={busy}
                                type="button"
                            >
                                Schedule
                            </button>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    );
}