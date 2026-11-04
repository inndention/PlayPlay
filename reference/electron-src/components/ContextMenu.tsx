import React, { useEffect, useRef, useState, useCallback } from 'react';
import { createPortal } from 'react-dom';
import { ContextMenuItem } from '../lib/types';
import { ChevronRight } from 'lucide-react';
import { normalizeContextMenuItems } from '../lib/contextMenuRegistry';
import styles from './ContextMenu.module.css';

interface ContextMenuProps {
    x: number;
    y: number;
    items: ContextMenuItem[];
    onClose: () => void;
    onAction?: (actionId: string, payload?: Record<string, unknown>, item?: ContextMenuItem) => void;
}

export function ContextMenu({ x, y, items, onClose, onAction }: ContextMenuProps) {
    const menuRef = useRef<HTMLDivElement>(null);
    const [position, setPosition] = useState({ x, y });
    const [openSubmenu, setOpenSubmenu] = useState<string | null>(null);

    // Adjust position to stay in viewport
    useEffect(() => {
        if (menuRef.current) {
            const rect = menuRef.current.getBoundingClientRect();
            const viewportWidth = window.innerWidth;
            const viewportHeight = window.innerHeight;

            let adjustedX = x;
            let adjustedY = y;

            if (x + rect.width > viewportWidth) {
                adjustedX = viewportWidth - rect.width - 10;
            }
            if (y + rect.height > viewportHeight) {
                adjustedY = viewportHeight - rect.height - 10;
            }

            setPosition({ x: adjustedX, y: adjustedY });
        }
    }, [x, y]);

    // Close on outside click
    useEffect(() => {
        const handleClick = (e: MouseEvent) => {
            if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
                onClose();
            }
        };
        const handleEscape = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };

        document.addEventListener('mousedown', handleClick);
        document.addEventListener('keydown', handleEscape);
        return () => {
            document.removeEventListener('mousedown', handleClick);
            document.removeEventListener('keydown', handleEscape);
        };
    }, [onClose]);

    const handleItemClick = (item: ContextMenuItem) => {
        if (item.disabled || item.divider) return;
        if (item.submenu) {
            setOpenSubmenu(openSubmenu === item.id ? null : item.id);
            return;
        }
        if (item.action) {
            item.action();
        }
        if (item.actionId && onAction) {
            onAction(item.actionId, item.payload, item);
        }
        onClose();
    };

    return createPortal(
        <div
            ref={menuRef}
            className={styles.menu}
            style={{ left: position.x, top: position.y }}
        >
            {items.map((item, index) => {
                if (item.divider) {
                    return <div key={`divider-${index}`} className={styles.divider} />;
                }

                const hasSubmenu = item.submenu && item.submenu.length > 0;

                return (
                    <div key={item.id} className={styles.itemWrapper}>
                        <button
                            className={`${styles.item} ${item.disabled ? styles.disabled : ''} ${item.danger ? styles.danger : ''}`}
                            onClick={() => handleItemClick(item)}
                            onMouseEnter={() => hasSubmenu && setOpenSubmenu(item.id)}
                            disabled={item.disabled}
                        >
                            {item.icon && <span className={styles.icon}>{item.icon}</span>}
                            <span className={styles.label}>{item.label}</span>
                            {item.shortcut && <span className={styles.shortcut}>{item.shortcut}</span>}
                            {hasSubmenu && <ChevronRight size={12} className={styles.arrow} />}
                        </button>

                        {hasSubmenu && openSubmenu === item.id && (
                            <div className={styles.submenu}>
                                {item.submenu!.map((subItem, subIndex) => {
                                    if (subItem.divider) {
                                        return <div key={`sub-divider-${subIndex}`} className={styles.divider} />;
                                    }
                                    return (
                                        <button
                                            key={subItem.id}
                                            className={`${styles.item} ${subItem.disabled ? styles.disabled : ''} ${subItem.danger ? styles.danger : ''}`}
                                            onClick={() => handleItemClick(subItem)}
                                            disabled={subItem.disabled}
                                        >
                                            {subItem.icon && <span className={styles.icon}>{subItem.icon}</span>}
                                            <span className={styles.label}>{subItem.label}</span>
                                            {subItem.shortcut && <span className={styles.shortcut}>{subItem.shortcut}</span>}
                                        </button>
                                    );
                                })}
                            </div>
                        )}
                    </div>
                );
            })}
        </div>,
        document.body
    );
}

