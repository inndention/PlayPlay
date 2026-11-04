// ============================================
// News Slate Generator
// Generate broadcast-quality news slates
// ============================================

import { NewsItem } from './newsFeedAggregator';
import { CGElement, CGType } from './types';
import { generateId } from './utils';

export type SlateStyle = 'breaking' | 'regular' | 'alert' | 'sports' | 'weather' | 'custom';

export interface SlateTemplate {
    id: string;
    name: string;
    style: SlateStyle;
    layout: {
        headline: {
            position: { x: number; y: number; width: number; height: number };
            fontSize: number;
            fontFamily: string;
            fontWeight: 'normal' | 'bold';
            color: string;
        };
        subheadline?: {
            position: { x: number; y: number; width: number; height: number };
            fontSize: number;
            fontFamily: string;
            color: string;
        };
        ticker?: {
            position: { x: number; y: number; width: number; height: number };
            fontSize: number;
            color: string;
            speed: number; // pixels per second
        };
        logo?: {
            position: { x: number; y: number; width: number; height: number };
            imageUrl?: string;
        };
        background?: {
            color: string;
            gradient?: { from: string; to: string; direction: 'horizontal' | 'vertical' };
            imageUrl?: string;
        };
    };
    animations?: {
        entrance: 'fade' | 'slide' | 'zoom' | 'none';
        exit: 'fade' | 'slide' | 'zoom' | 'none';
        duration: number; // milliseconds
    };
}

export interface GeneratedSlate {
    id: string;
    newsItem: NewsItem;
    template: SlateTemplate;
    cgElements: CGElement[];
    audioUrl?: string; // TTS audio URL
    duration: number; // seconds
    createdAt: number;
}

class NewsSlateGenerator {
    private templates: Map<string, SlateTemplate> = new Map();

    constructor() {
        this.initializeDefaultTemplates();
    }

    /**
     * Initialize default slate templates
     */
    private initializeDefaultTemplates(): void {
        // Breaking News Template
        const breakingTemplate: SlateTemplate = {
            id: 'breaking-default',
            name: 'Breaking News',
            style: 'breaking',
            layout: {
                headline: {
                    position: { x: 5, y: 20, width: 90, height: 25 },
                    fontSize: 72,
                    fontFamily: 'Arial Black',
                    fontWeight: 'bold',
                    color: '#ff0000',
                },
                subheadline: {
                    position: { x: 5, y: 50, width: 90, height: 20 },
                    fontSize: 48,
                    fontFamily: 'Arial',
                    color: '#ffffff',
                },
                ticker: {
                    position: { x: 0, y: 95, width: 100, height: 5 },
                    fontSize: 32,
                    color: '#ffff00',
                    speed: 100,
                },
                background: {
                    color: '#000000',
                    gradient: {
                        from: '#1a0000',
                        to: '#000000',
                        direction: 'vertical',
                    },
                },
            },
            animations: {
                entrance: 'zoom',
                exit: 'fade',
                duration: 500,
            },
        };

        // Regular News Template
        const regularTemplate: SlateTemplate = {
            id: 'regular-default',
            name: 'Regular News',
            style: 'regular',
            layout: {
                headline: {
                    position: { x: 10, y: 30, width: 80, height: 30 },
                    fontSize: 64,
                    fontFamily: 'Arial',
                    fontWeight: 'bold',
                    color: '#ffffff',
                },
                subheadline: {
                    position: { x: 10, y: 65, width: 80, height: 15 },
                    fontSize: 36,
                    fontFamily: 'Arial',
                    color: '#cccccc',
                },
                background: {
                    color: '#1a1a1a',
                },
            },
            animations: {
                entrance: 'slide',
                exit: 'fade',
                duration: 300,
            },
        };

        // Alert Template
        const alertTemplate: SlateTemplate = {
            id: 'alert-default',
            name: 'Alert',
            style: 'alert',
            layout: {
                headline: {
                    position: { x: 5, y: 35, width: 90, height: 30 },
                    fontSize: 68,
                    fontFamily: 'Arial Black',
                    fontWeight: 'bold',
                    color: '#ffaa00',
                },
                ticker: {
                    position: { x: 0, y: 90, width: 100, height: 10 },
                    fontSize: 28,
                    color: '#ffffff',
                    speed: 80,
                },
                background: {
                    color: '#2a1a00',
                },
            },
            animations: {
                entrance: 'zoom',
                exit: 'fade',
                duration: 400,
            },
        };

        this.templates.set(breakingTemplate.id, breakingTemplate);
        this.templates.set(regularTemplate.id, regularTemplate);
        this.templates.set(alertTemplate.id, alertTemplate);
    }

