// ============================================
// News Feed Aggregator
// Real-time RSS feed parsing and aggregation
// ============================================

// @ts-ignore - rss-parser types may not be available
import Parser from 'rss-parser';

export interface NewsItem {
    id: string;
    title: string;
    content: string;
    summary?: string;
    source: string;
    sourceUrl: string;
    publishedAt: number;
    urgency: 'breaking' | 'high' | 'normal' | 'low';
    category?: string;
    keywords: string[];
    imageUrl?: string;
    link?: string;
    slateGenerated?: boolean;
}

export interface NewsFeedConfig {
    url: string;
    name: string;
    updateInterval: number; // seconds
    enabled: boolean;
    priority: number; // Higher = more important
    keywords?: string[]; // Filter keywords
    categories?: string[]; // Filter categories
}

export interface NewsFeedAggregatorConfig {
    feeds: NewsFeedConfig[];
    breakingNewsKeywords: string[];
    cacheDuration: number; // seconds
    maxItemsPerFeed: number;
}

class NewsFeedAggregator {
    private parser: Parser;
    private config: NewsFeedAggregatorConfig;
    private feeds: Map<string, NewsFeedConfig> = new Map();
    private items: Map<string, NewsItem> = new Map();
    private updateIntervals: Map<string, NodeJS.Timeout> = new Map();
    private onUpdateCallbacks: Set<(items: NewsItem[]) => void> = new Set();
    private onBreakingNewsCallbacks: Set<(item: NewsItem) => void> = new Set();

    constructor(config: NewsFeedAggregatorConfig) {
        this.config = config;
        this.parser = new Parser({
            customFields: {
                item: [
                    ['media:content', 'mediaContent', { keepArray: true }],
                    ['media:thumbnail', 'mediaThumbnail'],
                    ['enclosure', 'enclosure'],
                ],
            },
        });

        // Initialize feeds
        config.feeds.forEach(feed => {
            if (feed.enabled) {
                this.feeds.set(feed.url, feed);
            }
        });
    }

    /**
     * Start aggregating feeds
     */
    public start(): void {
        console.log('[NewsFeed] Starting aggregator with', this.feeds.size, 'feeds');
        
        // Initial fetch for all feeds
        this.feeds.forEach((feed, url) => {
            this.fetchFeed(url, feed);
        });

        // Set up periodic updates
        this.feeds.forEach((feed, url) => {
            const interval = setInterval(() => {
                this.fetchFeed(url, feed);
            }, feed.updateInterval * 1000);
            this.updateIntervals.set(url, interval);
        });
    }

    /**
     * Stop aggregating feeds
     */
    public stop(): void {
        this.updateIntervals.forEach(interval => clearInterval(interval));
        this.updateIntervals.clear();
    }

    /**
     * Fetch and parse a single feed
     */
    private async fetchFeed(url: string, config: NewsFeedConfig): Promise<void> {
        try {
            console.log(`[NewsFeed] Fetching ${config.name}...`);
            const feed = await this.parser.parseURL(url);
            
            if (!feed.items || feed.items.length === 0) {
                console.warn(`[NewsFeed] No items in feed ${config.name}`);
                return;
            }

            const newItems: NewsItem[] = [];
            const now = Date.now();

            for (const item of feed.items.slice(0, this.config.maxItemsPerFeed)) {
                // Skip if too old (older than cache duration)
                const pubDate = item.pubDate ? new Date(item.pubDate).getTime() : now;
                if (now - pubDate > this.config.cacheDuration * 1000) {
                    continue;
                }

                // Check if already exists (deduplication)
                const itemId = this.generateItemId(item, config);
                if (this.items.has(itemId)) {
                    continue;
                }

                // Extract content
                const content = item.contentSnippet || item.content || item.summary || '';
                const title = item.title || 'Untitled';
                const link = item.link || '';

                // Determine urgency
                const urgency = this.determineUrgency(title, content, config);

                // Extract image
                const imageUrl = this.extractImageUrl(item);

                // Extract keywords
                const keywords = this.extractKeywords(title, content);

                // Filter by keywords/categories if configured
                if (config.keywords && config.keywords.length > 0) {
                    const matches = config.keywords.some(kw => 
                        title.toLowerCase().includes(kw.toLowerCase()) ||
                        content.toLowerCase().includes(kw.toLowerCase())
                    );
                    if (!matches) continue;
                }

                const newsItem: NewsItem = {
                    id: itemId,
                    title,
                    content: this.cleanContent(content),
                    summary: item.contentSnippet || item.summary,
                    source: config.name,
                    sourceUrl: url,
                    publishedAt: pubDate,
                    urgency,
                    category: feed.title,
                    keywords,
                    imageUrl,
                    link,
                    slateGenerated: false,
                };

                this.items.set(itemId, newsItem);
                newItems.push(newsItem);

                // Check for breaking news
                if (urgency === 'breaking') {
                    this.notifyBreakingNews(newsItem);
                }
            }

            if (newItems.length > 0) {
                console.log(`[NewsFeed] Added ${newItems.length} new items from ${config.name}`);
                this.notifyUpdate();
            }
        } catch (error) {
            console.error(`[NewsFeed] Failed to fetch ${config.name}:`, error);
        }
    }

