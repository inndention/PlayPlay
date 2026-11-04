// ============================================
// Slate Content Aggregator
// Fetches weather, stories, cinema data, and tower cam streams
// ============================================

import { ExtendedSettings } from './types';
import { loadSettings } from './storage';

export interface WeatherData {
    temperature: number;
    condition: string;
    humidity: number;
    windSpeed: number;
    forecast: Array<{
        day: string;
        high: number;
        low: number;
        condition: string;
    }>;
}

export interface StoryData {
    title: string;
    summary: string;
    publishedAt: string;
    url?: string;
}

export interface CinemaData {
    title: string;
    showtime: string;
    theater: string;
    rating?: string;
}

export interface TowerCamStream {
    id: string;
    name: string;
    url: string;
    type: 'rtsp' | 'rtmp' | 'hls';
}

// Fetch weather from Star News API or legacy service
export async function fetchWeather(): Promise<WeatherData | null> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        
        // Try Star News API first
        if (settings.starNewsApiBaseUrl) {
            const apiUrl = `${settings.starNewsApiBaseUrl}/api/v1/extra.php?resource=weather`;
            const response = await fetch(apiUrl);
            
            if (response.ok) {
                const data = await response.json();
                // Star News API response format
                return {
                    temperature: data.temperature || data.temp || 0,
                    condition: data.condition || data.weather || 'Unknown',
                    humidity: data.humidity || 0,
                    windSpeed: data.windSpeed || data.wind || 0,
                    forecast: data.forecast || [],
                };
            }
        }
        
        // Fallback to legacy service
        const url = settings.metroWeatherServiceUrl;
        if (!url) {
            console.warn('[SlateAggregator] No weather service URL configured');
            return null;
        }

        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Weather service returned ${response.status}`);
        }

        const data = await response.json();
        
        // Parse response (adjust based on your service format)
        return {
            temperature: data.temperature || data.temp || 0,
            condition: data.condition || data.weather || 'Unknown',
            humidity: data.humidity || 0,
            windSpeed: data.windSpeed || data.wind || 0,
            forecast: data.forecast || [],
        };
    } catch (error) {
        console.error('[SlateAggregator] Error fetching weather:', error);
        return null;
    }
}

// Fetch recent stories from Star News API or legacy feed
export async function fetchRecentStories(limit = 5): Promise<StoryData[]> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        
        // Try Star News API first
        if (settings.starNewsApiBaseUrl) {
            const apiUrl = `${settings.starNewsApiBaseUrl}/api/v1/content?status=published&limit=${limit}&offset=0`;
            const response = await fetch(apiUrl);
            
            if (response.ok) {
                const data = await response.json();
                // Star News API response format: { success, data: [...] }
                const stories = data.data || data;
                return stories.map((item: any) => ({
                    title: item.title || '',
                    summary: item.content?.substring(0, 200) || item.summary || '',
                    publishedAt: item.published_at || item.created_at || new Date().toISOString(),
                    url: item.url || `${settings.starNewsApiBaseUrl}/content/${item.id}`,
                }));
            }
        }
        
        // Fallback to legacy feed
        const url = settings.storyFeedUrl;
        if (!url) {
            console.warn('[SlateAggregator] No story feed URL configured');
            return [];
        }

        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Story feed returned ${response.status}`);
        }

        const data = await response.json();
        
        // Parse response (adjust based on your feed format)
        const stories = Array.isArray(data) ? data : data.items || data.stories || [];
        return stories.slice(0, limit).map((item: any) => ({
            title: item.title || item.headline || '',
            summary: item.summary || item.description || '',
            publishedAt: item.publishedAt || item.date || new Date().toISOString(),
            url: item.url || item.link,
        }));
    } catch (error) {
        console.error('[SlateAggregator] Error fetching stories:', error);
        return [];
    }
}

// Fetch Hollywood cinema data from Star News API or legacy feed
export async function fetchCinemaData(): Promise<CinemaData[]> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        
        // Try Star News API first
        if (settings.starNewsApiBaseUrl) {
            const apiUrl = `${settings.starNewsApiBaseUrl}/api/v1/extra.php?resource=cinema`;
            const response = await fetch(apiUrl);
            
            if (response.ok) {
                const data = await response.json();
                // Star News API response format: { success, data: [...] }
                const cinemas = data.data || data;
                return cinemas.map((item: any) => ({
                    title: item.title || item.movie || '',
                    showtime: item.showtime || item.time || '',
                    theater: item.theater || item.cinema || '',
                    rating: item.rating,
                }));
            }
        }
        
        // Fallback to legacy feed
        const url = settings.cinemaFeedUrl;
        if (!url) {
            console.warn('[SlateAggregator] No cinema feed URL configured');
            return [];
        }

        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Cinema feed returned ${response.status}`);
        }

        const data = await response.json();
        
        // Parse response (adjust based on your feed format)
        const cinemas = Array.isArray(data) ? data : data.showtimes || [];
        return cinemas.map((item: any) => ({
            title: item.title || item.movie || '',
            showtime: item.showtime || item.time || '',
            theater: item.theater || item.cinema || '',
            rating: item.rating,
        }));
    } catch (error) {
        console.error('[SlateAggregator] Error fetching cinema data:', error);
        return [];
    }
}

// Get tower cam streams
export async function getTowerCamStreams(): Promise<TowerCamStream[]> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        const urls = settings.towerCamUrls || [];
        
        return urls.map((url, index) => {
            let type: 'rtsp' | 'rtmp' | 'hls' = 'rtsp';
            if (url.startsWith('rtmp://')) type = 'rtmp';
            else if (url.includes('.m3u8')) type = 'hls';
            
            return {
                id: `tower_${index}`,
                name: `Tower Cam ${index + 1}`,
                url,
                type,
            };
        });
    } catch (error) {
        console.error('[SlateAggregator] Error getting tower cam streams:', error);
        return [];
    }
}

// Fetch "This Day in History" from Star News API
export async function fetchHistory(): Promise<Array<{ title: string; date: string; description: string }>> {
    try {
        const settings = loadSettings() as ExtendedSettings;
        
        if (settings.starNewsApiBaseUrl) {
            const apiUrl = `${settings.starNewsApiBaseUrl}/api/v1/extra.php?resource=history`;
            const response = await fetch(apiUrl);
            
            if (response.ok) {
                const data = await response.json();
                const history = data.data || data;
                return Array.isArray(history) ? history : [];
            }
        }
        
        return [];
    } catch (error) {
        console.error('[SlateAggregator] Error fetching history:', error);
        return [];
    }
}

// Aggregate all content for slate generation
export async function aggregateSlateContent() {
    const [weather, stories, cinema, towerCams, history] = await Promise.all([
        fetchWeather(),
        fetchRecentStories(5),
        fetchCinemaData(),
        getTowerCamStreams(),
        fetchHistory(),
    ]);

    return {
        weather,
        stories,
        cinema,
        towerCams,
        history,
    };
}

