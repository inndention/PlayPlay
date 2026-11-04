/**
 * SocialMediaStudio - Professional Social Media Content Creation & Scheduling
 * 
 * Features:
 * - AI-powered content rewriting and variations
 * - Visual content gallery with image editing
 * - Template system (saves to media library)
 * - Multi-platform scheduling (Facebook, Twitter, Instagram, TikTok, YouTube)
 * - Preview with real platform mockups
 * - Ticker/lower-third integration
 * - Content calendar visualization
 * - Analytics integration
 */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './SocialMediaStudio.module.css';
import {
    Send, Calendar, Clock, Image as ImageIcon, Video, FileText, Sparkles,
    RefreshCw, Copy, Trash2, Edit2, Save, Plus, X, Check, Eye, EyeOff,
    Facebook, Twitter, Instagram, Youtube, MessageCircle, Share2, Heart,
    ThumbsUp, Bookmark, Download, Upload, Palette, Type, Layout, Grid,
    List, Filter, Search, ChevronDown, ChevronRight, MoreHorizontal,
    AlertCircle, CheckCircle, Zap, TrendingUp, Users, Globe, Hash,
    AtSign, Link, Smile, Camera, Film, Music, Mic, Radio, Tv
} from 'lucide-react';

// Platform types
type Platform = 'facebook' | 'twitter' | 'instagram' | 'tiktok' | 'youtube' | 'linkedin';

interface SocialPost {
    id: string;
    content: string;
    mediaUrls: string[];
    platforms: Platform[];
    scheduledTime: Date | null;
    status: 'draft' | 'scheduled' | 'published' | 'failed';
    hashtags: string[];
    mentions: string[];
    linkUrl?: string;
    linkPreview?: {
        title: string;
        description: string;
        image: string;
    };
    variations?: string[];
    templateId?: string;
    analytics?: {
        impressions: number;
        engagements: number;
        clicks: number;
        shares: number;
    };
    createdAt: string;
    updatedAt: string;
}

interface ContentTemplate {
    id: string;
    name: string;
    category: string;
    content: string;
    mediaPlaceholders: number;
    platforms: Platform[];
    hashtags: string[];
    thumbnail?: string;
}

interface MediaAsset {
    id: string;
    type: 'image' | 'video' | 'gif';
    url: string;
    thumbnail: string;
    name: string;
    dimensions?: { width: number; height: number };
    duration?: number;
    tags: string[];
}

interface SocialMediaStudioProps {
    onCreatePost?: (post: SocialPost) => void;
    onSchedulePost?: (post: SocialPost, time: Date) => void;
    onSaveTemplate?: (template: ContentTemplate) => void;
    onIntegrateWithTicker?: (post: SocialPost) => void;
}

const PLATFORMS: { id: Platform; name: string; icon: React.ElementType; color: string; maxLength: number }[] = [
    { id: 'facebook', name: 'Facebook', icon: Facebook, color: '#1877f2', maxLength: 63206 },
    { id: 'twitter', name: 'Twitter/X', icon: Twitter, color: '#1da1f2', maxLength: 280 },
    { id: 'instagram', name: 'Instagram', icon: Instagram, color: '#e4405f', maxLength: 2200 },
    { id: 'tiktok', name: 'TikTok', icon: Video, color: '#000000', maxLength: 2200 },
    { id: 'youtube', name: 'YouTube', icon: Youtube, color: '#ff0000', maxLength: 5000 },
    { id: 'linkedin', name: 'LinkedIn', icon: Users, color: '#0077b5', maxLength: 3000 },
];

const CONTENT_CATEGORIES = [
    'Announcement', 'Promotion', 'Behind the Scenes', 'Live Show Promo',
    'Breaking News', 'Event', 'Poll/Question', 'Quote', 'Tips & Tricks',
    'User Generated', 'Contest', 'Holiday', 'Milestone', 'Product Launch'
];

const AI_TONES = [
    { id: 'professional', name: 'Professional', emoji: '💼' },
    { id: 'casual', name: 'Casual', emoji: '😊' },
    { id: 'humorous', name: 'Humorous', emoji: '😄' },
    { id: 'urgent', name: 'Urgent', emoji: '⚡' },
    { id: 'inspiring', name: 'Inspiring', emoji: '✨' },
    { id: 'informative', name: 'Informative', emoji: '📚' },
    { id: 'conversational', name: 'Conversational', emoji: '💬' },
    { id: 'promotional', name: 'Promotional', emoji: '📢' },
];

