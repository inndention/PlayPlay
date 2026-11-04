import React, { useCallback, useDeferredValue, useEffect, useMemo, useState } from 'react';
import {
    AlertCircle,
    Download,
    ExternalLink,
    FolderPlus,
    HardDrive,
    Hash,
    MessageSquare,
    Network,
    Play,
    RefreshCw,
    Search,
    Square,
    Upload,
    Wifi,
    WifiOff,
} from 'lucide-react';
import { ContextMenu, useContextMenu } from './ContextMenu';
import { useToast } from './Toast';
import { buildStarfileMenuItems } from '../lib/contextMenuActions';
import { executeContextMenuAction } from '../lib/contextMenuRegistry';
import { safePrompt } from '../lib/promptPolyfill';
import { trackerApi } from '../lib/trackerApi';
import type {
    StarfileComment,
    StarfileFileDetails,
    StarfileFileItem,
    StarfileNetworkPresence,
    StarfileNodePresence,
} from '../lib/types';
import styles from './StarfileWorkspace.module.css';

interface StarfileState {
    running: boolean;
    status: 'stopped' | 'starting' | 'running' | 'error' | string;
    managed: boolean;
    port: number;
    url: string;
    lastError: string | null;
}

interface StarfileApi {
    getStatus: () => Promise<StarfileState>;
    start: () => Promise<StarfileState>;
    stop: () => Promise<StarfileState>;
    restart: () => Promise<StarfileState>;
    openExternal: () => Promise<StarfileState>;
    listFiles: (requestedPath?: string) => Promise<StarfileFileItem[]>;
    search: (query: string) => Promise<StarfileFileItem[]>;
    getFileDetails: (filePath: string) => Promise<StarfileFileDetails>;
    getComments: (filePath: string) => Promise<Array<Record<string, unknown>>>;
    addComment: (filePath: string, comment: string) => Promise<unknown>;
    upload: (directory: string, filePaths: string[]) => Promise<unknown>;
    download: (filePath: string) => Promise<unknown>;
    rename: (filePath: string, newName: string) => Promise<unknown>;
    move: (filePath: string, targetDirectory: string) => Promise<unknown>;
    delete: (filePath: string) => Promise<unknown>;
    createDirectory: (parentPath: string, name: string) => Promise<unknown>;
    getShareUrl: (filePath: string) => Promise<string>;
    openPath: (filePath: string) => Promise<unknown>;
    revealPath: (filePath: string) => Promise<unknown>;
}

interface HubNodeRecord extends StarfileNodePresence {
    hostname: string;
    aliases: string[];
    online: boolean;
    isLocal?: boolean;
}

interface SwarmAssetRecord {
    fingerprint: string | null;
    filename: string;
    size: number;
    nodeIds: string[];
}

interface NetworkSnapshot {
    nodes: HubNodeRecord[];
    assetsByFingerprint: Map<string, SwarmAssetRecord>;
    assetsByFallbackKey: Map<string, SwarmAssetRecord[]>;
    trackerEnabled: boolean;
    trackerConnected: boolean;
}

interface StarfileWorkspaceProps {
    onImportToLibrary?: (item: StarfileFileItem) => void | Promise<void>;
    onSendToPlaylist?: (item: StarfileFileItem) => void | Promise<void>;
    onShowAnalytics?: (payload?: Record<string, unknown>) => void;
}

function getElectronStarfileApi() {
    const candidate = window as Window & {
        electronAPI?: {
            getHostname?: () => Promise<string>;
            starfile?: StarfileApi;
            openFileDialog?: (options?: unknown) => Promise<{ canceled: boolean; filePaths: string[] }>;
        };
    };
    return candidate.electronAPI;
}

function normalizeNodeKey(value: unknown): string {
    return String(value ?? '').trim().toLowerCase();
}

function buildFileFallbackKey(name: string, size: number): string {
    return `${name.trim().toLowerCase()}::${Number(size || 0)}`;
}

function normalizeAvailableOn(value: unknown): string[] {
    if (Array.isArray(value)) {
        return value.map((entry) => normalizeNodeKey(entry)).filter(Boolean);
    }
    if (typeof value === 'string') {
        return value
            .split(/[,\n]/)
            .map((entry) => normalizeNodeKey(entry))
            .filter(Boolean);
    }
    return [];
}

function normalizeTimestamp(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value > 10_000_000_000 ? value : value * 1000;
    }
    if (typeof value === 'string' && value) {
        const parsed = Date.parse(value);
        return Number.isNaN(parsed) ? null : parsed;
    }
    return null;
}

function inferOnline(raw: Record<string, unknown>): boolean {
    const status = String(raw.status ?? raw.state ?? '').toLowerCase();
    if (status) {
        return ['online', 'ready', 'active', 'healthy', 'connected', 'up'].includes(status);
    }
    const lastSeen = normalizeTimestamp(raw.lastSeen ?? raw.last_seen ?? raw.timestamp);
    return lastSeen !== null ? Date.now() - lastSeen < 120_000 : false;
}

function buildNodeLabel(node: { hostname: string; siteId?: string; playerId?: string }): string {
    if (node.siteId && node.playerId) {
        return `${node.siteId.toUpperCase()} / ${node.playerId}`;
    }
    if (node.siteId) {
        return node.siteId.toUpperCase();
    }
    if (node.playerId) {
        return node.playerId;
    }
    return node.hostname;
}