    /**
     * Generate unique ID for news item
     */
    private generateItemId(item: any, config: NewsFeedConfig): string {
        const title = item.title || '';
        const link = item.link || '';
        const pubDate = item.pubDate || '';
        const hash = `${config.url}_${title}_${link}_${pubDate}`;
        // Use a simple hash-based ID
        return `news_${Buffer.from(hash).toString('base64').replace(/[^a-zA-Z0-9]/g, '').substring(0, 16)}`;
    }

    /**
     * Determine urgency level
     */
    private determineUrgency(title: string, content: string, config: NewsFeedConfig): NewsItem['urgency'] {
        const text = `${title} ${content}`.toLowerCase();

        // Check for breaking news keywords
        const isBreaking = this.config.breakingNewsKeywords.some(kw => 
            text.includes(kw.toLowerCase())
        );

        if (isBreaking) return 'breaking';

        // Check for high urgency indicators
        const highUrgencyKeywords = ['urgent', 'alert', 'warning', 'crisis', 'emergency', 'developing'];
        if (highUrgencyKeywords.some(kw => text.includes(kw))) {
            return 'high';
        }

        // Check feed priority
        if (config.priority >= 8) {
            return 'high';
        }

        if (config.priority <= 3) {
            return 'low';
        }

        return 'normal';
    }

    /**
     * Extract image URL from feed item
     */
    private extractImageUrl(item: any): string | undefined {
        // Try media:thumbnail
        if (item.mediaThumbnail?.['$']?.url) {
            return item.mediaThumbnail['$'].url;
        }

        // Try media:content
        if (item.mediaContent?.[0]?.['$']?.url) {
            return item.mediaContent[0]['$'].url;
        }

        // Try enclosure
        if (item.enclosure?.url && item.enclosure.type?.startsWith('image/')) {
            return item.enclosure.url;
        }

        // Try extracting from HTML content
        if (item.content) {
            const imgMatch = item.content.match(/<img[^>]+src="([^"]+)"/i);
            if (imgMatch) {
                return imgMatch[1];
            }
        }

