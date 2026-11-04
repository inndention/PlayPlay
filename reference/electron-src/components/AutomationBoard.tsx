import React, { useEffect, useMemo, useState } from 'react';
import { RefreshCw, Clock, AlertCircle, Sparkles, Radio, Volume2 } from 'lucide-react';
import styles from './AutomationBoard.module.css';
import { SocialCampaignPanel } from './SocialCampaignPanel';
import { OBSController } from './OBSController';
import { OBSSession } from '../lib/types';
import {
    FillerSlate,
    generateAiFillerSlate,
    getAiGuidedGeneratorTemplates,
    getFillerSlates,
    refreshSiteFiller,
    seedDefaultAiFillerSlates,
} from '../lib/siteContentFiller';
import {
    buildProductionCuePlan,
    getProductionTools,
    ProductionCuePlan,
    renderProductionVoice,
} from '../lib/aiProductionTools';
import { buildSttRecordWorkflow, getRecommendedVoiceStack } from '../lib/voiceIntelligence';

interface AutomationBoardProps {
    obsSessions: OBSSession[];
    onSessionsChange: (sessions: OBSSession[]) => void;
}

interface PostLogEntry {
    id: string;
    client: string;
    message: string;
    channels: string[];
    media: string[];
    scheduledAt?: string | null;
    ts: number;
    status?: string;
}

const POST_LOG_KEY = 'social_post_log';

function loadPostLog(): PostLogEntry[] {
    try {
        const raw = localStorage.getItem(POST_LOG_KEY);
        if (!raw) return [];
        const parsed = JSON.parse(raw);
        return Array.isArray(parsed) ? parsed : [];
    } catch {
        return [];
    }
}

function formatRemaining(ms: number): string {
    if (ms <= 0) return 'expired';
    const minutes = Math.round(ms / 60000);
    if (minutes < 120) return `${minutes}m`;
    const hours = Math.round(minutes / 60);
    if (hours < 48) return `${hours}h`;
    const days = Math.round(hours / 24);
    return `${days}d`;
}