function normalizeNodeRecord(raw: Record<string, unknown>, localHostname: string): HubNodeRecord | null {
    const hostname = String(raw.hostname ?? raw.host ?? raw.name ?? raw.id ?? '').trim();
    const canonicalId = normalizeNodeKey(raw.id ?? hostname ?? raw.siteId ?? raw.playerId);
    if (!canonicalId) {
        return null;
    }

    const siteId = typeof raw.siteId === 'string' && raw.siteId ? raw.siteId : undefined;
    const playerId = typeof raw.playerId === 'string' && raw.playerId ? raw.playerId : undefined;
    const aliases = Array.from(new Set([
        canonicalId,
        normalizeNodeKey(hostname),
        normalizeNodeKey(siteId),
        normalizeNodeKey(playerId),
        normalizeNodeKey(siteId && playerId ? `${siteId}:${playerId}` : ''),
    ].filter(Boolean)));

    const resolvedHostname = hostname || siteId || playerId || canonicalId;
    const isLocal = normalizeNodeKey(localHostname) !== '' && aliases.includes(normalizeNodeKey(localHostname));
    const role = siteId ? 'core' : 'edge';

    return {
        nodeId: canonicalId,
        hostname: resolvedHostname,
        label: buildNodeLabel({ hostname: resolvedHostname, siteId, playerId }),
        siteId,
        playerId,
        role,
        aliases,
        online: inferOnline(raw),
        isLocal,
    };
}

function normalizeSwarmAssets(rawSwarm: any): SwarmAssetRecord[] {
    const assetMap = new Map<string, { fingerprint: string | null; filename: string; size: number; nodeIds: Set<string> }>();

    const addAsset = (rawFile: Record<string, unknown>, ownerNodeId?: string) => {
        const fingerprint = normalizeNodeKey(rawFile.hash ?? rawFile.fingerprint) || null;
        const filename = String(rawFile.filename ?? rawFile.name ?? '').trim();
        const size = Number(rawFile.size_bytes ?? rawFile.size ?? 0);
        const discoveredNodes = normalizeAvailableOn(rawFile.available_on);
        if (ownerNodeId) {
            discoveredNodes.push(normalizeNodeKey(ownerNodeId));
        }
        const nodeIds = Array.from(new Set(discoveredNodes.filter(Boolean)));

        if (!fingerprint && !filename) {
            return;
        }

        const key = fingerprint ? `fp:${fingerprint}` : `fallback:${buildFileFallbackKey(filename, size)}`;
        const existing = assetMap.get(key) || {
            fingerprint,
            filename,
            size,
            nodeIds: new Set<string>(),
        };

        if (!existing.filename && filename) {
            existing.filename = filename;
        }
        if (!existing.size && size) {
            existing.size = size;
        }
        nodeIds.forEach((nodeId) => existing.nodeIds.add(nodeId));
        assetMap.set(key, existing);
    };

    if (Array.isArray(rawSwarm?.files)) {
        rawSwarm.files.forEach((file: Record<string, unknown>) => addAsset(file));
    }

    if (rawSwarm && typeof rawSwarm === 'object') {
        Object.entries(rawSwarm).forEach(([nodeId, files]) => {
            if (['files', 'nodes', 'node_count', 'file_count'].includes(nodeId)) {
                return;
            }
            if (Array.isArray(files)) {
                files.forEach((file) => {
                    if (file && typeof file === 'object') {
                        addAsset(file as Record<string, unknown>, nodeId);
                    }
                });
            }
        });
    }

    return Array.from(assetMap.values()).map((asset) => ({
        fingerprint: asset.fingerprint,
        filename: asset.filename,
        size: asset.size,
        nodeIds: Array.from(asset.nodeIds),
    }));
}

function normalizeComment(entry: Record<string, unknown>): StarfileComment {
    return {
        user: typeof entry.user === 'string' && entry.user ? entry.user : 'Starfile',
        comment: String(entry.comment ?? entry.text ?? ''),
        timestamp: typeof entry.timestamp === 'string'
            ? entry.timestamp
            : typeof entry.date === 'string'
                ? entry.date
                : undefined,
    };
}

function formatBytes(size: number): string {
    if (!size) return '0 B';
    if (size >= 1_000_000_000) return `${(size / 1_000_000_000).toFixed(1)} GB`;
    if (size >= 1_000_000) return `${(size / 1_000_000).toFixed(1)} MB`;
    if (size >= 1_000) return `${(size / 1_000).toFixed(1)} KB`;
    return `${size} B`;
}

function formatDateLabel(value?: string | null): string {
    if (!value) return '—';
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return value;
    return new Intl.DateTimeFormat(undefined, {
        month: 'short',
        day: 'numeric',
        hour: 'numeric',
        minute: '2-digit',
    }).format(date);
}

function shortenFingerprint(value?: string | null): string {
    if (!value) return 'Pending index';
    return value.length > 14 ? `${value.slice(0, 10)}…${value.slice(-4)}` : value;
}

