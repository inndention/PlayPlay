import type { AutomationAction, PlaylistItem } from './types';
import { runFacebookAction } from './facebook';

const ACTION_TIMEOUT_MS = 8000;

function safeParseJson(input?: string): unknown {
    if (!input) return undefined;
    try {
        return JSON.parse(input);
    } catch {
        return undefined;
    }
}

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`Action timed out after ${ms}ms: ${label}`)), ms);
        promise.then(
            v => { clearTimeout(timer); resolve(v); },
            e => { clearTimeout(timer); reject(e); }
        );
    });
}

export async function runPlaylistItemActions(item: PlaylistItem): Promise<void> {
    const actions = (item.actions || []).filter(a => a && a.enabled !== false);
    if (actions.length === 0) return;

    for (const a of actions) {
        try {
            await runAction(a);
        } catch (err) {
            // Log but never let a failing action block the timeline
            console.error('[AutomationActions] Action failed, continuing:', a.type, err);
        }
    }
}

async function runAction(a: AutomationAction): Promise<void> {
    switch (a.type) {
        case 'facebook_post':
            if (!a.message?.trim()) return;
            await withTimeout(runFacebookAction({
                type: 'post',
                message: a.message,
                link: a.link,
                extra: safeParseJson(a.webhookData),
            }), ACTION_TIMEOUT_MS, 'facebook_post');
            return;
        case 'facebook_live_start':
            await withTimeout(runFacebookAction({
                type: 'live_start',
                title: a.message,
                extra: safeParseJson(a.webhookData),
            }), ACTION_TIMEOUT_MS, 'facebook_live_start');
            return;
        case 'facebook_live_stop':
            await withTimeout(runFacebookAction({
                type: 'live_stop',
                extra: safeParseJson(a.webhookData),
            }), ACTION_TIMEOUT_MS, 'facebook_live_stop');
            return;
    }
}