// Hook for managing context menu state
export function useContextMenu() {
    const [menuState, setMenuState] = useState<{
        visible: boolean;
        x: number;
        y: number;
        items: ContextMenuItem[];
    }>({ visible: false, x: 0, y: 0, items: [] });

    const showMenu = useCallback((e: React.MouseEvent, items: ContextMenuItem[]) => {
        e.preventDefault();
        e.stopPropagation();
        setMenuState({
            visible: true,
            x: e.clientX,
            y: e.clientY,
            items: normalizeContextMenuItems(items),
        });
    }, []);

    const showMenuAt = useCallback((x: number, y: number, items: ContextMenuItem[]) => {
        setMenuState({
            visible: true,
            x,
            y,
            items: normalizeContextMenuItems(items),
        });
    }, []);

    const hideMenu = useCallback(() => {
        setMenuState(prev => ({ ...prev, visible: false }));
    }, []);

    return { menuState, showMenu, showMenuAt, hideMenu };
}

// Common menu item builders
export const menuBuilders = {
    sendToDDR: (channels: ('A' | 'B' | 'C' | 'D')[], onSend: (channel: 'A' | 'B' | 'C' | 'D') => void): ContextMenuItem => ({
        id: 'send_to_ddr',
        label: 'Send to DDR',
        icon: '🎬',
        submenu: channels.map(ch => ({
            id: `ddr_${ch}`,
            label: `DDR ${ch}`,
            action: () => onSend(ch),
        })),
    }),

    addToPlaylist: (onAdd: () => void): ContextMenuItem => ({
        id: 'add_to_playlist',
        label: 'Add to Playlist',
        icon: '➕',
        action: onAdd,
    }),

    editItem: (onEdit: () => void): ContextMenuItem => ({
        id: 'edit',
        label: 'Edit',
        icon: '✏️',
        shortcut: 'E',
        action: onEdit,
    }),

    deleteItem: (onDelete: () => void): ContextMenuItem => ({
        id: 'delete',
        label: 'Delete',
        icon: '🗑️',
        danger: true,
        shortcut: 'Del',
        action: onDelete,
    }),

    copyItem: (onCopy: () => void): ContextMenuItem => ({
        id: 'copy',
        label: 'Copy',
        icon: '📋',
        shortcut: 'Ctrl+C',
        action: onCopy,
    }),

    pasteItem: (onPaste: () => void, disabled: boolean = false): ContextMenuItem => ({
        id: 'paste',
        label: 'Paste',
        icon: '📋',
        shortcut: 'Ctrl+V',
        disabled,
        action: onPaste,
    }),

    divider: (): ContextMenuItem => ({
        id: `divider_${Date.now()}`,
        label: '',
        divider: true,
    }),

    triggerCG: (cgElements: { id: string; name: string }[], onTrigger: (id: string) => void): ContextMenuItem => ({
        id: 'trigger_cg',
        label: 'Trigger CG',
        icon: '🎬',
        submenu: cgElements.map(cg => ({
            id: `cg_${cg.id}`,
            label: cg.name,
            action: () => onTrigger(cg.id),
        })),
    }),

    sendToOBS: (sessions: { id: string; name: string }[], onAction: (sessionId: string, action: string) => void): ContextMenuItem => ({
        id: 'obs_action',
        label: 'OBS Action',
        icon: '📡',
        submenu: sessions.flatMap(session => [
            { id: `obs_scene_${session.id}`, label: `${session.name}: Change Scene`, action: () => onAction(session.id, 'change_scene') },
            { id: `obs_rec_${session.id}`, label: `${session.name}: Toggle Record`, action: () => onAction(session.id, 'toggle_record') },
        ]),
    }),
};