export function AutomationBoard({ obsSessions, onSessionsChange }: AutomationBoardProps) {
    const [postLog, setPostLog] = useState<PostLogEntry[]>(() => loadPostLog());
    const [slates, setSlates] = useState<FillerSlate[]>(() => getFillerSlates());
    const [productionPlan, setProductionPlan] = useState<ProductionCuePlan | null>(null);
    const [buildingPlanId, setBuildingPlanId] = useState<string | null>(null);
    const [voiceBusy, setVoiceBusy] = useState(false);

    useEffect(() => {
        const handleStorage = () => setPostLog(loadPostLog());
        window.addEventListener('storage', handleStorage);
        return () => window.removeEventListener('storage', handleStorage);
    }, []);

    useEffect(() => {
        const handleSlates = (e: Event) => {
            if ((e as CustomEvent<FillerSlate[]>).detail) {
                setSlates((e as CustomEvent<FillerSlate[]>).detail);
            } else {
                setSlates(getFillerSlates());
            }
        };
        window.addEventListener('site-filler-updated', handleSlates as EventListener);
        return () => window.removeEventListener('site-filler-updated', handleSlates as EventListener);
    }, []);

    const upcomingPosts = useMemo(() => {
        return [...postLog].sort((a, b) => (b.ts || 0) - (a.ts || 0)).slice(0, 25);
    }, [postLog]);

    const productionTools = useMemo(() => getProductionTools(), []);
    const voiceStack = useMemo(() => getRecommendedVoiceStack(), []);

    const handleRefreshPosts = () => setPostLog(loadPostLog());

    const handleRefreshSlates = async () => {
        const next = await refreshSiteFiller();
        setSlates(next);
    };

    const handleSeedGenerators = () => {
        setSlates(seedDefaultAiFillerSlates());
    };

    const handleGenerateSlate = async (templateId: string) => {
        const slate = await generateAiFillerSlate(templateId);
        setSlates([slate, ...getFillerSlates()]);
    };

    const handleBuildProductionPlan = async (toolId: string) => {
        setBuildingPlanId(toolId);
        try {
            const plan = await buildProductionCuePlan(toolId, {
                stationName: 'Star News',
                siteName: 'Martinsville, Danville, and Rockingham viewers',
                programTitle: 'the current program',
                nextProgramTitle: 'the next scheduled show',
                storySlug: 'a developing local story',
                reporterName: 'our field reporter',
            });
            setProductionPlan(plan);
        } finally {
            setBuildingPlanId(null);
        }
    };

    const handleVoicePreview = async () => {
        if (!productionPlan) return;
        setVoiceBusy(true);
        try {
            const blob = await renderProductionVoice(productionPlan, { voice: 'af_heart', speed: 1, provider: 'auto' });
            if (!blob) return;
            const url = URL.createObjectURL(blob);
            const audio = new Audio(url);
            audio.onended = () => URL.revokeObjectURL(url);
            await audio.play();
        } finally {
            setVoiceBusy(false);
        }
    };

    return (
        <div className={styles.wrapper}>
            <div className={styles.column}>
                <div className={styles.sectionHeader}>Social &amp; Web Scheduling</div>
                <div className={styles.socialHost}>
                    <SocialCampaignPanel embedded />
                </div>
                <div className={styles.sectionHeaderRow}>
                    <span>Recent posts &amp; drafts</span>
                    <button className={styles.microButton} onClick={handleRefreshPosts}>Refresh</button>
                </div>
                <div className={styles.logList}>
                    {upcomingPosts.length === 0 ? (
                        <div className={styles.muted}>No posts logged yet.</div>
                    ) : (
                        upcomingPosts.map((p) => (
                            <div key={p.id} className={styles.logItem}>
                                <div className={styles.logTitle}>
                                    <span className={styles.logClient}>{p.client || 'General'}</span>
                                    <span className={styles.logChannels}>{p.channels?.join(', ') || '—'}</span>
                                    <span className={styles.logTime}>{new Date(p.ts).toLocaleString()}</span>
                                </div>
                                <div className={styles.logMessage}>{p.message}</div>
                                <div className={styles.logMeta}>
                                    {p.scheduledAt ? (
                                        <span>Scheduled {new Date(p.scheduledAt).toLocaleString()}</span>
                                    ) : (
                                        <span>Immediate</span>
                                    )}
                                    {p.media?.length ? <span>{p.media.length} media</span> : null}
                                    {p.status ? <span className={styles.status}>{p.status}</span> : null}
                                </div>
                            </div>
                        ))
                    )}
                </div>
            </div>

            <div className={styles.column}>
                <div className={styles.sectionHeaderRow}>
                    <span>OBS &amp; Automation Commands</span>
                    <button className={styles.microButton} onClick={handleRefreshSlates}>
                        <RefreshCw size={14} /> Refresh site content
                    </button>
                </div>
                <div className={styles.obsShell}>
                    <OBSController sessions={obsSessions} onSessionsChange={onSessionsChange} />
                </div>
                <div className={styles.productionPanel}>
                    <div className={styles.generatorHeader}>
                        <Radio size={14} />
                        <span>AI production assistant</span>
                    </div>
                    <div className={styles.voiceStack}>
                        {voiceStack.slice(0, 3).map(profile => (
                            <span key={profile.id} title={`${profile.endpointHint}: ${profile.useFor.join(', ')}`}>
                                {profile.label}
                            </span>
                        ))}
                    </div>
                    <div className={styles.productionToolGrid}>
                        {productionTools.map(tool => (
                            <button
                                key={tool.id}
                                className={styles.productionTool}
                                onClick={() => handleBuildProductionPlan(tool.id)}
                                title={`${tool.trigger}: ${tool.description}`}
                            >
                                <span className={styles.toolCategory}>{tool.category.replace('_', ' ')}</span>
                                <span>{buildingPlanId === tool.id ? 'Building...' : tool.title}</span>
                            </button>
                        ))}
                    </div>
                    {productionPlan ? (
                        <div className={styles.productionPlan}>
                            <div className={styles.productionPlanTop}>
                                <div>
                                    <span className={styles.toolCategory}>{productionPlan.category.replace('_', ' ')}</span>
                                    <strong>{productionPlan.title}</strong>
                                </div>
                                <button className={styles.microButton} onClick={handleVoicePreview} disabled={voiceBusy}>
                                    <Volume2 size={14} /> {voiceBusy ? 'Rendering' : 'Voice'}
                                </button>
                            </div>
                            <div className={styles.scriptBox}>{productionPlan.script}</div>
                            <div className={styles.stepList}>
                                {productionPlan.steps.map((step, index) => (
                                    <span key={`${productionPlan.id}_${index}`}>{index + 1}. {step}</span>
                                ))}
                            </div>
                            {productionPlan.toolId === 'stt_context_index' ? (
                                <div className={styles.stepList}>
                                    {buildSttRecordWorkflow().map((step, index) => (
                                        <span key={`stt_${index}`}>STT {index + 1}. {step}</span>
                                    ))}
                                </div>
                            ) : null}
                        </div>
                    ) : null}
                </div>
                <div className={styles.sectionHeaderRow}>
                    <span>Live site content (auto slates)</span>
                    <span className={styles.muted}><AlertCircle size={14} /> expires auto</span>
                </div>
                <div className={styles.generatorPanel}>
                    <div className={styles.generatorHeader}>
                        <Sparkles size={14} />
                        <span>AI-guided fillers</span>
                        <button className={styles.microButton} onClick={handleSeedGenerators}>Seed defaults</button>
                    </div>
                    <div className={styles.generatorButtons}>
                        {getAiGuidedGeneratorTemplates().map(template => (
                            <button key={template.id} onClick={() => handleGenerateSlate(template.id)} title={template.prompt}>
                                {template.title}
                            </button>
                        ))}
                    </div>
                </div>
                <div className={styles.slateGrid}>
                    {slates.length === 0 ? (
                        <div className={styles.muted}>No site content cached yet. Refresh to pull from the site feed.</div>
                    ) : (
                        slates.map((s) => (
                            <div key={s.id} className={styles.slateCard}>
                                <div className={styles.slateTop}>
                                    <span className={styles.slateType}>{s.type}</span>
                                    <span className={styles.slateTimer}>
                                        <Clock size={12} /> {formatRemaining(s.expiresAt - Date.now())}
                                    </span>
                                </div>
                                <div className={styles.slateTitle}>{s.title}</div>
                                <div className={styles.slateContent}>{s.content}</div>
                            </div>
                        ))
                    )}
                </div>
            </div>
        </div>
    );
}
