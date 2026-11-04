import type { ContextMenuItem, HourSlot, OBSCommand, StarfileFileItem, UnifiedCGLayerState } from './types';

interface MenuTargetPayload {
    targetId: string;
    targetType: string;
    [key: string]: unknown;
}

interface MediaMenuOptions {
    ddrChannels?: Array<'A' | 'B' | 'C' | 'D'>;
    cgElements?: Array<{ id: string; name: string }>;
}

interface PlaylistMenuOptions {
    ddrChannels?: Array<'A' | 'B' | 'C' | 'D'>;
    obsCommands?: OBSCommand[];
}

function buildDDRSubmenu(payload: MenuTargetPayload, channels: Array<'A' | 'B' | 'C' | 'D'> = []): ContextMenuItem[] {
    return channels.map((channel) => ({
        id: `${payload.targetType}-${payload.targetId}-ddr-${channel}`,
        label: `DDR ${channel}`,
        actionId: 'send_to_ddr',
        payload: {
            ...payload,
            channel,
        },
    }));
}

function buildCGSubmenu(payload: MenuTargetPayload, elements: Array<{ id: string; name: string }> = []): ContextMenuItem[] {
    return elements.map((element) => ({
        id: `${payload.targetType}-${payload.targetId}-cg-${element.id}`,
        label: element.name,
        actionId: 'trigger_cg',
        payload: {
            ...payload,
            cgElementId: element.id,
        },
    }));
}

function buildOBSCommandSubmenu(payload: MenuTargetPayload, commands: OBSCommand[] = []): ContextMenuItem[] {
    return commands.map((command) => ({
        id: `${payload.targetType}-${payload.targetId}-obs-${command.id}`,
        label: command.label,
        actionId: 'attach_obs_command',
        payload: {
            ...payload,
            obsCommandId: command.id,
        },
    }));
}