    /**
     * Generate a slate from a news item
     */
    public generateSlate(
        newsItem: NewsItem,
        templateId?: string,
        audioUrl?: string
    ): GeneratedSlate {
        // Select template based on urgency or provided ID
        const template = templateId
            ? this.templates.get(templateId)
            : this.selectTemplateForUrgency(newsItem.urgency);

        if (!template) {
            throw new Error(`Template not found: ${templateId || newsItem.urgency}`);
        }

        // Generate CG elements from template
        const cgElements = this.generateCGElements(newsItem, template);

        // Calculate duration (estimate based on text length)
        const duration = this.estimateDuration(newsItem, audioUrl);

        return {
            id: generateId(),
            newsItem,
            template,
            cgElements,
            audioUrl,
            duration,
            createdAt: Date.now(),
        };
    }

    /**
     * Select template based on urgency
     */
    private selectTemplateForUrgency(urgency: NewsItem['urgency']): SlateTemplate | undefined {
        switch (urgency) {
            case 'breaking':
                return this.templates.get('breaking-default');
            case 'high':
                return this.templates.get('alert-default');
            default:
                return this.templates.get('regular-default');
        }
    }

    /**
     * Generate CG elements from template and news item
     */
    private generateCGElements(newsItem: NewsItem, template: SlateTemplate): CGElement[] {
        const elements: CGElement[] = [];

        // Background
        if (template.layout.background) {
            const bg = template.layout.background;
            elements.push({
                id: generateId(),
                name: 'Slate Background',
                type: 'custom' as CGType,
                title: '',
                colors: {
                    text: bg.color,
                    primary: bg.gradient?.from || bg.color,
                    secondary: bg.gradient?.to || bg.color,
                    gradient: !!bg.gradient,
                },
                effects: {
                    shadow: false,
                    glow: false,
                    borderRadius: 0,
                    opacity: 1,
                    glassMorphism: false,
                },
                animationIn: template.animations?.entrance === 'fade' ? 'fade' : 'none',
                animationOut: template.animations?.exit === 'fade' ? 'fade' : 'none',
                animationDuration: template.animations?.duration || 500,
                loop: 'none',
                position: {
                    x: 0,
                    y: 0,
                    width: 100,
                    height: 100,
                    anchor: 'top-left',
                },
                fontSize: 0,
                fontFamily: '',
                isVisible: true,
                isPreset: false,
            });
        }

        // Headline
        const headline = template.layout.headline;
        elements.push({
            id: generateId(),
            name: 'Headline',
            type: 'lower_third' as CGType,
            title: newsItem.title,
            colors: {
                text: headline.color,
                primary: headline.color,
                secondary: headline.color,
                gradient: false,
            },
            effects: {
                shadow: true,
                shadowColor: '#000000',
                shadowBlur: 4,
                glow: headline.fontWeight === 'bold',
                glowColor: headline.color,
                glowIntensity: 0.5,
                borderRadius: 0,
                opacity: 1,
                glassMorphism: false,
            },
            animationIn: template.animations?.entrance === 'zoom' ? 'zoom' : 
                       template.animations?.entrance === 'slide' ? 'slide_left' : 'fade',
            animationOut: template.animations?.exit === 'fade' ? 'fade' : 'none',
            animationDuration: template.animations?.duration || 500,
            loop: 'none',
            position: { ...headline.position, anchor: 'top-left' as const },
            fontSize: headline.fontSize,
            fontFamily: headline.fontFamily,
            isVisible: true,
            isPreset: false,
        });

        // Subheadline
        if (template.layout.subheadline && newsItem.summary) {
            const subheadline = template.layout.subheadline;
            elements.push({
                id: generateId(),
                name: 'Subheadline',
                type: 'lower_third' as CGType,
                title: newsItem.summary.substring(0, 150), // Limit length
                colors: {
                    text: subheadline.color,
                    primary: subheadline.color,
                    secondary: subheadline.color,
                    gradient: false,
                },
            effects: {
                shadow: true,
                shadowColor: '#000000',
                shadowBlur: 2,
                glow: false,
                borderRadius: 0,
                opacity: 1,
                glassMorphism: false,
            },
            animationIn: 'fade',
            animationOut: 'fade',
            animationDuration: 300,
            loop: 'none',
                position: { ...subheadline.position, anchor: 'top-left' as const },
                fontSize: subheadline.fontSize,
                fontFamily: subheadline.fontFamily,
                isVisible: true,
                isPreset: false,
            });
        }

        // Ticker
        if (template.layout.ticker) {
            const ticker = template.layout.ticker;
            const tickerText = `${newsItem.source} - ${newsItem.title}${newsItem.summary ? ` - ${newsItem.summary}` : ''}`;
            elements.push({
                id: generateId(),
                name: 'News Ticker',
                type: 'ticker' as CGType,
                title: tickerText,
                tickerText: tickerText,
                colors: {
                    text: ticker.color,
                    primary: ticker.color,
                    secondary: ticker.color,
                    gradient: false,
                },
                effects: {
                    shadow: false,
                    glow: false,
                    borderRadius: 0,
                    opacity: 1,
                    glassMorphism: false,
                },
                animationIn: 'slide_left',
                animationOut: 'slide_left',
                animationDuration: 500,
                loop: 'none',
                position: { ...ticker.position, anchor: 'bottom-left' as const },
                fontSize: ticker.fontSize,
                fontFamily: 'Arial',
                isVisible: true,
                isPreset: false,
            });
        }

        // Logo/Image
        if (template.layout.logo && newsItem.imageUrl) {
            const logo = template.layout.logo;
            elements.push({
                id: generateId(),
                name: 'News Image',
                type: 'bug' as CGType,
                title: '',
                imageSrc: newsItem.imageUrl,
                colors: {
                    text: '#ffffff',
                    primary: '#ffffff',
                    secondary: '#ffffff',
                    gradient: false,
                },
                effects: {
                    shadow: true,
                    shadowBlur: 8,
                    glow: false,
                    borderRadius: 4,
                    opacity: 1,
                    glassMorphism: false,
                },
                animationIn: 'fade',
                animationOut: 'fade',
                animationDuration: 300,
                loop: 'none',
                position: { ...logo.position, anchor: 'top-right' as const },
                fontSize: 0,
                fontFamily: '',
                isVisible: true,
                isPreset: false,
            });
        }

        return elements;
    }

