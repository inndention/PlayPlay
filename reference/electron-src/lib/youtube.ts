/**
 * YouTube Live API Integration
 * Handles YouTube Live streaming, video scheduling, and auto-publishing
 */

import { loadSettings } from './storage';

export interface YouTubeLiveSession {
    id: string;
    streamKey: string;
    streamUrl: string;
    rtmpUrl: string;
    status: 'idle' | 'live' | 'ended';
    startedAt?: Date;
    endedAt?: Date;
    title?: string;
    description?: string;
    broadcastId?: string;
}

export interface YouTubeVideo {
    id: string;
    title: string;
    description: string;
    thumbnailUrl: string;
    duration: number;
    publishedAt: string;
    channelId: string;
    channelTitle: string;
}

export interface YouTubeLiveStats {
    viewers: number;
    peakViewers: number;
    likes: number;
    dislikes: number;
    comments: number;
    chatMessages: number;
    superChats: number;
}

export interface YouTubeComment {
    id: string;
    text: string;
    author: {
        name: string;
        channelId: string;
        imageUrl?: string;
    };
    publishedAt: string;
    likeCount: number;
    isSuperChat?: boolean;
    superChatAmount?: number;
}

/**
 * Get OAuth2 access token from settings
 */
function getAccessToken(): string {
    const settings = loadSettings() as any; // ExtendedSettings extends AutomationSettings
    const token = (settings.youtubeAccessToken || '').trim();
    if (!token) {
        throw new Error('YouTube API not configured (need OAuth2 Access Token)');
    }
    return token;
}

/**
 * Create a YouTube Live broadcast
 */
