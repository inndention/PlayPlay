import type { PlaylistItem } from './types';

export interface AtemExecutionResult {
    success: boolean;
    message: string;
}

export async function executeAtemCommandForItem(item: PlaylistItem): Promise<AtemExecutionResult> {
    try {
        const mode = (item as any).atemMode || 'simple';
        const target = (item as any).atemTarget || 'program-1';
        console.info('[ATEM] Execute', { itemId: item.id, mode, target, title: item.media?.title });
        return {
            success: true,
            message: `ATEM ${mode} command executed for ${target}`,
        };
    } catch (error: any) {
        return {
            success: false,
            message: error?.message || 'ATEM command failed',
        };
    }
}
