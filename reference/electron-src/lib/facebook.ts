import { loadSettings } from './storage';

export type FacebookAction =
    | { type: 'post'; message: string; link?: string; extra?: unknown }
    | { type: 'story'; message: string; imageUrl?: string; extra?: unknown }
    | { type: 'live_start'; title?: string; extra?: unknown }
    | { type: 'live_stop'; extra?: unknown };

async function postViaWebhook(url: string, payload: unknown): Promise<void> {
    const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
    });
    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Webhook failed: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

async function postViaGraphFeed(pageId: string, accessToken: string, message: string, link?: string): Promise<void> {
    // Basic Page feed post.
    // Requires a Page access token with permissions for posting.
    const body = new URLSearchParams();
    body.set('message', message);
    if (link) body.set('link', link);
    body.set('access_token', accessToken);

    const res = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(pageId)}/feed`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body,
    });
    if (!res.ok) {
        const text = await res.text().catch(() => '');
        throw new Error(`Graph feed post failed: ${res.status} ${res.statusText} ${text}`.trim());
    }
}

async function postStoryViaGraph(pageId: string, accessToken: string, message: string, imageUrl?: string): Promise<void> {
    // Facebook Stories API
    // Note: Stories require different permissions and may need to be posted via Page Stories API
    // For now, we'll use the feed API with story-specific formatting
    
    // If image URL provided, we'd need to upload it first, then create story
    // For simplicity, we'll post as a regular post but mark it as story-like
    // In production, you'd use: /{page-id}/photo with published=false, then /{page-id}/stories
    
    const body = new URLSearchParams();
    body.set('message', message);
    body.set('access_token', accessToken);
    
    // If image URL, try to post as photo first
    if (imageUrl) {
        // Upload photo (simplified - in production you'd handle file upload properly)
        const photoBody = new URLSearchParams();
        photoBody.set('url', imageUrl);
        photoBody.set('published', 'false'); // Don't publish to feed
        photoBody.set('access_token', accessToken);
        
        const photoRes = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(pageId)}/photos`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
            body: photoBody,
        });
        
        if (photoRes.ok) {
            const photoData = await photoRes.json();
            // Then create story from photo
            const storyBody = new URLSearchParams();
            storyBody.set('attached_media', JSON.stringify([{ media_fbid: photoData.id }]));
            storyBody.set('access_token', accessToken);
            
            const storyRes = await fetch(`https://graph.facebook.com/v20.0/${encodeURIComponent(pageId)}/stories`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: storyBody,
            });
            
            if (!storyRes.ok) {
                const text = await storyRes.text().catch(() => '');
                throw new Error(`Graph story post failed: ${storyRes.status} ${storyRes.statusText} ${text}`.trim());
            }
            return;
        }
    }
    
    // Fallback: post as regular feed post (stories API may not be available)
    // In production, you'd want proper story creation
    await postViaGraphFeed(pageId, accessToken, message);
}

export async function runFacebookAction(action: FacebookAction): Promise<void> {
    const settings = loadSettings();
    const webhookUrl = (settings.facebookWebhookUrl || '').trim();
    const webhookEnabled = settings.facebookWebhookEnabled !== false && !!webhookUrl;

    const graphEnabled = settings.facebookGraphEnabled === true;
    const pageId = (settings.facebookGraphPageId || '').trim();
    const token = (settings.facebookGraphAccessToken || '').trim();
    const graphReady = graphEnabled && !!pageId && !!token;

    const preferGraph = settings.facebookPreferGraph !== false;

    // Graph currently supports only "post" (feed) here; live start/stop goes via webhook.
    if (action.type === 'post') {
        if (preferGraph && graphReady) {
            await postViaGraphFeed(pageId, token, action.message, action.link);
            return;
        }
        if (webhookEnabled) {
            const { type: _t, ...rest } = action;
            await postViaWebhook(webhookUrl, { type: 'facebook_post', ...rest });
            return;
        }
        if (graphReady) {
            await postViaGraphFeed(pageId, token, action.message, action.link);
            return;
        }
        throw new Error('Facebook not configured (need webhook URL and/or Graph Page ID + Access Token)');
    }

    // Story posting
    if (action.type === 'story') {
        if (preferGraph && graphReady) {
            await postStoryViaGraph(pageId, token, action.message, action.imageUrl);
            return;
        }
        if (webhookEnabled) {
            const { type: _t, ...rest } = action;
            await postViaWebhook(webhookUrl, { type: 'facebook_story', ...rest });
            return;
        }
        if (graphReady) {
            await postStoryViaGraph(pageId, token, action.message, action.imageUrl);
            return;
        }
        throw new Error('Facebook not configured (need webhook URL and/or Graph Page ID + Access Token)');
    }

    // Live control: webhook first (recommended)
    if (webhookEnabled) {
        const t = action.type === 'live_start' ? 'facebook_live_start' : 'facebook_live_stop';
        const { type: _t, ...rest } = action;
        await postViaWebhook(webhookUrl, { type: t, ...rest });
        return;
    }

    throw new Error('Facebook Live control requires webhook (Graph Live API not configured here)');
}