export async function createYouTubeLiveBroadcast(
    title: string,
    description: string,
    scheduledStartTime?: Date
): Promise<YouTubeLiveSession> {
    const token = getAccessToken();

    // Create broadcast
    const broadcastBody = {
        snippet: {
            title,
            description,
            scheduledStartTime: scheduledStartTime?.toISOString(),
        },
        status: {
            privacyStatus: 'public',
            selfDeclaredMadeForKids: false,
        },
        contentDetails: {
            enableAutoStart: true,
            enableAutoStop: true,
        },
    };

    const broadcastRes = await fetch('https://www.googleapis.com/youtube/v3/liveBroadcasts?part=snippet,status,contentDetails', {
        method: 'POST',
        headers: {
            'Authorization': `Bearer ${token}`,
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(broadcastBody),
    });

    if (!broadcastRes.ok) {
        const text = await broadcastRes.text().catch(() => '');
        throw new Error(`Failed to create broadcast: ${broadcastRes.status} ${broadcastRes.statusText} ${text}`.trim());
    }

    const broadcastData = await broadcastRes.json();
    const broadcastId = broadcastData.id;

    // Create stream
    const streamBody = {
        snippet: {
            title: `${title} - Stream`,
        },
        cdn: {
            format: '1080p',
            ingestionType: 'rtmp',
        },
    };

    const streamRes = await fetch('https://www.googleapis.com/youtube/v3/liveStreams?part=snippet,cdn', {
        method: 'POST',
        headers: {
            'Authorization': `Bearer ${token}`,
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(streamBody),
    });

    if (!streamRes.ok) {
        const text = await streamRes.text().catch(() => '');
        throw new Error(`Failed to create stream: ${streamRes.status} ${streamRes.statusText} ${text}`.trim());
    }

    const streamData = await streamRes.json();
    const streamKey = streamData.cdn?.ingestionInfo?.streamName || '';
    const streamUrl = streamData.cdn?.ingestionInfo?.ingestionAddress || '';
    const rtmpUrl = streamUrl.replace('rtmps://', 'rtmp://');

    // Bind broadcast to stream
    const bindRes = await fetch(
        `https://www.googleapis.com/youtube/v3/liveBroadcasts/bind?id=${encodeURIComponent(broadcastId)}&part=snippet,contentDetails&streamId=${encodeURIComponent(streamData.id)}`,
        {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
            },
        }
    );

    if (!bindRes.ok) {
        const text = await bindRes.text().catch(() => '');
        throw new Error(`Failed to bind stream: ${bindRes.status} ${bindRes.statusText} ${text}`.trim());
    }

    return {
        id: streamData.id,
        streamKey,
        streamUrl,
        rtmpUrl,
        status: 'live',
        startedAt: new Date(),
        title,
        description,
        broadcastId,
    };
}

/**
 * Start a YouTube Live broadcast
 */
export async function startYouTubeLiveBroadcast(broadcastId: string): Promise<void> {
    const token = getAccessToken();

    const res = await fetch(
        `https://www.googleapis.com/youtube/v3/liveBroadcasts/transition?broadcastStatus=live&id=${encodeURIComponent(broadcastId)}&part=id,snippet,contentDetails,status`,
        {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
            },
        }
    );

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to start broadcast: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

/**
 * End a YouTube Live broadcast
 */
export async function endYouTubeLiveBroadcast(broadcastId: string): Promise<void> {
    const token = getAccessToken();

    const res = await fetch(
        `https://www.googleapis.com/youtube/v3/liveBroadcasts/transition?broadcastStatus=complete&id=${encodeURIComponent(broadcastId)}&part=id,snippet,contentDetails,status`,
        {
            method: 'POST',
            headers: {
                'Authorization': `Bearer ${token}`,
            },
        }
    );

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to end broadcast: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

/**
 * Get live chat messages
 */
export async function getYouTubeLiveChatMessages(liveChatId: string, limit: number = 50): Promise<YouTubeComment[]> {
    const token = getAccessToken();

    const res = await fetch(
        `https://www.googleapis.com/youtube/v3/liveChat/messages?liveChatId=${encodeURIComponent(liveChatId)}&part=snippet,authorDetails&maxResults=${limit}`,
        {
            headers: {
                'Authorization': `Bearer ${token}`,
            },
        }
    );

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to get chat messages: ${res.status} ${res.statusText} ${text}`.trim());
    }

    const data = await res.json();
    return (data.items || []).map((item: any) => ({
        id: item.id,
        text: item.snippet?.displayMessage || '',
        author: {
            name: item.authorDetails?.displayName || 'Unknown',
            channelId: item.authorDetails?.channelId || '',
            imageUrl: item.authorDetails?.profileImageUrl,
        },
        publishedAt: item.snippet?.publishedAt || '',
        likeCount: item.snippet?.likeCount || 0,
        isSuperChat: item.snippet?.type === 'superChatEvent',
        superChatAmount: item.snippet?.superChatDetails?.amountMicros
            ? parseInt(item.snippet.superChatDetails.amountMicros, 10) / 1000000
            : undefined,
    }));
}

/**
 * Get live broadcast statistics
 */
export async function getYouTubeLiveStats(broadcastId: string): Promise<YouTubeLiveStats> {
    const token = getAccessToken();

    // Get broadcast details to find live chat ID
    const broadcastRes = await fetch(
        `https://www.googleapis.com/youtube/v3/liveBroadcasts?part=liveStreamingDetails&id=${encodeURIComponent(broadcastId)}`,
        {
            headers: {
                'Authorization': `Bearer ${token}`,
            },
        }
    );

    if (!broadcastRes.ok) {
        throw new Error('Failed to get broadcast details');
    }

    const broadcastData = await broadcastRes.json();
    const liveChatId = broadcastData.items?.[0]?.liveStreamingDetails?.activeLiveChatId;

    // Get video statistics
    const videoId = broadcastData.items?.[0]?.liveStreamingDetails?.activeLiveChatId
        ? undefined
        : broadcastId; // Use broadcast ID if no video ID

    let viewers = 0;
    let likes = 0;
    let dislikes = 0;
    let comments = 0;
    let chatMessages = 0;
    let superChats = 0;

    if (videoId) {
        const statsRes = await fetch(
            `https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails,statistics&id=${encodeURIComponent(videoId)}`,
            {
                headers: {
                    'Authorization': `Bearer ${token}`,
                },
            }
        );

        if (statsRes.ok) {
            const statsData = await statsRes.json();
            const stats = statsData.items?.[0]?.statistics;
            if (stats) {
                viewers = parseInt(stats.viewCount || '0', 10);
                likes = parseInt(stats.likeCount || '0', 10);
                dislikes = parseInt(stats.dislikeCount || '0', 10);
                comments = parseInt(stats.commentCount || '0', 10);
            }
        }
    }

    if (liveChatId) {
        const chatRes = await fetch(
            `https://www.googleapis.com/youtube/v3/liveChat/messages?liveChatId=${encodeURIComponent(liveChatId)}&part=snippet&maxResults=1`,
            {
                headers: {
                    'Authorization': `Bearer ${token}`,
                },
            }
        );

        if (chatRes.ok) {
            const chatData = await chatRes.json();
            chatMessages = parseInt(chatData.pageInfo?.totalResults || '0', 10);
            
            // Count super chats
            const allMessages = await getYouTubeLiveChatMessages(liveChatId, 200);
            superChats = allMessages.filter(m => m.isSuperChat).length;
        }
    }

    return {
        viewers,
        peakViewers: viewers, // Peak would need historical tracking
        likes,
        dislikes,
        comments,
        chatMessages,
        superChats,
    };
}

/**
 * Schedule a YouTube video in playlist with prefetch
 */
export async function scheduleYouTubeVideoInPlaylist(
    videoId: string,
    playlistId: string,
    position?: number
): Promise<void> {
    const token = getAccessToken();

    const body = {
        snippet: {
            playlistId,
            resourceId: {
                kind: 'youtube#video',
                videoId,
            },
            position: position || 0,
        },
    };

    const res = await fetch('https://www.googleapis.com/youtube/v3/playlistItems?part=snippet', {
        method: 'POST',
        headers: {
            'Authorization': `Bearer ${token}`,
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(body),
    });

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to add video to playlist: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

/**
 * Auto-publish recording to YouTube
 */
export async function publishRecordingToYouTube(
    videoFile: File | string,
    title: string,
    description: string,
    tags: string[] = []
): Promise<YouTubeVideo> {
    const token = getAccessToken();

    // Upload video file
    const metadata = {
        snippet: {
            title,
            description,
            tags,
            categoryId: '10', // Music (default)
        },
        status: {
            privacyStatus: 'public',
            selfDeclaredMadeForKids: false,
        },
    };

    const formData = new FormData();
    formData.append('metadata', new Blob([JSON.stringify(metadata)], { type: 'application/json' }));
    
    if (typeof videoFile === 'string') {
        // If it's a file path, we'd need to read it first
        // For now, assume it's a URL and fetch it
        const fileRes = await fetch(videoFile);
        const blob = await fileRes.blob();
        formData.append('video', blob);
    } else {
        formData.append('video', videoFile);
    }

    const res = await fetch('https://www.googleapis.com/upload/youtube/v3/videos?uploadType=multipart&part=snippet,status', {
        method: 'POST',
        headers: {
            'Authorization': `Bearer ${token}`,
        },
        body: formData,
    });

    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Failed to upload video: ${res.status} ${res.statusText} ${text}`.trim());
    }

    const data = await res.json();
    
    return {
        id: data.id,
        title: data.snippet?.title || title,
        description: data.snippet?.description || description,
        thumbnailUrl: data.snippet?.thumbnails?.default?.url || '',
        duration: 0, // Would need to parse from contentDetails
        publishedAt: data.snippet?.publishedAt || new Date().toISOString(),
        channelId: data.snippet?.channelId || '',
        channelTitle: data.snippet?.channelTitle || '',
    };
}