export function buildMediaMenuItems(payload: MenuTargetPayload, options: MediaMenuOptions = {}): ContextMenuItem[] {
    const mediaRow = payload.media as { priorityFrozen?: boolean } | undefined;
    const frozen = Boolean(mediaRow?.priorityFrozen);

    const items: ContextMenuItem[] = [
        {
            id: `media-${payload.targetId}-add`,
            label: 'Add to Playlist',
            actionId: 'add_to_playlist',
            payload,
            group: 'primary',
        },
        {
            id: `media-${payload.targetId}-preview`,
            label: 'Open Preview',
            actionId: 'open_preview',
            payload,
            group: 'primary',
        },
        {
            id: `media-${payload.targetId}-prio-up`,
            label: 'Increase priority',
            actionId: 'media_priority_increase',
            payload,
            group: 'primary',
        },
        {
            id: `media-${payload.targetId}-prio-down`,
            label: 'Reduce priority',
            actionId: 'media_priority_decrease',
            payload,
            group: 'primary',
        },
        {
            id: `media-${payload.targetId}-prio-freeze`,
            label: frozen ? 'Unfreeze priority' : 'Freeze priority',
            actionId: 'media_priority_freeze_toggle',
            payload,
            group: 'primary',
        },
    ];

    if ((options.ddrChannels || []).length > 0) {
        items.push({
            id: `media-${payload.targetId}-ddr`,
            label: 'Send to DDR',
            actionId: 'send_to_ddr',
            payload,
            submenu: buildDDRSubmenu(payload, options.ddrChannels),
            group: 'transport',
        });
    }

    if ((options.cgElements || []).length > 0) {
        items.push({
            id: `media-${payload.targetId}-cg`,
            label: 'Trigger CG',
            actionId: 'trigger_cg',
            payload,
            submenu: buildCGSubmenu(payload, options.cgElements),
            group: 'cg',
        });
    }

    items.push(
        {
            id: `media-${payload.targetId}-set-start`,
            label: 'Set Valid Start...',
            actionId: 'set_media_start',
            payload,
            group: 'file',
        },
        {
            id: `media-${payload.targetId}-set-expire`,
            label: 'Set Expire Time...',
            actionId: 'set_media_expire',
            payload,
            group: 'file',
        },
        {
            id: `media-${payload.targetId}-ad-cadence`,
            label: 'Ad Cadence Wizard...',
            actionId: 'open_ad_cadence_wizard',
            payload,
            group: 'transport',
            disabled: Boolean(payload?.media) && (payload.media as any).type !== 'ad',
        },
        {
            id: `media-${payload.targetId}-segments`,
            label: 'Segment Editor',
            actionId: 'edit_segments',
            payload,
            group: 'file',
        },
        {
            id: `media-${payload.targetId}-analytics`,
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
        {
            id: `media-${payload.targetId}-asrun`,
            label: 'Generate As-Run Report',
            actionId: 'generate_asrun_report',
            payload,
            group: 'analytics',
        },
    );

    return items;
}

export function buildPlaylistMenuItems(payload: MenuTargetPayload, options: PlaylistMenuOptions = {}): ContextMenuItem[] {
    const items: ContextMenuItem[] = [
        {
            id: `playlist-${payload.targetId}-preview`,
            label: 'Open Preview',
            actionId: 'open_preview',
            payload,
            group: 'primary',
        },
    ];

    if ((options.ddrChannels || []).length > 0) {
        items.push({
            id: `playlist-${payload.targetId}-ddr`,
            label: 'Send to DDR',
            actionId: 'send_to_ddr',
            payload,
            submenu: buildDDRSubmenu(payload, options.ddrChannels),
            group: 'transport',
        });
    }

    if ((options.obsCommands || []).length > 0) {
        items.push({
            id: `playlist-${payload.targetId}-obs`,
            label: 'Attach OBS Command',
            actionId: 'attach_obs_command',
            payload,
            submenu: buildOBSCommandSubmenu(payload, options.obsCommands),
            group: 'transport',
        });
    }

    items.push(
        {
            id: `playlist-${payload.targetId}-segments`,
            label: 'Segment Editor',
            actionId: 'edit_segments',
            payload,
            group: 'file',
        },
        {
            id: `playlist-${payload.targetId}-analytics`,
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
        {
            id: `playlist-${payload.targetId}-delete`,
            label: 'Delete',
            actionId: 'delete',
            payload,
            group: 'destructive',
            danger: true,
        },
    );

    return items;
}

export function buildSchedulerHourMenuItems(
    payload: MenuTargetPayload,
    canPaste: boolean,
    options?: { canUndoShake?: boolean; showHourSlot?: HourSlot | null },
): ContextMenuItem[] {
    const sh = options?.showHourSlot?.showHour;
    const showHourEnabled = Boolean(sh?.enabled);
    const hasWatchFolder = Boolean(sh?.watchFolderPath?.trim());

    return [
        {
            id: 'hour-edit',
            label: 'Edit Hour',
            actionId: 'edit_hour',
            payload,
            group: 'primary',
        },
        {
            id: 'hour-show-hour-toggle',
            label: showHourEnabled ? 'Clear show hour' : 'Set as show hour',
            actionId: 'toggle_show_hour',
            payload,
            group: 'primary',
        },
        {
            id: 'hour-show-hour-watch',
            label: 'Set show hour watch folder…',
            actionId: 'show_hour_watch_folder',
            payload,
            group: 'file',
            disabled: !showHourEnabled,
        },
        {
            id: 'hour-show-hour-clear-watch',
            label: 'Clear watch folder',
            actionId: 'show_hour_clear_watch_folder',
            payload,
            group: 'file',
            disabled: !showHourEnabled || !hasWatchFolder,
        },
        {
            id: 'hour-show-hour-refresh',
            label: 'Refresh watch folder now',
            actionId: 'show_hour_refresh_watch',
            payload,
            group: 'file',
            disabled: !showHourEnabled || !hasWatchFolder,
        },
        {
            id: 'hour-shake-ads',
            label: 'Shake ads (weighted)…',
            actionId: 'shake_hour_ads',
            payload,
            group: 'transport',
        },
        {
            id: 'hour-undo-shake',
            label: 'Undo last ad shake',
            actionId: 'undo_shake_hour_ads',
            payload,
            group: 'transport',
            disabled: !options?.canUndoShake,
        },
        {
            id: 'hour-copy',
            label: 'Copy Hour',
            actionId: 'copy_hour',
            payload,
            group: 'file',
        },
        {
            id: 'hour-paste',
            label: 'Paste Hour',
            actionId: 'paste_hour',
            payload,
            group: 'file',
            disabled: !canPaste,
        },
        {
            id: 'hour-analytics',
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
        {
            id: 'hour-clear',
            label: 'Clear Hour',
            actionId: 'clear_hour',
            payload,
            group: 'destructive',
            danger: true,
        },
    ];
}

export function buildSchedulerDayMenuItems(payload: MenuTargetPayload, canPaste: boolean): ContextMenuItem[] {
    return [
        {
            id: 'day-copy',
            label: 'Copy Day',
            actionId: 'copy_day',
            payload,
            group: 'file',
        },
        {
            id: 'day-paste',
            label: 'Paste Day...',
            actionId: 'paste_day',
            payload,
            group: 'file',
            disabled: !canPaste,
        },
        {
            id: 'day-clear',
            label: 'Clear Day',
            actionId: 'clear_day',
            payload,
            group: 'destructive',
            danger: true,
        },
    ];
}

export function buildStarfileMenuItems(item: StarfileFileItem): ContextMenuItem[] {
    const payload: MenuTargetPayload = {
        targetId: item.id,
        targetType: 'starfile',
        path: item.path,
        directory: item.directory,
        isDir: item.isDir,
        shareUrl: item.shareUrl || '',
        previewUrl: item.previewUrl || '',
        analyticsKey: item.analyticsKey,
        item,
    };

    const items: ContextMenuItem[] = [];

    if (!item.isDir) {
        items.push({
            id: 'starfile-open',
            label: 'Open Preview',
            actionId: 'open_preview',
            payload,
            group: 'primary',
        });
        items.push({
            id: 'starfile-send-playlist',
            label: 'Send to Playlist',
            actionId: 'send_to_playlist',
            payload,
            group: 'transport',
        });
    }

    items.push(
        {
            id: 'starfile-import',
            label: 'Import to Library',
            actionId: 'import_to_library',
            payload,
            group: 'file',
            disabled: item.isDir,
        },
        {
            id: 'starfile-reveal',
            label: item.isDir ? 'Open Folder' : 'Reveal Folder',
            actionId: 'reveal_folder',
            payload,
            group: 'file',
        },
        {
            id: 'starfile-copy-link',
            label: 'Copy Share Link',
            actionId: 'copy_share_link',
            payload,
            group: 'file',
            disabled: item.isDir,
        },
        {
            id: 'starfile-download',
            label: 'Download',
            actionId: 'download',
            payload,
            group: 'file',
            disabled: item.isDir,
        },
        {
            id: 'starfile-rename',
            label: 'Rename',
            actionId: 'rename',
            payload,
            group: 'file',
        },
        {
            id: 'starfile-move',
            label: 'Move',
            actionId: 'move',
            payload,
            group: 'file',
        },
        {
            id: 'starfile-analytics',
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
        {
            id: 'starfile-delete',
            label: 'Delete',
            actionId: 'delete',
            payload,
            group: 'destructive',
            danger: true,
        },
    );

    return items;
}

export function buildCGTemplateMenuItems(
    templateId: string,
    templateName: string,
    category?: string,
    extraPayload: Record<string, unknown> = {},
): ContextMenuItem[] {
    const payload: MenuTargetPayload = {
        targetId: templateId,
        targetType: 'cg_template',
        templateId,
        templateName,
        category,
        ...extraPayload,
    };

    return [
        {
            id: `cg-template-${templateId}-select`,
            label: 'Load In CG Console',
            actionId: 'select_template',
            payload,
            group: 'primary',
        },
        {
            id: `cg-template-${templateId}-analytics`,
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
    ];
}

export function buildCGLayerMenuItems(layer: UnifiedCGLayerState, channel: 'preview' | 'program'): ContextMenuItem[] {
    const payload: MenuTargetPayload = {
        targetId: layer.id,
        targetType: 'cg',
        layerId: layer.id,
        channel,
        layerNumber: layer.layer,
    };

    return [
        {
            id: `${channel}-${layer.id}-take`,
            label: channel === 'preview' ? 'Take to Program' : 'Update Program',
            actionId: channel === 'preview' ? 'take_to_program' : 'update_program',
            payload,
            group: 'primary',
        },
        {
            id: `${channel}-${layer.id}-analytics`,
            label: 'View Analytics',
            actionId: 'view_analytics',
            payload,
            group: 'analytics',
        },
        {
            id: `${channel}-${layer.id}-clear`,
            label: channel === 'preview' ? 'Clear Preview' : 'Clear Program',
            actionId: channel === 'preview' ? 'clear_preview' : 'clear_program',
            payload,
            group: 'destructive',
            danger: true,
        },
    ];
}
