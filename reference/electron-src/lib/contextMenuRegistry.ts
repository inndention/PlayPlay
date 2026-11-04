import type { ContextMenuActionId, ContextMenuItem } from './types';

const GROUP_ORDER: Array<NonNullable<ContextMenuItem['group']>> = [
    'primary',
    'transport',
    'cg',
    'file',
    'analytics',
    'destructive',
];

function getGroupRank(item: ContextMenuItem): number {
    if (!item.group) {
        return Number.MAX_SAFE_INTEGER;
    }

    const index = GROUP_ORDER.indexOf(item.group);
    return index === -1 ? Number.MAX_SAFE_INTEGER : index;
}

export function normalizeContextMenuItems(items: ContextMenuItem[]): ContextMenuItem[] {
    if (!items.some((item) => item.group) || items.some((item) => item.divider)) {
        return items;
    }

    const ordered = items
        .map((item, index) => ({ item, index }))
        .sort((left, right) => {
            const rankDelta = getGroupRank(left.item) - getGroupRank(right.item);
            if (rankDelta !== 0) {
                return rankDelta;
            }
            return left.index - right.index;
        });

    const normalized: ContextMenuItem[] = [];
    let lastGroup: ContextMenuItem['group'] | null = null;

    ordered.forEach(({ item }, index) => {
        if (index > 0 && item.group && lastGroup && item.group !== lastGroup) {
            normalized.push({
                id: `divider-${lastGroup}-${item.group}-${index}`,
                label: '',
                divider: true,
            });
        }

        normalized.push(item);
        lastGroup = item.group ?? lastGroup;
    });

    return normalized;
}

export interface ContextMenuExecutor {
    onOpenPreview?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onAddToPlaylist?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onSendToPlaylist?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onSendToDDR?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onTriggerCG?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onEditSegments?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onAttachOBSCommand?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onImportToLibrary?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onRevealFolder?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onCopyShareLink?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onDownload?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onRename?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onMove?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onDelete?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onViewAnalytics?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onEditHour?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onCopyHour?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onPasteHour?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onClearHour?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onTakeToProgram?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onUpdateProgram?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onClearPreview?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onClearProgram?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onSelectTemplate?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onSetMediaStart?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onSetMediaExpire?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onOpenAdCadenceWizard?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onGenerateAsRunReport?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onCopyDay?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onPasteDay?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onClearDay?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onMediaPriorityIncrease?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onMediaPriorityDecrease?: (payload?: Record<string, unknown>) => void | Promise<void>;
    onMediaPriorityFreezeToggle?: (payload?: Record<string, unknown>) => void | Promise<void>;
}

export async function executeContextMenuAction(
    actionId: ContextMenuActionId,
    payload: Record<string, unknown> | undefined,
    executor: ContextMenuExecutor,
): Promise<void> {
    switch (actionId) {
        case 'open_preview':
            await executor.onOpenPreview?.(payload);
            break;
        case 'add_to_playlist':
            await executor.onAddToPlaylist?.(payload);
            break;
        case 'send_to_playlist':
            await executor.onSendToPlaylist?.(payload);
            break;
        case 'send_to_ddr':
            await executor.onSendToDDR?.(payload);
            break;
        case 'trigger_cg':
            await executor.onTriggerCG?.(payload);
            break;
        case 'edit_segments':
            await executor.onEditSegments?.(payload);
            break;
        case 'attach_obs_command':
            await executor.onAttachOBSCommand?.(payload);
            break;
        case 'import_to_library':
            await executor.onImportToLibrary?.(payload);
            break;
        case 'reveal_folder':
            await executor.onRevealFolder?.(payload);
            break;
        case 'copy_share_link':
            await executor.onCopyShareLink?.(payload);
            break;
        case 'download':
            await executor.onDownload?.(payload);
            break;
        case 'rename':
            await executor.onRename?.(payload);
            break;
        case 'move':
            await executor.onMove?.(payload);
            break;
        case 'delete':
            await executor.onDelete?.(payload);
            break;
        case 'view_analytics':
            await executor.onViewAnalytics?.(payload);
            break;
        case 'edit_hour':
            await executor.onEditHour?.(payload);
            break;
        case 'copy_hour':
            await executor.onCopyHour?.(payload);
            break;
        case 'paste_hour':
            await executor.onPasteHour?.(payload);
            break;
        case 'clear_hour':
            await executor.onClearHour?.(payload);
            break;
        case 'take_to_program':
            await executor.onTakeToProgram?.(payload);
            break;
        case 'update_program':
            await executor.onUpdateProgram?.(payload);
            break;
        case 'clear_preview':
            await executor.onClearPreview?.(payload);
            break;
        case 'clear_program':
            await executor.onClearProgram?.(payload);
            break;
        case 'select_template':
            await executor.onSelectTemplate?.(payload);
            break;
        case 'set_media_start':
            await executor.onSetMediaStart?.(payload);
            break;
        case 'set_media_expire':
            await executor.onSetMediaExpire?.(payload);
            break;
        case 'open_ad_cadence_wizard':
            await executor.onOpenAdCadenceWizard?.(payload);
            break;
        case 'generate_asrun_report':
            await executor.onGenerateAsRunReport?.(payload);
            break;
        case 'copy_day':
            await executor.onCopyDay?.(payload);
            break;
        case 'paste_day':
            await executor.onPasteDay?.(payload);
            break;
        case 'clear_day':
            await executor.onClearDay?.(payload);
            break;
        case 'media_priority_increase':
            await executor.onMediaPriorityIncrease?.(payload);
            break;
        case 'media_priority_decrease':
            await executor.onMediaPriorityDecrease?.(payload);
            break;
        case 'media_priority_freeze_toggle':
            await executor.onMediaPriorityFreezeToggle?.(payload);
            break;
        default:
            break;
    }
}
