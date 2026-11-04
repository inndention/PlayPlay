import React, { useMemo, useState } from 'react';
import {
    Box,
    Cpu,
    ExternalLink,
    FileCode2,
    FolderOpen,
    Package,
    Play,
    Server,
    ShieldCheck,
    Terminal,
    Wrench,
} from 'lucide-react';
import styles from './UtilityMenu.module.css';
import { AISchedulingAgentPanel } from './AISchedulingAgentPanel';
import { PlayplayAgentSettingsCard } from './PlayplayAgentSettingsCard';

type UtilityItem = {
    id: string;
    label: string;
    group: 'Launchers' | 'Scripts' | 'Dependencies' | 'Collaboration';
    description: string;
    command?: string;
    url?: string;
    icon: React.ReactNode;
};

const UTILITY_ITEMS: UtilityItem[] = [
    {
        id: 'cg-launcher',
        label: 'CG Launcher',
        group: 'Launchers',
        description: 'Open the separated CG template launcher and scheduler.',
        command: 'Electron: open CG launcher window',
        icon: <Box size={18} />,
    },
    {
        id: 'output-window',
        label: 'Output Window',
        group: 'Launchers',
        description: 'Open the OBS/browser-source output monitor.',
        command: 'Electron: open output window',
        icon: <Play size={18} />,
    },
    {
        id: 'build',
        label: 'Build App',
        group: 'Scripts',
        description: 'TypeScript and Vite production build.',
        command: 'npm run build',
        icon: <Terminal size={18} />,
    },
    {
        id: 'electron-build',
        label: 'Package Electron',
        group: 'Scripts',
        description: 'Clean, build, package, and verify Windows desktop release.',
        command: 'npm run electron:build',
        icon: <Package size={18} />,
    },
    {
        id: 'verify-build',
        label: 'Verify Build',
        group: 'Scripts',
        description: 'Run packaged-path and output checks after building.',
        command: 'npm run verify:build',
        icon: <ShieldCheck size={18} />,
    },
    {
        id: 'download-mpv',
        label: 'Download MPV',
        group: 'Dependencies',
        description: 'Fetch/update MPV runtime dependency.',
        command: 'npm run download:mpv',
        icon: <Cpu size={18} />,
    },
    {
        id: 'ai-check',
        label: 'AI CLI Check',
        group: 'Dependencies',
        description: 'Check Claude/Gemini/OpenAI/Cursor CLI availability.',
        command: 'npm run ai:check',
        icon: <Wrench size={18} />,
    },
    {
        id: 'copilot-server',
        label: 'Copilot Server',
        group: 'Collaboration',
        description: 'Start the local collaboration/copilot helper service.',
        command: 'npm run copilot:server',
        icon: <Server size={18} />,
    },
    {
        id: 'scripts-folder',
        label: 'Scripts Folder',
        group: 'Scripts',
        description: 'Open source scripts for review or handoff.',
        command: 'Explorer: scripts',
        icon: <FolderOpen size={18} />,
    },
    {
        id: 'package-json',
        label: 'Package Manifest',
        group: 'Dependencies',
        description: 'Dependency and npm script manifest.',
        command: 'Open package.json',
        icon: <FileCode2 size={18} />,
    },
];

export function UtilityMenu() {
    const [query, setQuery] = useState('');
    const electronAPI = (window as any).electronAPI;

    const filtered = useMemo(() => {
        const q = query.trim().toLowerCase();
        if (!q) return UTILITY_ITEMS;
        return UTILITY_ITEMS.filter(item => (
            item.label.toLowerCase().includes(q) ||
            item.group.toLowerCase().includes(q) ||
            item.description.toLowerCase().includes(q) ||
            item.command?.toLowerCase().includes(q)
        ));
    }, [query]);

    const grouped = useMemo(() => {
        return filtered.reduce<Record<string, UtilityItem[]>>((acc, item) => {
            acc[item.group] = acc[item.group] || [];
            acc[item.group].push(item);
            return acc;
        }, {});
    }, [filtered]);

    const runItem = async (item: UtilityItem) => {
        if (item.id === 'cg-launcher') {
            await electronAPI?.openCGLauncher?.();
            return;
        }
        if (item.id === 'output-window') {
            await electronAPI?.openOutputWindow?.();
            return;
        }
        if (item.id === 'scripts-folder') {
            await electronAPI?.openExternal?.('file:///C:/path/to/play_play/scripts');
            return;
        }
        if (item.id === 'package-json') {
            await electronAPI?.openExternal?.('file:///C:/path/to/play_play/package.json');
            return;
        }
        await navigator.clipboard?.writeText(item.command || '');
    };

    return (
        <div className={styles.container}>
            <header className={styles.header}>
                <div>
                    <h1>Utilities</h1>
                    <p>Deep tools, scripts, dependency checks, and collaboration launch points.</p>
                </div>
                <input
                    value={query}
                    onChange={(e) => setQuery(e.target.value)}
                    placeholder="Filter tools"
                    className={styles.search}
                />
            </header>

            <PlayplayAgentSettingsCard />

            <AISchedulingAgentPanel />

            <div className={styles.grid}>
                {Object.entries(grouped).map(([group, items]) => (
                    <section key={group} className={styles.group}>
                        <h2>{group}</h2>
                        <div className={styles.items}>
                            {items.map(item => (
                                <button key={item.id} className={styles.item} onClick={() => runItem(item)}>
                                    <span className={styles.icon}>{item.icon}</span>
                                    <span className={styles.itemText}>
                                        <strong>{item.label}</strong>
                                        <small>{item.description}</small>
                                        {item.command && <code>{item.command}</code>}
                                    </span>
                                    <ExternalLink size={14} className={styles.actionIcon} />
                                </button>
                            ))}
                        </div>
                    </section>
                ))}
            </div>
        </div>
    );
}