        return undefined;
    }

    /**
     * Extract keywords from text
     */
    private extractKeywords(title: string, content: string): string[] {
        const text = `${title} ${content}`.toLowerCase();
        const commonWords = new Set(['the', 'a', 'an', 'and', 'or', 'but', 'in', 'on', 'at', 'to', 'for', 'of', 'with', 'by']);
        
        const words = text
            .replace(/[^\w\s]/g, ' ')
            .split(/\s+/)
            .filter(w => w.length > 3 && !commonWords.has(w));

        // Return unique keywords, limited to 10
        return Array.from(new Set(words)).slice(0, 10);
    }

    /**
     * Clean HTML content
     */
    private cleanContent(content: string): string {
        return content
            .replace(/<[^>]+>/g, '') // Remove HTML tags
            .replace(/&nbsp;/g, ' ')
            .replace(/&amp;/g, '&')
            .replace(/&lt;/g, '<')
            .replace(/&gt;/g, '>')
            .replace(/&quot;/g, '"')
            .replace(/\s+/g, ' ')
            .trim();
    }

    /**
     * Get all news items
     */
    public getItems(): NewsItem[] {
        return Array.from(this.items.values())
            .sort((a, b) => {
                // Sort by urgency first, then by publish date
                const urgencyOrder = { breaking: 0, high: 1, normal: 2, low: 3 };
                const urgencyDiff = urgencyOrder[a.urgency] - urgencyOrder[b.urgency];
                if (urgencyDiff !== 0) return urgencyDiff;
                return b.publishedAt - a.publishedAt;
            });
    }

    /**
     * Get breaking news items
     */
    public getBreakingNews(): NewsItem[] {
        return this.getItems().filter(item => item.urgency === 'breaking');
    }

    /**
     * Get items by urgency
     */
    public getItemsByUrgency(urgency: NewsItem['urgency']): NewsItem[] {
        return this.getItems().filter(item => item.urgency === urgency);
    }

    /**
     * Mark item as slate generated
     */
    public markSlateGenerated(itemId: string): void {
        const item = this.items.get(itemId);
        if (item) {
            item.slateGenerated = true;
            this.items.set(itemId, item);
        }
    }

    /**
     * Subscribe to updates
     */
    public onUpdate(callback: (items: NewsItem[]) => void): () => void {
        this.onUpdateCallbacks.add(callback);
        return () => this.onUpdateCallbacks.delete(callback);
    }

    /**
     * Subscribe to breaking news
     */
    public onBreakingNews(callback: (item: NewsItem) => void): () => void {
        this.onBreakingNewsCallbacks.add(callback);
        return () => this.onBreakingNewsCallbacks.delete(callback);
    }

    /**
     * Notify subscribers of updates
     */
    private notifyUpdate(): void {
        const items = this.getItems();
        this.onUpdateCallbacks.forEach(callback => {
            try {
                callback(items);
            } catch (error) {
                console.error('[NewsFeed] Update callback error:', error);
            }
        });
    }

    /**
     * Notify subscribers of breaking news
     */
    private notifyBreakingNews(item: NewsItem): void {
        console.log(`[NewsFeed] BREAKING NEWS: ${item.title}`);
        this.onBreakingNewsCallbacks.forEach(callback => {
            try {
                callback(item);
            } catch (error) {
                console.error('[NewsFeed] Breaking news callback error:', error);
            }
        });
    }

    /**
     * Update configuration
     */
    public updateConfig(config: Partial<NewsFeedAggregatorConfig>): void {
        this.config = { ...this.config, ...config };
        
        // Restart if feeds changed
        if (config.feeds) {
            this.stop();
            this.feeds.clear();
            config.feeds.forEach(feed => {
                if (feed.enabled) {
                    this.feeds.set(feed.url, feed);
                }
            });
            this.start();
        }
    }

    /**
     * Add a new feed
     */
    public addFeed(feed: NewsFeedConfig): void {
        this.feeds.set(feed.url, feed);
        this.fetchFeed(feed.url, feed);
        
        const interval = setInterval(() => {
            this.fetchFeed(feed.url, feed);
        }, feed.updateInterval * 1000);
        this.updateIntervals.set(feed.url, interval);
    }

    /**
     * Remove a feed
     */
    public removeFeed(url: string): void {
        const interval = this.updateIntervals.get(url);
        if (interval) {
            clearInterval(interval);
            this.updateIntervals.delete(url);
        }
        this.feeds.delete(url);
    }
}

// Default configuration
const DEFAULT_CONFIG: NewsFeedAggregatorConfig = {
    feeds: [
        {
            url: 'https://feeds.bbci.co.uk/news/rss.xml',
            name: 'BBC News',
            updateInterval: 60,
            enabled: true,
            priority: 8,
        },
        {
            url: 'https://rss.cnn.com/rss/edition.rss',
            name: 'CNN',
            updateInterval: 60,
            enabled: true,
            priority: 8,
        },
        {
            url: 'https://feeds.reuters.com/reuters/topNews',
            name: 'Reuters Top News',
            updateInterval: 60,
            enabled: true,
            priority: 9,
        },
    ],
    breakingNewsKeywords: [
        'breaking', 'urgent', 'alert', 'developing', 'just in', 'live', 'crisis', 'emergency',
        'explosion', 'shooting', 'attack', 'disaster', 'earthquake', 'hurricane', 'tornado',
    ],
    cacheDuration: 3600, // 1 hour
    maxItemsPerFeed: 20,
};

// Singleton instance
let aggregatorInstance: NewsFeedAggregator | null = null;

export function getNewsFeedAggregator(config?: NewsFeedAggregatorConfig): NewsFeedAggregator {
    if (!aggregatorInstance) {
        aggregatorInstance = new NewsFeedAggregator(config || DEFAULT_CONFIG);
    }
    return aggregatorInstance;
}

export function startNewsFeedAggregator(config?: NewsFeedAggregatorConfig): NewsFeedAggregator {
    const agg = getNewsFeedAggregator(config);
    agg.start();
    return agg;
}