function derivePresence(
    item: StarfileFileItem,
    snapshot: NetworkSnapshot,
    details?: StarfileFileDetails,
): StarfileNetworkPresence | null {
    if (item.isDir) {
        return null;
    }

    const targetCopies = 3;
    const fingerprint = item.fingerprint || details?.fingerprint || null;
    let matchedBy: StarfileNetworkPresence['matchedBy'] = 'none';
    let asset: SwarmAssetRecord | undefined;

    if (fingerprint) {
        asset = snapshot.assetsByFingerprint.get(fingerprint);
        if (asset) {
            matchedBy = 'fingerprint';
        }
    }

    if (!asset) {
        const candidates = snapshot.assetsByFallbackKey.get(buildFileFallbackKey(item.name, item.size)) || [];
        if (candidates.length === 1) {
            asset = candidates[0];
            matchedBy = 'filename_size';
        }
    }

    const presentNodes = asset?.nodeIds
        .map((nodeId) => snapshot.nodes.find((node) => node.nodeId === nodeId))
        .filter(Boolean) as HubNodeRecord[];
    const missingNodes = snapshot.nodes.filter((node) => !presentNodes.some((present) => present.nodeId === node.nodeId));
    const onlineCount = presentNodes.filter((node) => node.online).length;
    const copyCount = presentNodes.length;
    const healthy = copyCount >= targetCopies;

    let availabilityLabel = 'Local only';
    if (copyCount > 0) {
        availabilityLabel = healthy ? `${copyCount}/${targetCopies} ready` : `${copyCount}/${targetCopies} copies`;
    } else if (!snapshot.trackerEnabled) {
        availabilityLabel = 'Tracker off';
    } else if (!snapshot.trackerConnected) {
        availabilityLabel = 'Tracker offline';
    }

    return {
        matchedBy,
        copyCount,
        onlineCount,
        targetCopies,
        healthy,
        availabilityLabel,
        fingerprint,
        nodes: presentNodes.map((node) => ({
            nodeId: node.nodeId,
            label: node.label,
            hostname: node.hostname,
            siteId: node.siteId,
            playerId: node.playerId,
            online: node.online,
            role: node.role,
        })),
        missingNodes: missingNodes.slice(0, Math.max(0, targetCopies - copyCount)).map((node) => ({
            nodeId: node.nodeId,
            label: node.label,
            hostname: node.hostname,
            siteId: node.siteId,
            playerId: node.playerId,
            online: node.online,
            role: node.role,
        })),
    };
}