    /**
     * Estimate slate duration
     */
    private estimateDuration(newsItem: NewsItem, audioUrl?: string): number {
        // If audio provided, use its duration (would need to be calculated separately)
        if (audioUrl) {
            // Default to 15 seconds for audio slates
            return 15;
        }

        // Estimate based on text length (average reading speed: 150 words/minute)
        const text = `${newsItem.title} ${newsItem.summary || ''}`;
        const wordCount = text.split(/\s+/).length;
        const readingTime = (wordCount / 150) * 60; // seconds

        // Minimum 5 seconds, maximum 30 seconds
        return Math.max(5, Math.min(30, Math.ceil(readingTime)));
    }

    /**
     * Add custom template
     */
    public addTemplate(template: SlateTemplate): void {
        this.templates.set(template.id, template);
    }

    /**
     * Get template
     */
    public getTemplate(id: string): SlateTemplate | undefined {
        return this.templates.get(id);
    }

    /**
     * Get all templates
     */
    public getTemplates(): SlateTemplate[] {
        return Array.from(this.templates.values());
    }

    /**
     * Get templates by style
     */
    public getTemplatesByStyle(style: SlateStyle): SlateTemplate[] {
        return Array.from(this.templates.values()).filter(t => t.style === style);
    }
}

// Singleton instance
let generatorInstance: NewsSlateGenerator | null = null;

export function getNewsSlateGenerator(): NewsSlateGenerator {
    if (!generatorInstance) {
        generatorInstance = new NewsSlateGenerator();
    }
    return generatorInstance;
}

export const newsSlateGenerator = getNewsSlateGenerator();

