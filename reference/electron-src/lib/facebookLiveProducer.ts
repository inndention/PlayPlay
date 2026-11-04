/**
 * Facebook Live Producer API - Extended functionality
 * Handles comments, reactions, viewer stats, and live stream management
 */

import { loadSettings } from './storage';

export interface FacebookLiveSession {
    id: string;
    streamKey: string;
    streamUrl: string;
    rtmpUrl: string;
    status: 'idle' | 'live' | 'ended';
    startedAt?: Date;
    endedAt?: Date;
    title?: string;
    description?: string;
}

export interface FacebookComment {
    id: string;
    message: string;
    from: {
        name: string;
        id: string;
        picture?: {
            data: {
                url: string;
            };
        };
    };
    created_time: string;
    like_count: number;
    can_comment: boolean;
}

export interface FacebookReaction {
    type: 'like' | 'love' | 'wow' | 'haha' | 'sorry' | 'anger';
    count: number;
}

export interface FacebookLiveStats {
    viewers: number;
    peakViewers: number;
    reactions: FacebookReaction[];
    comments: number;
    shares: number;
    totalReactions: number;
}

export interface FacebookViewerJoin {
    id: string;
    name: string;
    avatar?: string;
    joinedAt: Date;
}

/**
 * Create a Facebook Live video
 */
export async function createFacebookLiveVideo(title?: string, description?: string): Promise<FacebookLiveSession> {
    const settings = loadSettings();
    const pageId = (settings.facebookGraphPageId || '').trim();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!pageId || !token) {
        throw new Error('Facebook Graph API not configured (need Page ID and Access Token)');
    }

    const body = new URLSearchParams();
    body.set('status', 'LIVE_NOW');
    if (title) body.set('title', title);
    if (description) body.set('description', description);
    body.set('access_token', token);

    const res = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(pageId)}/live_videos`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body,
    });

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to create live video: ${res.status} ${res.statusText} ${text}`.trim());
    }

    const data = await res.json();
    
    return {
        id: data.id,
        streamKey: data.stream_key || '',
        streamUrl: data.secure_stream_url || '',
        rtmpUrl: data.secure_stream_url?.replace('https://', 'rtmp://') || '',
        status: 'live',
        startedAt: new Date(),
        title: data.title || title,
        description: data.description || description,
    };
}

/**
 * Get live video stream key and URL
 */
export async function getFacebookLiveStreamKey(videoId: string): Promise<{ streamKey: string; streamUrl: string; rtmpUrl: string }> {
    const settings = loadSettings();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!token) {
        throw new Error('Facebook Graph API not configured');
    }

    const res = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}?fields=stream_key,secure_stream_url&access_token=${encodeURIComponent(token)}`);

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to get stream key: ${res.status} ${res.statusText} ${text}`.trim());
    }

    const data = await res.json();
    const streamUrl = data.secure_stream_url || '';
    
    return {
        streamKey: data.stream_key || '',
        streamUrl,
        rtmpUrl: streamUrl.replace('https://', 'rtmp://'),
    };
}

/**
 * End a Facebook Live video
 */
export async function endFacebookLiveVideo(videoId: string): Promise<void> {
    const settings = loadSettings();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!token) {
        throw new Error('Facebook Graph API not configured');
    }

    const body = new URLSearchParams();
    body.set('end_live_video', 'true');
    body.set('access_token', token);

    const res = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body,
    });

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to end live video: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

/**
 * Get comments for a live video
 */
export async function getFacebookLiveComments(videoId: string, limit: number = 50): Promise<FacebookComment[]> {
    const settings = loadSettings();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!token) {
        throw new Error('Facebook Graph API not configured');
    }

    const res = await fetch(
        `https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}/comments?limit=${limit}&order=chronological&fields=id,message,from{name,id,picture},created_time,like_count,can_comment&access_token=${encodeURIComponent(token)}`
    );

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to get comments: ${res.status} ${res.statusText} ${text}`.trim());
    }

    const data = await res.json();
    return (data.data || []).map((comment: any) => ({
        id: comment.id,
        message: comment.message || '',
        from: {
            name: comment.from?.name || 'Unknown',
            id: comment.from?.id || '',
            picture: comment.from?.picture,
        },
        created_time: comment.created_time,
        like_count: comment.like_count || 0,
        can_comment: comment.can_comment !== false,
    }));
}

/**
 * Get live video insights/stats
 */
export async function getFacebookLiveStats(videoId: string): Promise<FacebookLiveStats> {
    const settings = loadSettings();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!token) {
        throw new Error('Facebook Graph API not configured');
    }

    // Get video insights
    const insightsRes = await fetch(
        `https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}/insights?metric=video_view_time,video_views,reactions_total,comments,shares&access_token=${encodeURIComponent(token)}`
    );

    // Get reactions breakdown
    const reactionsRes = await fetch(
        `https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}/reactions?summary=true&access_token=${encodeURIComponent(token)}`
    );

    let viewers = 0;
    let peakViewers = 0;
    let totalReactions = 0;
    let comments = 0;
    let shares = 0;
    const reactions: FacebookReaction[] = [];

    if (insightsRes.ok) {
        const insightsData = await insightsRes.json();
        const insights = insightsData.data || [];
        
        const viewsMetric = insights.find((m: any) => m.name === 'video_views');
        if (viewsMetric) {
            viewers = parseInt(viewsMetric.values[0]?.value || '0', 10);
            peakViewers = viewers; // Peak would need historical tracking
        }

        const reactionsMetric = insights.find((m: any) => m.name === 'reactions_total');
        if (reactionsMetric) {
            totalReactions = parseInt(reactionsMetric.values[0]?.value || '0', 10);
        }

        const commentsMetric = insights.find((m: any) => m.name === 'comments');
        if (commentsMetric) {
            comments = parseInt(commentsMetric.values[0]?.value || '0', 10);
        }

        const sharesMetric = insights.find((m: any) => m.name === 'shares');
        if (sharesMetric) {
            shares = parseInt(sharesMetric.values[0]?.value || '0', 10);
        }
    }

    if (reactionsRes.ok) {
        const reactionsData = await reactionsRes.json();
        const summary = reactionsData.summary || {};
        
        reactions.push(
            { type: 'like', count: summary.like || 0 },
            { type: 'love', count: summary.love || 0 },
            { type: 'wow', count: summary.wow || 0 },
            { type: 'haha', count: summary.haha || 0 },
            { type: 'sorry', count: summary.sorry || 0 },
            { type: 'anger', count: summary.anger || 0 }
        );
    }

    return {
        viewers,
        peakViewers,
        reactions,
        comments,
        shares,
        totalReactions,
    };
}

/**
 * Subscribe to real-time updates (requires webhook setup)
 */
export async function subscribeToLiveUpdates(videoId: string, callbackUrl: string): Promise<void> {
    const settings = loadSettings();
    const token = (settings.facebookGraphAccessToken || '').trim();

    if (!token) {
        throw new Error('Facebook Graph API not configured');
    }

    const body = new URLSearchParams();
    body.set('object', 'video');
    body.set('callback_url', callbackUrl);
    body.set('fields', 'comments,reactions');
    body.set('verify_token', 'play_play_verify_token');
    body.set('access_token', token);

    const res = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(videoId)}/subscriptions`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body,
    });

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to subscribe: ${res.status} ${res.statusText} ${text}`.trim());
    }
}