export function StarfileWorkspace({
    onImportToLibrary,
    onSendToPlaylist,
    onShowAnalytics,
}: StarfileWorkspaceProps) {
    const electron = getElectronStarfileApi();
    const starfileApi = electron?.starfile;
    const { showToast } = useToast();
    const { menuState, showMenu, hideMenu } = useContextMenu();
    const [state, setState] = useState<StarfileState | null>(null);
    const [busyAction, setBusyAction] = useState<string | null>(null);
    const [listBusy, setListBusy] = useState(false);
    const [networkBusy, setNetworkBusy] = useState(false);
    const [advancedMode, setAdvancedMode] = useState(false);
    const [iframeNonce, setIframeNonce] = useState(0);
    const [currentPath, setCurrentPath] = useState('');
    const [searchQuery, setSearchQuery] = useState('');
    const deferredSearchQuery = useDeferredValue(searchQuery);
    const [items, setItems] = useState<StarfileFileItem[]>([]);
    const [selectedItemId, setSelectedItemId] = useState<string | null>(null);
    const [detailCache, setDetailCache] = useState<Record<string, StarfileFileDetails>>({});
    const [comments, setComments] = useState<StarfileComment[]>([]);
    const [shareUrl, setShareUrl] = useState('');
    const [localHostname, setLocalHostname] = useState('');
    const [registeredNodes, setRegisteredNodes] = useState<any[]>([]);
    const [swarmState, setSwarmState] = useState<any>(null);

    const refreshStatus = useCallback(async () => {
        if (!starfileApi) {
            return;
        }
        setState(await starfileApi.getStatus());
    }, [starfileApi]);

    const refreshList = useCallback(async () => {
        if (!starfileApi || !state?.running) {
            return;
        }

        setListBusy(true);
        try {
            const nextItems = deferredSearchQuery.trim()
                ? await starfileApi.search(deferredSearchQuery.trim())
                : await starfileApi.listFiles(currentPath);

            setItems(nextItems);
            setSelectedItemId((current) => {
                if (current && nextItems.some((item) => item.id === current)) {
                    return current;
                }
                return nextItems[0]?.id || null;
            });
        } finally {
            setListBusy(false);
        }
    }, [currentPath, deferredSearchQuery, starfileApi, state?.running]);

    const refreshNetwork = useCallback(async () => {
        if (!state?.running || !trackerApi.isEnabled()) {
            setRegisteredNodes([]);
            setSwarmState(null);
            return;
        }

        setNetworkBusy(true);
        try {
            const [instances, swarm] = await Promise.all([
                trackerApi.getRegisteredInstances(),
                trackerApi.getSwarmState(),
            ]);
            setRegisteredNodes(Array.isArray(instances) ? instances : []);
            setSwarmState(swarm || null);
        } finally {
            setNetworkBusy(false);
        }
    }, [state?.running]);

    const runAction = useCallback(async (
        label: string,
        action: () => Promise<unknown>,
        options?: { refreshFrame?: boolean; refreshList?: boolean },
    ) => {
        setBusyAction(label);
        try {
            const result = await action();
            if (result && typeof result === 'object' && 'status' in result) {
                setState(result as StarfileState);
            }
            if (options?.refreshFrame) {
                setIframeNonce((value) => value + 1);
            }
            if (options?.refreshList) {
                await refreshList();
                await refreshNetwork();
            }
            return result;
        } catch (error) {
            showToast(error instanceof Error ? error.message : String(error), 'error');
            throw error;
        } finally {
            setBusyAction(null);
        }
    }, [refreshList, refreshNetwork, showToast]);

    useEffect(() => {
        void refreshStatus();
    }, [refreshStatus]);

    useEffect(() => {
        if (!electron?.getHostname) {
            return;
        }
        electron.getHostname()
            .then((hostname) => setLocalHostname(hostname || ''))
            .catch(() => setLocalHostname(''));
    }, [electron]);

    useEffect(() => {
        if (!starfileApi) {
            return;
        }
        const intervalId = window.setInterval(() => {
            void refreshStatus();
        }, 5000);
        return () => window.clearInterval(intervalId);
    }, [refreshStatus, starfileApi]);

    useEffect(() => {
        if (!starfileApi || !state || state.running || state.status === 'starting') {
            return;
        }
        void runAction('Starting Starfile', () => starfileApi.start(), { refreshFrame: true });
    }, [runAction, starfileApi, state]);

    useEffect(() => {
        if (!state?.running) {
            setItems([]);
            setSelectedItemId(null);
            return;
        }
        const timeoutId = window.setTimeout(() => {
            void refreshList();
        }, 120);
        return () => window.clearTimeout(timeoutId);
    }, [refreshList, state?.running]);

    useEffect(() => {
        if (!state?.running) {
            setRegisteredNodes([]);
            setSwarmState(null);
            return;
        }
        void refreshNetwork();
        const intervalId = window.setInterval(() => {
            void refreshNetwork();
        }, 7000);
        return () => window.clearInterval(intervalId);
    }, [refreshNetwork, state?.running]);

    const networkSnapshot = useMemo<NetworkSnapshot>(() => {
        const nodeMap = new Map<string, HubNodeRecord>();
        const aliasMap = new Map<string, string>();

        const mergeNode = (raw: Record<string, unknown>) => {
            const normalized = normalizeNodeRecord(raw, localHostname);
            if (!normalized) {
                return;
            }

            const existing = nodeMap.get(normalized.nodeId);
            if (!existing) {
                nodeMap.set(normalized.nodeId, normalized);
            } else {
                existing.online = normalized.online || existing.online;
                existing.siteId = existing.siteId || normalized.siteId;
                existing.playerId = existing.playerId || normalized.playerId;
                existing.role = existing.role || normalized.role;
                existing.isLocal = existing.isLocal || normalized.isLocal;
                existing.aliases = Array.from(new Set([...existing.aliases, ...normalized.aliases]));
                existing.label = buildNodeLabel({
                    hostname: existing.hostname,
                    siteId: existing.siteId,
                    playerId: existing.playerId,
                });
            }

            const record = nodeMap.get(normalized.nodeId);
            record?.aliases.forEach((alias) => aliasMap.set(alias, normalized.nodeId));
        };

        registeredNodes.forEach((node) => {
            if (node && typeof node === 'object') {
                mergeNode(node as Record<string, unknown>);
            }
        });

        if (Array.isArray(swarmState?.swarm?.nodes)) {
            swarmState.swarm.nodes.forEach((node: Record<string, unknown>) => mergeNode(node));
        }

        const rawAssets = normalizeSwarmAssets(swarmState?.swarm ?? swarmState);
        rawAssets.forEach((asset) => {
            asset.nodeIds.forEach((nodeId) => {
                const canonicalId = aliasMap.get(nodeId) || nodeId;
                if (!nodeMap.has(canonicalId)) {
                    const syntheticNode: HubNodeRecord = {
                        nodeId: canonicalId,
                        hostname: canonicalId,
                        label: canonicalId,
                        aliases: [canonicalId],
                        online: false,
                        role: 'edge',
                    };
                    nodeMap.set(canonicalId, syntheticNode);
                    aliasMap.set(canonicalId, canonicalId);
                }
            });
        });

        const assetsByFingerprint = new Map<string, SwarmAssetRecord>();
        const assetsByFallbackKey = new Map<string, SwarmAssetRecord[]>();

        rawAssets.forEach((asset) => {
            const normalizedNodes = asset.nodeIds
                .map((nodeId) => aliasMap.get(nodeId) || nodeId)
                .filter(Boolean);

            const nextAsset: SwarmAssetRecord = {
                ...asset,
                nodeIds: Array.from(new Set(normalizedNodes)),
            };

            if (nextAsset.fingerprint) {
                assetsByFingerprint.set(nextAsset.fingerprint, nextAsset);
            }

            const fallbackKey = buildFileFallbackKey(nextAsset.filename, nextAsset.size);
            const fallbackList = assetsByFallbackKey.get(fallbackKey) || [];
            fallbackList.push(nextAsset);
            assetsByFallbackKey.set(fallbackKey, fallbackList);
        });

        const nodes = Array.from(nodeMap.values()).sort((left, right) => {
            if (left.online !== right.online) {
                return left.online ? -1 : 1;
            }
            return left.label.localeCompare(right.label);
        });

        return {
            nodes,
            assetsByFingerprint,
            assetsByFallbackKey,
            trackerEnabled: trackerApi.isEnabled(),
            trackerConnected: trackerApi.getConnectionState(),
        };
    }, [localHostname, registeredNodes, swarmState]);

    const displayedItems = useMemo(() => (
        items.map((item) => {
            const details = detailCache[item.id];
            const fingerprint = item.fingerprint || details?.fingerprint || null;
            const fingerprintAlgorithm = item.fingerprintAlgorithm || details?.fingerprint_algorithm || null;
            return {
                ...item,
                fingerprint,
                fingerprintAlgorithm,
                network: derivePresence({ ...item, fingerprint, fingerprintAlgorithm }, networkSnapshot, details),
            };
        })
    ), [detailCache, items, networkSnapshot]);

    const selectedItem = useMemo(
        () => displayedItems.find((item) => item.id === selectedItemId) || null,
        [displayedItems, selectedItemId],
    );
    const selectedDetails = selectedItem ? detailCache[selectedItem.id] : undefined;

    useEffect(() => {
        if (!starfileApi || !selectedItem) {
            setComments([]);
            setShareUrl('');
            return;
        }

        let isActive = true;
        const loadSelectedItemData = async () => {
            if (selectedItem.isDir) {
                if (isActive) {
                    setComments([]);
                    setShareUrl('');
                }
                return;
            }

            const [details, rawComments, nextShareUrl] = await Promise.all([
                starfileApi.getFileDetails(selectedItem.path).catch(() => null),
                starfileApi.getComments(selectedItem.path).catch(() => []),
                starfileApi.getShareUrl(selectedItem.path).catch(() => selectedItem.shareUrl || ''),
            ]);

            if (!isActive) {
                return;
            }

            if (details) {
                setDetailCache((current) => ({
                    ...current,
                    [selectedItem.id]: details,
                }));
            }
            setComments(Array.isArray(rawComments) ? rawComments.map((entry) => normalizeComment(entry)) : []);
            setShareUrl(nextShareUrl);
        };

        void loadSelectedItemData();
        return () => {
            isActive = false;
        };
    }, [selectedItem, starfileApi]);

    const breadcrumbs = useMemo(() => {
        const normalized = currentPath.replace(/\\/g, '/');
        const parts = normalized.split('/').filter(Boolean);
        const crumbs = [{ label: 'Root', path: '' }];
        parts.forEach((part, index) => {
            crumbs.push({
                label: part,
                path: parts.slice(0, index + 1).join('/'),
            });
        });
        return crumbs;
    }, [currentPath]);

    const fileSummary = useMemo(() => {
        const fileItems = displayedItems.filter((item) => !item.isDir);
        const covered = fileItems.filter((item) => (item.network?.copyCount || 0) > 0).length;
        const ready = fileItems.filter((item) => item.network?.healthy).length;
        const averageCopies = fileItems.length > 0
            ? fileItems.reduce((total, item) => total + (item.network?.copyCount || 0), 0) / fileItems.length
            : 0;

        return {
            fileCount: fileItems.length,
            covered,
            ready,
            averageCopies,
        };
    }, [displayedItems]);

    if (!starfileApi) {
        return (
            <div className={styles.shell}>
                <div className={styles.emptyState}>
                    <AlertCircle size={18} />
                    <span>Starfile integration is available only in the Electron desktop shell.</span>
                </div>
            </div>
        );
    }

    const status = state?.status ?? 'stopped';
    const isBusy = busyAction !== null || status === 'starting';
    const iframeUrl = state?.url ? `${state.url}?embedded=1&playplay=1` : '';
    const trackerLabel = !networkSnapshot.trackerEnabled
        ? 'Tracker off'
        : networkBusy
            ? 'Syncing'
            : networkSnapshot.trackerConnected
                ? 'Connected'
                : 'Configured / idle';

    const handleCreateFolder = async () => {
        const name = await safePrompt('New folder name', '');
        if (!name) return;
        await runAction('Creating folder', () => starfileApi.createDirectory(currentPath, name), {
            refreshList: true,
            refreshFrame: true,
        });
        showToast(`Created "${name}"`, 'success');
    };

    const handleUpload = async () => {
        const result = await electron?.openFileDialog?.({ properties: ['openFile', 'multiSelections'] });
        if (!result || result.canceled || result.filePaths.length === 0) {
            return;
        }
        await runAction('Uploading files', () => starfileApi.upload(currentPath, result.filePaths), {
            refreshList: true,
            refreshFrame: true,
        });
        showToast(`Uploaded ${result.filePaths.length} file${result.filePaths.length === 1 ? '' : 's'}`, 'success');
    };

    const openFolder = async (pathToOpen: string) => {
        setCurrentPath(pathToOpen);
        setSearchQuery('');
        setSelectedItemId(null);
    };

    const handleItemMenu = (event: React.MouseEvent, item: StarfileFileItem) => {
        setSelectedItemId(item.id);
        showMenu(event, buildStarfileMenuItems(item));
    };

    const handleMenuAction = async (actionId: any, payload?: Record<string, unknown>) => {
        const item = payload?.item as StarfileFileItem | undefined;
        await executeContextMenuAction(actionId, payload, {
            onOpenPreview: async () => {
                if (!item) return;
                if (item.isDir) {
                    await openFolder(item.path);
                    return;
                }
                if (item.previewUrl) {
                    window.open(item.previewUrl, '_blank', 'noopener');
                    return;
                }
                await starfileApi.openPath(item.path);
            },
            onImportToLibrary: async () => {
                if (item && onImportToLibrary) {
                    await onImportToLibrary(item);
                    showToast(`Imported "${item.name}"`, 'success');
                }
            },
            onSendToPlaylist: async () => {
                if (item && onSendToPlaylist) {
                    await onSendToPlaylist(item);
                    showToast(`Queued "${item.name}"`, 'success');
                }
            },
            onRevealFolder: async () => {
                if (!item) return;
                if (item.isDir) {
                    await openFolder(item.path);
                    return;
                }
                await starfileApi.revealPath(item.path);
            },
            onCopyShareLink: async () => {
                if (!item || item.isDir) return;
                const nextShareUrl = await starfileApi.getShareUrl(item.path);
                await navigator.clipboard.writeText(nextShareUrl);
                setShareUrl(nextShareUrl);
                showToast('Share link copied', 'success');
            },
            onDownload: async () => {
                if (!item || item.isDir) return;
                await runAction('Downloading file', () => starfileApi.download(item.path));
                showToast(`Downloaded "${item.name}"`, 'success');
            },
            onRename: async () => {
                if (!item) return;
                const nextName = await safePrompt('Rename item', item.name);
                if (!nextName || nextName === item.name) return;
                await runAction('Renaming item', () => starfileApi.rename(item.path, nextName), {
                    refreshList: true,
                    refreshFrame: true,
                });
                showToast(`Renamed to "${nextName}"`, 'success');
            },
            onMove: async () => {
                if (!item) return;
                const targetDirectory = await safePrompt('Move to directory', item.directory || currentPath || '');
                if (!targetDirectory) return;
                await runAction('Moving item', () => starfileApi.move(item.path, targetDirectory), {
                    refreshList: true,
                    refreshFrame: true,
                });
                showToast(`Moved "${item.name}"`, 'success');
            },
            onDelete: async () => {
                if (!item || !window.confirm(`Delete "${item.name}"?`)) return;
                await runAction('Deleting item', () => starfileApi.delete(item.path), {
                    refreshList: true,
                    refreshFrame: true,
                });
                showToast(`Deleted "${item.name}"`, 'info');
            },
            onViewAnalytics: async (nextPayload) => {
                onShowAnalytics?.(nextPayload);
            },
        });
    };

    const handleAddComment = async () => {
        if (!selectedItem || selectedItem.isDir) return;
        const comment = await safePrompt('Add a comment', '');
        if (!comment) return;
        await runAction('Adding comment', () => starfileApi.addComment(selectedItem.path, comment));
        const nextComments = await starfileApi.getComments(selectedItem.path).catch(() => []);
        setComments(Array.isArray(nextComments) ? nextComments.map((entry) => normalizeComment(entry)) : []);
        showToast('Comment added', 'success');
    };

    return (
        <div className={styles.shell}>
            <header className={styles.header}>
                <div>
                    <div className={styles.kicker}>Play Play File Hub</div>
                    <h2 className={styles.title}>Starfile2 shared workspace</h2>
                    <p className={styles.subtitle}>
                        Native file browsing now carries node-awareness, cached asset fingerprints, and direct Starfile actions so you can see what is local, what is redundant, and what is still missing from the cluster.
                    </p>
                </div>
                <div className={styles.actions}>
                    <button type="button" className={styles.secondaryButton} onClick={() => void refreshStatus()} disabled={isBusy}>
                        <RefreshCw size={16} />
                        Refresh
                    </button>
                    <button
                        type="button"
                        className={styles.secondaryButton}
                        onClick={() => void runAction('Restarting Starfile', () => starfileApi.restart(), {
                            refreshFrame: true,
                            refreshList: true,
                        })}
                        disabled={isBusy}
                    >
                        <RefreshCw size={16} />
                        Restart
                    </button>
                    <button type="button" className={styles.secondaryButton} onClick={() => setAdvancedMode((value) => !value)} disabled={!state?.running}>
                        <ExternalLink size={16} />
                        {advancedMode ? 'Native' : 'Advanced'}
                    </button>
                    <button
                        type="button"
                        className={styles.secondaryButton}
                        onClick={() => void runAction('Opening Starfile', () => starfileApi.openExternal())}
                        disabled={!state?.running || isBusy}
                    >
                        <ExternalLink size={16} />
                        Open External
                    </button>
                    <button
                        type="button"
                        className={styles.primaryButton}
                        onClick={() => void runAction(
                            state?.running ? 'Stopping Starfile' : 'Starting Starfile',
                            () => (state?.running ? starfileApi.stop() : starfileApi.start()),
                            { refreshFrame: !state?.running },
                        )}
                        disabled={isBusy}
                    >
                        {state?.running ? <Square size={16} /> : <Play size={16} />}
                        {state?.running ? 'Stop' : 'Start'}
                    </button>
                </div>
            </header>

            <section className={styles.statusGrid}>
                <div className={styles.statusCard}>
                    <span className={styles.label}>Status</span>
                    <strong className={styles.value}>{isBusy ? busyAction : status}</strong>
                </div>
                <div className={styles.statusCard}>
                    <span className={styles.label}>Host</span>
                    <strong className={styles.value}>{state?.url || 'http://127.0.0.1:5001/'}</strong>
                </div>
                <div className={styles.statusCard}>
                    <span className={styles.label}>Tracker</span>
                    <strong className={styles.value}>{trackerLabel}</strong>
                </div>
                <div className={styles.statusCard}>
                    <span className={styles.label}>Mode</span>
                    <strong className={styles.value}>{advancedMode ? 'Advanced / Legacy' : 'Native React Hub'}</strong>
                </div>
            </section>

            {state?.lastError && (
                <div className={styles.errorBanner}>
                    <AlertCircle size={16} />
                    <span>{state.lastError}</span>
                </div>
            )}

            {state?.running && advancedMode && iframeUrl ? (
                <div className={styles.frameShell}>
                    <div className={styles.frameHeader}>
                        <ExternalLink size={16} />
                        <span>Embedded Starfile2 fallback</span>
                    </div>
                    <iframe
                        key={`${iframeUrl}-${iframeNonce}`}
                        src={iframeUrl}
                        title="Starfile2 Workspace"
                        className={styles.frame}
                    />
                </div>
            ) : state?.running ? (
                <div className={styles.nativeShell}>
                    <section className={styles.networkStrip}>
                        <div className={styles.networkCard}>
                            <span className={styles.label}>Cluster</span>
                            <strong className={styles.value}>{networkSnapshot.nodes.length} nodes</strong>
                            <span className={styles.networkMeta}>
                                <Network size={14} />
                                {fileSummary.ready} ready assets
                            </span>
                        </div>
                        <div className={styles.networkCard}>
                            <span className={styles.label}>Coverage</span>
                            <strong className={styles.value}>{fileSummary.covered}/{fileSummary.fileCount || 0}</strong>
                            <span className={styles.networkMeta}>
                                <HardDrive size={14} />
                                files seen on network
                            </span>
                        </div>
                        <div className={styles.networkCard}>
                            <span className={styles.label}>Average Copies</span>
                            <strong className={styles.value}>{fileSummary.averageCopies.toFixed(1)}</strong>
                            <span className={styles.networkMeta}>
                                {networkSnapshot.trackerConnected ? <Wifi size={14} /> : <WifiOff size={14} />}
                                per visible file
                            </span>
                        </div>
                        <div className={styles.networkCard}>
                            <span className={styles.label}>Local Node</span>
                            <strong className={styles.value}>{localHostname || 'Unknown'}</strong>
                            <span className={styles.networkMeta}>
                                {networkBusy ? <RefreshCw size={14} className={styles.spinningIcon} /> : <Hash size={14} />}
                                asset fingerprint aware
                            </span>
                        </div>
                    </section>

                    <div className={styles.toolbar}>
                        <div className={styles.breadcrumbs}>
                            {breadcrumbs.map((crumb, index) => (
                                <button
                                    key={`${crumb.path}-${index}`}
                                    type="button"
                                    className={styles.crumb}
                                    onClick={() => {
                                        setCurrentPath(crumb.path);
                                        setSearchQuery('');
                                    }}
                                >
                                    {crumb.label}
                                </button>
                            ))}
                        </div>
                        <label className={styles.searchBox}>
                            <Search size={15} />
                            <input
                                value={searchQuery}
                                onChange={(event) => setSearchQuery(event.target.value)}
                                placeholder="Search files, folders, or indexed assets"
                            />
                        </label>
                        <div className={styles.toolbarActions}>
                            <button type="button" className={styles.secondaryButton} onClick={handleCreateFolder}>
                                <FolderPlus size={15} />
                                Folder
                            </button>
                            <button type="button" className={styles.secondaryButton} onClick={handleUpload}>
                                <Upload size={15} />
                                Upload
                            </button>
                            <button type="button" className={styles.secondaryButton} onClick={() => void Promise.all([refreshList(), refreshNetwork()])}>
                                <RefreshCw size={15} />
                                Reload
                            </button>
                        </div>
                    </div>

                    <div className={styles.contentGrid}>
                        <div className={styles.fileList}>
                            <div className={styles.fileListHeader}>
                                <span>Name</span>
                                <span>Presence</span>
                                <span>Type</span>
                                <span>Modified</span>
                            </div>
                            <div className={styles.fileListBody}>
                                {displayedItems.map((item) => (
                                    <button
                                        key={item.id}
                                        type="button"
                                        className={`${styles.fileRow} ${selectedItem?.id === item.id ? styles.fileRowActive : ''}`}
                                        onClick={() => setSelectedItemId(item.id)}
                                        onDoubleClick={() => {
                                            if (item.isDir) {
                                                void openFolder(item.path);
                                            } else if (item.previewUrl) {
                                                window.open(item.previewUrl, '_blank', 'noopener');
                                            }
                                        }}
                                        onContextMenu={(event) => handleItemMenu(event, item)}
                                    >
                                        <div className={styles.filePrimary}>
                                            <span className={styles.fileName}>{item.name}</span>
                                            <span className={styles.filePath}>{item.directory || 'Root'}</span>
                                        </div>
                                        <div className={styles.presenceCell}>
                                            {item.isDir ? (
                                                <span className={styles.metaMuted}>Folder</span>
                                            ) : (
                                                <>
                                                    <span className={`${styles.presencePill} ${item.network?.healthy ? styles.presenceHealthy : styles.presenceWarn}`}>
                                                        {item.network?.availabilityLabel || 'Local only'}
                                                    </span>
                                                    <div className={styles.nodeBadgeRow}>
                                                        {(item.network?.nodes || []).slice(0, 3).map((node) => (
                                                            <span
                                                                key={`${item.id}-${node.nodeId}`}
                                                                className={`${styles.nodeBadge} ${node.online ? styles.nodeOnline : styles.nodeOffline}`}
                                                                title={node.hostname || node.label}
                                                            >
                                                                {node.label}
                                                            </span>
                                                        ))}
                                                        {(item.network?.nodes?.length || 0) > 3 && (
                                                            <span className={styles.nodeBadge}>+{(item.network?.nodes?.length || 0) - 3}</span>
                                                        )}
                                                    </div>
                                                </>
                                            )}
                                        </div>
                                        <span className={styles.metaMuted}>{item.isDir ? 'Folder' : item.type}</span>
                                        <span className={styles.metaMuted}>{formatDateLabel(item.modifiedDate)}</span>
                                    </button>
                                ))}
                                {displayedItems.length === 0 && (
                                    <div className={styles.emptyState}>
                                        {listBusy ? <RefreshCw size={18} className={styles.spinningIcon} /> : <Search size={18} />}
                                        <span>{searchQuery ? 'No matching files.' : 'No files found in this location.'}</span>
                                    </div>
                                )}
                            </div>
                        </div>

                        <aside className={styles.detailPanel}>
                            <div className={styles.detailHeader}>
                                <div>
                                    <div className={styles.label}>Selected</div>
                                    <strong className={styles.value}>{selectedItem?.name || 'Nothing selected'}</strong>
                                </div>
                                {selectedItem && (
                                    <button
                                        type="button"
                                        className={styles.secondaryButton}
                                        onClick={() => {
                                            if (selectedItem.isDir) {
                                                void openFolder(selectedItem.path);
                                                return;
                                            }
                                            void handleMenuAction('download', { item: selectedItem });
                                        }}
                                    >
                                        {selectedItem.isDir ? <ExternalLink size={15} /> : <Download size={15} />}
                                        {selectedItem.isDir ? 'Open' : 'Download'}
                                    </button>
                                )}
                            </div>

                            {selectedItem && (
                                <>
                                    <div className={styles.detailMeta}>
                                        <div><span>Path</span><strong>{selectedItem.path}</strong></div>
                                        <div><span>Size</span><strong>{selectedItem.isDir ? '—' : formatBytes(selectedItem.size)}</strong></div>
                                        <div><span>Modified</span><strong>{formatDateLabel(selectedItem.modifiedDate)}</strong></div>
                                        <div><span>Share</span><strong>{shareUrl || selectedItem.shareUrl || '—'}</strong></div>
                                    </div>

                                    {!selectedItem.isDir && (
                                        <div className={styles.networkPanel}>
                                            <div className={styles.commentsHeader}>
                                                <div className={styles.sectionHeading}>
                                                    <HardDrive size={14} />
                                                    <span>Network Presence</span>
                                                </div>
                                                <span className={`${styles.presencePill} ${selectedItem.network?.healthy ? styles.presenceHealthy : styles.presenceWarn}`}>
                                                    {selectedItem.network?.availabilityLabel || 'Local only'}
                                                </span>
                                            </div>

                                            <div className={styles.detailMeta}>
                                                <div><span>Fingerprint</span><strong>{shortenFingerprint(selectedItem.fingerprint)}</strong></div>
                                                <div><span>Match</span><strong>{selectedItem.network?.matchedBy === 'fingerprint' ? 'Fingerprint' : selectedItem.network?.matchedBy === 'filename_size' ? 'Name + size' : 'None'}</strong></div>
                                                <div><span>Downloads</span><strong>{selectedDetails?.download_count ?? 0}</strong></div>
                                                <div><span>Online Copies</span><strong>{selectedItem.network?.onlineCount ?? 0}</strong></div>
                                            </div>

                                            <div className={styles.nodeSection}>
                                                <span className={styles.label}>Available On</span>
                                                <div className={styles.nodeMatrix}>
                                                    {(selectedItem.network?.nodes || []).length > 0 ? (
                                                        selectedItem.network?.nodes.map((node) => (
                                                            <span
                                                                key={`present-${node.nodeId}`}
                                                                className={`${styles.nodeBadge} ${node.online ? styles.nodeOnline : styles.nodeOffline}`}
                                                                title={node.hostname || node.label}
                                                            >
                                                                {node.label}
                                                            </span>
                                                        ))
                                                    ) : (
                                                        <span className={styles.metaMuted}>No remote copies advertised.</span>
                                                    )}
                                                </div>
                                            </div>

                                            {(selectedItem.network?.missingNodes || []).length > 0 && (
                                                <div className={styles.nodeSection}>
                                                    <span className={styles.label}>Missing From Core Copies</span>
                                                    <div className={styles.nodeMatrix}>
                                                        {selectedItem.network?.missingNodes?.map((node) => (
                                                            <span key={`missing-${node.nodeId}`} className={`${styles.nodeBadge} ${styles.nodeMissing}`}>
                                                                {node.label}
                                                            </span>
                                                        ))}
                                                    </div>
                                                </div>
                                            )}
                                        </div>
                                    )}
                                </>
                            )}

                            <div className={styles.commentsBlock}>
                                <div className={styles.commentsHeader}>
                                    <div className={styles.sectionHeading}>
                                        <MessageSquare size={14} />
                                        <span>Comments</span>
                                    </div>
                                    <button
                                        type="button"
                                        className={styles.secondaryButton}
                                        onClick={handleAddComment}
                                        disabled={!selectedItem || selectedItem.isDir}
                                    >
                                        Add
                                    </button>
                                </div>
                                <div className={styles.commentsList}>
                                    {comments.map((comment, index) => (
                                        <div key={`${comment.user}-${comment.timestamp}-${index}`} className={styles.commentCard}>
                                            <strong>{comment.user || 'Starfile'}</strong>
                                            <p>{comment.comment}</p>
                                            <span>{comment.timestamp || ''}</span>
                                        </div>
                                    ))}
                                    {comments.length === 0 && (
                                        <div className={styles.emptyState}>
                                            <MessageSquare size={18} />
                                            <span>No comments yet.</span>
                                        </div>
                                    )}
                                </div>
                            </div>
                        </aside>
                    </div>
                </div>
            ) : (
                <div className={styles.emptyState}>
                    <AlertCircle size={18} />
                    <span>Start Starfile to load the shared file hub.</span>
                </div>
            )}

            {menuState.visible && (
                <ContextMenu
                    x={menuState.x}
                    y={menuState.y}
                    items={menuState.items}
                    onClose={hideMenu}
                    onAction={handleMenuAction}
                />
            )}
        </div>
    );
}