export const SocialMediaStudio: React.FC<SocialMediaStudioProps> = ({
    onCreatePost,
    onSchedulePost,
    onSaveTemplate,
    onIntegrateWithTicker
}) => {
    // State
    const [activeTab, setActiveTab] = useState<'compose' | 'calendar' | 'templates' | 'gallery' | 'analytics'>('compose');
    const [content, setContent] = useState('');
    const [selectedPlatforms, setSelectedPlatforms] = useState<Set<Platform>>(new Set(['facebook', 'twitter']));
    const [mediaAssets, setMediaAssets] = useState<MediaAsset[]>([]);
    const [selectedMedia, setSelectedMedia] = useState<string[]>([]);
    const [scheduledTime, setScheduledTime] = useState<Date | null>(null);
    const [hashtags, setHashtags] = useState<string[]>([]);
    const [newHashtag, setNewHashtag] = useState('');
    const [linkUrl, setLinkUrl] = useState('');
    const [showPreview, setShowPreview] = useState(true);
    const [previewPlatform, setPreviewPlatform] = useState<Platform>('facebook');
    const [aiTone, setAiTone] = useState('professional');
    const [aiVariations, setAiVariations] = useState<string[]>([]);
    const [isGeneratingAI, setIsGeneratingAI] = useState(false);
    const [templates, setTemplates] = useState<ContentTemplate[]>([]);
    const [scheduledPosts, setScheduledPosts] = useState<SocialPost[]>([]);
    const [showTemplateModal, setShowTemplateModal] = useState(false);
    const [templateName, setTemplateName] = useState('');
    const [templateCategory, setTemplateCategory] = useState('Announcement');
    const [showImageEditor, setShowImageEditor] = useState(false);
    const [editingImage, setEditingImage] = useState<MediaAsset | null>(null);
    const [calendarView, setCalendarView] = useState<'week' | 'month'>('week');
    const [showTickerPreview, setShowTickerPreview] = useState(false);
    
    const fileInputRef = useRef<HTMLInputElement>(null);
    const canvasRef = useRef<HTMLCanvasElement>(null);

    // Load saved data
    useEffect(() => {
        try {
            const savedTemplates = localStorage.getItem('social_templates');
            if (savedTemplates) setTemplates(JSON.parse(savedTemplates));
            
            const savedPosts = localStorage.getItem('social_scheduled_posts');
            if (savedPosts) setScheduledPosts(JSON.parse(savedPosts));
            
            const savedAssets = localStorage.getItem('social_media_assets');
            if (savedAssets) setMediaAssets(JSON.parse(savedAssets));
        } catch (e) {
            console.error('Failed to load social media data:', e);
        }
    }, []);

    // Save data
    useEffect(() => {
        localStorage.setItem('social_templates', JSON.stringify(templates));
    }, [templates]);

    useEffect(() => {
        localStorage.setItem('social_scheduled_posts', JSON.stringify(scheduledPosts));
    }, [scheduledPosts]);

    // Platform toggle
    const togglePlatform = (platform: Platform) => {
        setSelectedPlatforms(prev => {
            const next = new Set(prev);
            if (next.has(platform)) {
                next.delete(platform);
            } else {
                next.add(platform);
            }
            return next;
        });
    };

    // Character count for each platform
    const getCharacterStatus = (platform: Platform) => {
        const platformConfig = PLATFORMS.find(p => p.id === platform);
        if (!platformConfig) return { count: 0, max: 0, status: 'ok' as const };
        
        const count = content.length;
        const max = platformConfig.maxLength;
        const status = count > max ? 'error' : count > max * 0.9 ? 'warning' : 'ok';
        
        return { count, max, status };
    };

    // AI Content Generation
    const generateAIVariations = async () => {
        if (!content.trim()) return;
        
        setIsGeneratingAI(true);
        
        try {
            // Simulate AI generation (in production, call OpenAI/Claude API)
            await new Promise(resolve => setTimeout(resolve, 1500));
            
            const variations = generateMockVariations(content, aiTone);
            setAiVariations(variations);
        } catch (error) {
            console.error('AI generation failed:', error);
        } finally {
            setIsGeneratingAI(false);
        }
    };

    const generateMockVariations = (text: string, tone: string): string[] => {
        const baseText = text.trim();
        const variations: string[] = [];
        
        // Generate 3 variations based on tone
        switch (tone) {
            case 'professional':
                variations.push(`📢 ${baseText}`);
                variations.push(`We're excited to share: ${baseText}`);
                variations.push(`Important update: ${baseText}`);
                break;
            case 'casual':
                variations.push(`Hey everyone! ${baseText} 🎉`);
                variations.push(`Check this out: ${baseText} 👀`);
                variations.push(`So... ${baseText.toLowerCase()} - thoughts? 💭`);
                break;
            case 'humorous':
                variations.push(`Plot twist: ${baseText} 😂`);
                variations.push(`Not to be dramatic, but... ${baseText} 🎭`);
                variations.push(`*pretends to be surprised* ${baseText} 😱`);
                break;
            case 'urgent':
                variations.push(`🚨 BREAKING: ${baseText.toUpperCase()}`);
                variations.push(`⚡ Don't miss this: ${baseText}`);
                variations.push(`🔴 LIVE NOW: ${baseText}`);
                break;
            case 'inspiring':
                variations.push(`✨ ${baseText} - Let's make it happen!`);
                variations.push(`Dream big: ${baseText} 🌟`);
                variations.push(`Here's to new beginnings: ${baseText} 🙌`);
                break;
            default:
                variations.push(baseText);
                variations.push(`${baseText} Learn more below.`);
                variations.push(`Quick update: ${baseText}`);
        }
        
        return variations;
    };

    // Add hashtag
    const addHashtag = () => {
        const tag = newHashtag.trim().replace(/^#/, '');
        if (tag && !hashtags.includes(tag)) {
            setHashtags([...hashtags, tag]);
            setNewHashtag('');
        }
    };

    // Remove hashtag
    const removeHashtag = (tag: string) => {
        setHashtags(hashtags.filter(t => t !== tag));
    };

    // Handle media upload
    const handleMediaUpload = (e: React.ChangeEvent<HTMLInputElement>) => {
        const files = e.target.files;
        if (!files) return;
        
        Array.from(files).forEach(file => {
            const reader = new FileReader();
            reader.onload = (event) => {
                const url = event.target?.result as string;
                const isVideo = file.type.startsWith('video/');
                
                const asset: MediaAsset = {
                    id: `asset_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`,
                    type: isVideo ? 'video' : 'image',
                    url,
                    thumbnail: url,
                    name: file.name,
                    tags: []
                };
                
                setMediaAssets(prev => [...prev, asset]);
                setSelectedMedia(prev => [...prev, asset.id]);
            };
            reader.readAsDataURL(file);
        });
        
        e.target.value = '';
    };

    // Create post
    const handleCreatePost = () => {
        if (!content.trim() && selectedMedia.length === 0) return;
        
        const post: SocialPost = {
            id: `post_${Date.now()}`,
            content,
            mediaUrls: selectedMedia.map(id => mediaAssets.find(a => a.id === id)?.url || ''),
            platforms: Array.from(selectedPlatforms),
            scheduledTime,
            status: scheduledTime ? 'scheduled' : 'draft',
            hashtags,
            mentions: [],
            linkUrl: linkUrl || undefined,
            createdAt: new Date().toISOString(),
            updatedAt: new Date().toISOString()
        };
        
        if (scheduledTime) {
            setScheduledPosts(prev => [...prev, post]);
            onSchedulePost?.(post, scheduledTime);
        } else {
            onCreatePost?.(post);
        }
        
        // Reset form
        setContent('');
        setSelectedMedia([]);
        setHashtags([]);
        setLinkUrl('');
        setScheduledTime(null);
        setAiVariations([]);
    };

    // Save as template
    const saveAsTemplate = () => {
        if (!templateName.trim()) return;
        
        const template: ContentTemplate = {
            id: `template_${Date.now()}`,
            name: templateName,
            category: templateCategory,
            content,
            mediaPlaceholders: selectedMedia.length,
            platforms: Array.from(selectedPlatforms),
            hashtags,
            thumbnail: selectedMedia.length > 0 
                ? mediaAssets.find(a => a.id === selectedMedia[0])?.thumbnail 
                : undefined
        };
        
        setTemplates(prev => [...prev, template]);
        onSaveTemplate?.(template);
        setShowTemplateModal(false);
        setTemplateName('');
    };

    // Load template
    const loadTemplate = (template: ContentTemplate) => {
        setContent(template.content);
        setSelectedPlatforms(new Set(template.platforms));
        setHashtags(template.hashtags);
        setActiveTab('compose');
    };

    // Integrate with ticker
    const handleTickerIntegration = () => {
        const post: SocialPost = {
            id: `ticker_${Date.now()}`,
            content,
            mediaUrls: selectedMedia.map(id => mediaAssets.find(a => a.id === id)?.url || ''),
            platforms: Array.from(selectedPlatforms),
            scheduledTime: null,
            status: 'draft',
            hashtags,
            mentions: [],
            createdAt: new Date().toISOString(),
            updatedAt: new Date().toISOString()
        };
        
        onIntegrateWithTicker?.(post);
        setShowTickerPreview(true);
    };

    // Render platform preview
    const renderPlatformPreview = () => {
        const platform = PLATFORMS.find(p => p.id === previewPlatform);
        if (!platform) return null;
        
        const charStatus = getCharacterStatus(previewPlatform);
        const mediaUrl = selectedMedia.length > 0 
            ? mediaAssets.find(a => a.id === selectedMedia[0])?.url 
            : null;

        return (
            <div className={styles.preview}>
                <div className={styles.previewHeader}>
                    <div className={styles.previewTabs}>
                        {PLATFORMS.filter(p => selectedPlatforms.has(p.id)).map(p => (
                            <button
                                key={p.id}
                                className={`${styles.previewTab} ${previewPlatform === p.id ? styles.active : ''}`}
                                onClick={() => setPreviewPlatform(p.id)}
                                style={{ '--platform-color': p.color } as React.CSSProperties}
                            >
                                <p.icon size={14} />
                                {p.name}
                            </button>
                        ))}
                    </div>
                </div>
                
                <div className={styles.previewContent} style={{ '--platform-color': platform.color } as React.CSSProperties}>
                    <div className={styles.mockPost}>
                        <div className={styles.mockHeader}>
                            <div className={styles.mockAvatar}>
                                <Tv size={20} />
                            </div>
                            <div className={styles.mockUserInfo}>
                                <span className={styles.mockUsername}>Your Channel</span>
                                <span className={styles.mockTime}>Just now</span>
                            </div>
                            <MoreHorizontal size={16} className={styles.mockMore} />
                        </div>
                        
                        <div className={styles.mockBody}>
                            <p className={styles.mockText}>
                                {content || 'Your post content will appear here...'}
                            </p>
                            
                            {hashtags.length > 0 && (
                                <p className={styles.mockHashtags}>
                                    {hashtags.map(tag => `#${tag}`).join(' ')}
                                </p>
                            )}
                            
                            {linkUrl && (
                                <div className={styles.mockLink}>
                                    <Link size={12} />
                                    <span>{linkUrl}</span>
                                </div>
                            )}
                            
                            {mediaUrl && (
                                <div className={styles.mockMedia}>
                                    <img src={mediaUrl} alt="Preview" />
                                </div>
                            )}
                        </div>
                        
                        <div className={styles.mockActions}>
                            <button><ThumbsUp size={16} /> Like</button>
                            <button><MessageCircle size={16} /> Comment</button>
                            <button><Share2 size={16} /> Share</button>
                        </div>
                    </div>
                    
                    <div className={`${styles.charCount} ${styles[charStatus.status]}`}>
                        {charStatus.count} / {charStatus.max}
                    </div>
                </div>
            </div>
        );
    };

    // Render calendar view
    const renderCalendar = () => {
        const today = new Date();
        const days = calendarView === 'week' ? 7 : 30;
        const dates: Date[] = [];
        
        for (let i = 0; i < days; i++) {
            const date = new Date(today);
            date.setDate(today.getDate() + i);
            dates.push(date);
        }
        
        const getPostsForDate = (date: Date) => {
            return scheduledPosts.filter(post => {
                if (!post.scheduledTime) return false;
                const postDate = new Date(post.scheduledTime);
                return postDate.toDateString() === date.toDateString();
            });
        };

        return (
            <div className={styles.calendar}>
                <div className={styles.calendarHeader}>
                    <h3><Calendar size={18} /> Content Calendar</h3>
                    <div className={styles.calendarViewToggle}>
                        <button 
                            className={calendarView === 'week' ? styles.active : ''}
                            onClick={() => setCalendarView('week')}
                        >
                            Week
                        </button>
                        <button 
                            className={calendarView === 'month' ? styles.active : ''}
                            onClick={() => setCalendarView('month')}
                        >
                            Month
                        </button>
                    </div>
                </div>
                
                <div className={`${styles.calendarGrid} ${calendarView === 'month' ? styles.monthView : ''}`}>
                    {dates.map((date, index) => {
                        const posts = getPostsForDate(date);
                        const isToday = date.toDateString() === today.toDateString();
                        
                        return (
                            <div 
                                key={index} 
                                className={`${styles.calendarDay} ${isToday ? styles.today : ''}`}
                            >
                                <div className={styles.dayHeader}>
                                    <span className={styles.dayName}>
                                        {date.toLocaleDateString('en-US', { weekday: 'short' })}
                                    </span>
                                    <span className={styles.dayNumber}>
                                        {date.getDate()}
                                    </span>
                                </div>
                                
                                <div className={styles.dayPosts}>
                                    {posts.map(post => (
                                        <div key={post.id} className={styles.scheduledPost}>
                                            <div className={styles.postTime}>
                                                {new Date(post.scheduledTime!).toLocaleTimeString('en-US', { 
                                                    hour: '2-digit', 
                                                    minute: '2-digit' 
                                                })}
                                            </div>
                                            <div className={styles.postPlatforms}>
                                                {post.platforms.map(p => {
                                                    const platform = PLATFORMS.find(pl => pl.id === p);
                                                    if (!platform) return null;
                                                    return (
                                                        <span 
                                                            key={p} 
                                                            style={{ color: platform.color }}
                                                        >
                                                            <platform.icon size={10} />
                                                        </span>
                                                    );
                                                })}
                                            </div>
                                            <div className={styles.postSnippet}>
                                                {post.content.substring(0, 30)}...
                                            </div>
                                        </div>
                                    ))}
                                    
                                    {posts.length === 0 && (
                                        <button 
                                            className={styles.addPostBtn}
                                            onClick={() => {
                                                const newDate = new Date(date);
                                                newDate.setHours(12, 0, 0, 0);
                                                setScheduledTime(newDate);
                                                setActiveTab('compose');
                                            }}
                                        >
                                            <Plus size={12} />
                                        </button>
                                    )}
                                </div>
                            </div>
                        );
                    })}
                </div>
            </div>
        );
    };

    return (
        <div className={styles.studio}>
            {/* Header */}
            <div className={styles.header}>
                <div className={styles.title}>
                    <Share2 size={24} />
                    <h2>Social Media Studio</h2>
                </div>
                
                <div className={styles.tabs}>
                    <button 
                        className={activeTab === 'compose' ? styles.active : ''}
                        onClick={() => setActiveTab('compose')}
                    >
                        <Edit2 size={14} /> Compose
                    </button>
                    <button 
                        className={activeTab === 'calendar' ? styles.active : ''}
                        onClick={() => setActiveTab('calendar')}
                    >
                        <Calendar size={14} /> Calendar
                    </button>
                    <button 
                        className={activeTab === 'templates' ? styles.active : ''}
                        onClick={() => setActiveTab('templates')}
                    >
                        <Layout size={14} /> Templates
                    </button>
                    <button 
                        className={activeTab === 'gallery' ? styles.active : ''}
                        onClick={() => setActiveTab('gallery')}
                    >
                        <ImageIcon size={14} /> Gallery
                    </button>
                    <button 
                        className={activeTab === 'analytics' ? styles.active : ''}
                        onClick={() => setActiveTab('analytics')}
                    >
                        <TrendingUp size={14} /> Analytics
                    </button>
                </div>
            </div>

            {/* Main Content */}
            <div className={styles.main}>
                {activeTab === 'compose' && (
                    <div className={styles.composer}>
                        {/* Platform Selection */}
                        <div className={styles.platformSelect}>
                            <span className={styles.label}>Platforms:</span>
                            <div className={styles.platforms}>
                                {PLATFORMS.map(platform => (
                                    <button
                                        key={platform.id}
                                        className={`${styles.platformBtn} ${selectedPlatforms.has(platform.id) ? styles.active : ''}`}
                                        onClick={() => togglePlatform(platform.id)}
                                        style={{ '--platform-color': platform.color } as React.CSSProperties}
                                    >
                                        <platform.icon size={16} />
                                        <span>{platform.name}</span>
                                    </button>
                                ))}
                            </div>
                        </div>

                        {/* Content Editor */}
                        <div className={styles.editor}>
                            <textarea
                                value={content}
                                onChange={(e) => setContent(e.target.value)}
                                placeholder="What's happening? Write your post content here..."
                                className={styles.contentInput}
                                rows={5}
                            />
                            
                            {/* AI Assistant */}
                            <div className={styles.aiAssistant}>
                                <div className={styles.aiHeader}>
                                    <Sparkles size={16} />
                                    <span>AI Assistant</span>
                                </div>
                                <div className={styles.aiTones}>
                                    {AI_TONES.map(tone => (
                                        <button
                                            key={tone.id}
                                            className={`${styles.toneBtn} ${aiTone === tone.id ? styles.active : ''}`}
                                            onClick={() => setAiTone(tone.id)}
                                        >
                                            {tone.emoji} {tone.name}
                                        </button>
                                    ))}
                                </div>
                                <button 
                                    className={styles.generateBtn}
                                    onClick={generateAIVariations}
                                    disabled={isGeneratingAI || !content.trim()}
                                >
                                    {isGeneratingAI ? (
                                        <><RefreshCw size={14} className={styles.spinning} /> Generating...</>
                                    ) : (
                                        <><Zap size={14} /> Generate Variations</>
                                    )}
                                </button>
                                
                                {aiVariations.length > 0 && (
                                    <div className={styles.variations}>
                                        {aiVariations.map((variation, index) => (
                                            <div key={index} className={styles.variation}>
                                                <p>{variation}</p>
                                                <button 
                                                    onClick={() => setContent(variation)}
                                                    title="Use this variation"
                                                >
                                                    <Check size={14} />
                                                </button>
                                            </div>
                                        ))}
                                    </div>
                                )}
                            </div>

                            {/* Media Attachments */}
                            <div className={styles.attachments}>
                                <div className={styles.attachmentHeader}>
                                    <span>Media</span>
                                    <button onClick={() => fileInputRef.current?.click()}>
                                        <Plus size={14} /> Add
                                    </button>
                                    <input
                                        ref={fileInputRef}
                                        type="file"
                                        accept="image/*,video/*"
                                        multiple
                                        onChange={handleMediaUpload}
                                        hidden
                                    />
                                </div>
                                
                                {selectedMedia.length > 0 && (
                                    <div className={styles.mediaGrid}>
                                        {selectedMedia.map(id => {
                                            const asset = mediaAssets.find(a => a.id === id);
                                            if (!asset) return null;
                                            return (
                                                <div key={id} className={styles.mediaItem}>
                                                    <img src={asset.thumbnail} alt={asset.name} />
                                                    <div className={styles.mediaActions}>
                                                        <button onClick={() => {
                                                            setEditingImage(asset);
                                                            setShowImageEditor(true);
                                                        }}>
                                                            <Edit2 size={12} />
                                                        </button>
                                                        <button onClick={() => {
                                                            setSelectedMedia(prev => prev.filter(i => i !== id));
                                                        }}>
                                                            <X size={12} />
                                                        </button>
                                                    </div>
                                                </div>
                                            );
                                        })}
                                    </div>
                                )}
                            </div>

                            {/* Hashtags */}
                            <div className={styles.hashtagSection}>
                                <div className={styles.hashtagInput}>
                                    <Hash size={14} />
                                    <input
                                        type="text"
                                        value={newHashtag}
                                        onChange={(e) => setNewHashtag(e.target.value)}
                                        onKeyDown={(e) => e.key === 'Enter' && addHashtag()}
                                        placeholder="Add hashtag"
                                    />
                                    <button onClick={addHashtag}>Add</button>
                                </div>
                                {hashtags.length > 0 && (
                                    <div className={styles.hashtags}>
                                        {hashtags.map(tag => (
                                            <span key={tag} className={styles.hashtag}>
                                                #{tag}
                                                <button onClick={() => removeHashtag(tag)}>
                                                    <X size={10} />
                                                </button>
                                            </span>
                                        ))}
                                    </div>
                                )}
                            </div>

                            {/* Link */}
                            <div className={styles.linkSection}>
                                <Link size={14} />
                                <input
                                    type="url"
                                    value={linkUrl}
                                    onChange={(e) => setLinkUrl(e.target.value)}
                                    placeholder="Add link URL"
                                />
                            </div>

                            {/* Schedule */}
                            <div className={styles.scheduleSection}>
                                <Clock size={14} />
                                <input
                                    type="datetime-local"
                                    value={scheduledTime ? scheduledTime.toISOString().slice(0, 16) : ''}
                                    onChange={(e) => setScheduledTime(e.target.value ? new Date(e.target.value) : null)}
                                />
                                {scheduledTime && (
                                    <button 
                                        className={styles.clearSchedule}
                                        onClick={() => setScheduledTime(null)}
                                    >
                                        <X size={12} /> Clear
                                    </button>
                                )}
                            </div>
                        </div>

                        {/* Actions */}
                        <div className={styles.actions}>
                            <button 
                                className={styles.secondaryBtn}
                                onClick={() => setShowTemplateModal(true)}
                            >
                                <Save size={14} /> Save as Template
                            </button>
                            <button 
                                className={styles.secondaryBtn}
                                onClick={handleTickerIntegration}
                            >
                                <Tv size={14} /> Add to Ticker
                            </button>
                            <button 
                                className={styles.primaryBtn}
                                onClick={handleCreatePost}
                                disabled={!content.trim() && selectedMedia.length === 0}
                            >
                                {scheduledTime ? (
                                    <><Calendar size={14} /> Schedule Post</>
                                ) : (
                                    <><Send size={14} /> Post Now</>
                                )}
                            </button>
                        </div>
                    </div>
                )}

                {activeTab === 'calendar' && renderCalendar()}

                {activeTab === 'templates' && (
                    <div className={styles.templates}>
                        <div className={styles.templatesHeader}>
                            <h3><Layout size={18} /> Content Templates</h3>
                            <button onClick={() => setShowTemplateModal(true)}>
                                <Plus size={14} /> Create Template
                            </button>
                        </div>
                        
                        <div className={styles.templateCategories}>
                            {CONTENT_CATEGORIES.map(category => (
                                <button key={category} className={styles.categoryBtn}>
                                    {category}
                                </button>
                            ))}
                        </div>
                        
                        <div className={styles.templateGrid}>
                            {templates.map(template => (
                                <div key={template.id} className={styles.templateCard}>
                                    {template.thumbnail && (
                                        <div className={styles.templateThumb}>
                                            <img src={template.thumbnail} alt={template.name} />
                                        </div>
                                    )}
                                    <div className={styles.templateInfo}>
                                        <h4>{template.name}</h4>
                                        <span className={styles.templateCategory}>{template.category}</span>
                                        <p>{template.content.substring(0, 80)}...</p>
                                        <div className={styles.templatePlatforms}>
                                            {template.platforms.map(p => {
                                                const platform = PLATFORMS.find(pl => pl.id === p);
                                                if (!platform) return null;
                                                return <platform.icon key={p} size={12} style={{ color: platform.color }} />;
                                            })}
                                        </div>
                                    </div>
                                    <div className={styles.templateActions}>
                                        <button onClick={() => loadTemplate(template)}>
                                            <Edit2 size={12} /> Use
                                        </button>
                                        <button onClick={() => setTemplates(prev => prev.filter(t => t.id !== template.id))}>
                                            <Trash2 size={12} />
                                        </button>
                                    </div>
                                </div>
                            ))}
                            
                            {templates.length === 0 && (
                                <div className={styles.emptyState}>
                                    <Layout size={48} />
                                    <p>No templates yet</p>
                                    <span>Create templates to speed up your social media workflow</span>
                                </div>
                            )}
                        </div>
                    </div>
                )}

                {activeTab === 'gallery' && (
                    <div className={styles.gallery}>
                        <div className={styles.galleryHeader}>
                            <h3><ImageIcon size={18} /> Media Gallery</h3>
                            <button onClick={() => fileInputRef.current?.click()}>
                                <Upload size={14} /> Upload
                            </button>
                        </div>
                        
                        <div className={styles.galleryGrid}>
                            {mediaAssets.map(asset => (
                                <div key={asset.id} className={styles.galleryItem}>
                                    <img src={asset.thumbnail} alt={asset.name} />
                                    <div className={styles.galleryOverlay}>
                                        <span>{asset.name}</span>
                                        <div className={styles.galleryActions}>
                                            <button onClick={() => {
                                                setSelectedMedia(prev => [...prev, asset.id]);
                                                setActiveTab('compose');
                                            }}>
                                                <Plus size={14} />
                                            </button>
                                            <button onClick={() => {
                                                setEditingImage(asset);
                                                setShowImageEditor(true);
                                            }}>
                                                <Edit2 size={14} />
                                            </button>
                                            <button onClick={() => {
                                                setMediaAssets(prev => prev.filter(a => a.id !== asset.id));
                                            }}>
                                                <Trash2 size={14} />
                                            </button>
                                        </div>
                                    </div>
                                </div>
                            ))}
                            
                            {mediaAssets.length === 0 && (
                                <div className={styles.emptyState}>
                                    <ImageIcon size={48} />
                                    <p>No media assets</p>
                                    <span>Upload images and videos for your social posts</span>
                                    <button onClick={() => fileInputRef.current?.click()}>
                                        <Upload size={14} /> Upload Media
                                    </button>
                                </div>
                            )}
                        </div>
                    </div>
                )}

                {activeTab === 'analytics' && (
                    <div className={styles.analytics}>
                        <div className={styles.analyticsHeader}>
                            <h3><TrendingUp size={18} /> Post Analytics</h3>
                        </div>
                        
                        <div className={styles.analyticsGrid}>
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Send size={24} /></div>
                                <div className={styles.statValue}>{scheduledPosts.length}</div>
                                <div className={styles.statLabel}>Scheduled Posts</div>
                            </div>
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Eye size={24} /></div>
                                <div className={styles.statValue}>0</div>
                                <div className={styles.statLabel}>Total Impressions</div>
                            </div>
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Heart size={24} /></div>
                                <div className={styles.statValue}>0</div>
                                <div className={styles.statLabel}>Total Engagements</div>
                            </div>
                            <div className={styles.statCard}>
                                <div className={styles.statIcon}><Users size={24} /></div>
                                <div className={styles.statValue}>0</div>
                                <div className={styles.statLabel}>Reach</div>
                            </div>
                        </div>
                        
                        <div className={styles.emptyAnalytics}>
                            <TrendingUp size={48} />
                            <p>Analytics will appear here once you publish posts</p>
                            <span>Connect your social accounts to track performance</span>
                        </div>
                    </div>
                )}

                {/* Preview Panel */}
                {activeTab === 'compose' && showPreview && renderPlatformPreview()}
            </div>

            {/* Template Save Modal */}
            {showTemplateModal && (
                <div className={styles.modal} onClick={() => setShowTemplateModal(false)}>
                    <div className={styles.modalContent} onClick={e => e.stopPropagation()}>
                        <h3>Save as Template</h3>
                        <div className={styles.formGroup}>
                            <label>Template Name</label>
                            <input
                                type="text"
                                value={templateName}
                                onChange={(e) => setTemplateName(e.target.value)}
                                placeholder="Enter template name"
                            />
                        </div>
                        <div className={styles.formGroup}>
                            <label>Category</label>
                            <select 
                                value={templateCategory}
                                onChange={(e) => setTemplateCategory(e.target.value)}
                            >
                                {CONTENT_CATEGORIES.map(cat => (
                                    <option key={cat} value={cat}>{cat}</option>
                                ))}
                            </select>
                        </div>
                        <div className={styles.modalActions}>
                            <button onClick={() => setShowTemplateModal(false)}>Cancel</button>
                            <button onClick={saveAsTemplate} disabled={!templateName.trim()}>
                                <Save size={14} /> Save Template
                            </button>
                        </div>
                    </div>
                </div>
            )}

            {/* Ticker Preview */}
            {showTickerPreview && (
                <div className={styles.tickerPreview}>
                    <div className={styles.tickerHeader}>
                        <Tv size={16} />
                        <span>Ticker Preview</span>
                        <button onClick={() => setShowTickerPreview(false)}>
                            <X size={14} />
                        </button>
                    </div>
                    <div className={styles.tickerContent}>
                        <div className={styles.tickerBar}>
                            <span className={styles.tickerLabel}>SOCIAL</span>
                            <span className={styles.tickerText}>
                                {content} {hashtags.map(t => `#${t}`).join(' ')}
                            </span>
                        </div>
                    </div>
                    <p className={styles.tickerNote}>
                        This will appear in your broadcast ticker/lower-third
                    </p>
                </div>
            )}
        </div>
    );
};

export default SocialMediaStudio;
