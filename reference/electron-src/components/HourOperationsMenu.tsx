import React from 'react';
import { Copy, RefreshCw, Merge, Plus } from 'lucide-react';
import { ContextMenuItem } from '../lib/types';
import { ContextMenu } from './ContextMenu';

interface HourOperationsMenuProps {
    x: number;
    y: number;
    siteId: string;
    day: number;
    hour: number;
    onClose: () => void;
    onCopy?: (siteId: string, day: number, hour: number) => void;
    onRedeployAds?: (siteId: string, day: number, hour: number) => void;
    onFillWithIds?: (siteId: string, day: number, hour: number) => void;
    onFillWithBumpers?: (siteId: string, day: number, hour: number) => void;
    onRecalculateAds?: (siteId: string, day: number, hour: number) => void;
    onMerge?: (siteId: string, day: number, hour: number) => void;
}

export function HourOperationsMenu({
    x,
    y,
    siteId,
    day,
    hour,
    onClose,
    onCopy,
    onRedeployAds,
    onFillWithIds,
    onFillWithBumpers,
    onRecalculateAds,
    onMerge,
}: HourOperationsMenuProps) {
    const items: ContextMenuItem[] = [
        {
            id: 'copy',
            label: 'Copy Hour',
            icon: 'copy',
            action: () => {
                onCopy?.(siteId, day, hour);
                onClose();
            },
        },
        {
            id: 'redeploy_ads',
            label: 'Redeploy Ads',
            icon: 'refresh',
            action: () => {
                onRedeployAds?.(siteId, day, hour);
                onClose();
            },
        },
        {
            id: 'fill_ids',
            label: 'Fill with Station IDs',
            icon: 'plus',
            action: () => {
                onFillWithIds?.(siteId, day, hour);
                onClose();
            },
        },
        {
            id: 'fill_bumpers',
            label: 'Fill with Program Bumpers',
            icon: 'plus',
            action: () => {
                onFillWithBumpers?.(siteId, day, hour);
                onClose();
            },
        },
        {
            id: 'recalculate',
            label: 'Recalculate Ad Placement',
            icon: 'refresh',
            action: () => {
                onRecalculateAds?.(siteId, day, hour);
                onClose();
            },
        },
        {
            id: 'merge',
            label: 'Merge Hours',
            icon: 'merge',
            action: () => {
                onMerge?.(siteId, day, hour);
                onClose();
            },
        },
    ];

    return (
        <ContextMenu
            x={x}
            y={y}
            items={items}
            onClose={onClose}
        />
    );
}




