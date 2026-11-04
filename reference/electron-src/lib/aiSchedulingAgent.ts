import { loadLibraryMedia, loadLocalMedia, loadWeeklySchedule, saveLibraryMedia, saveWeeklySchedule } from './storage';
import { dbFetchMedia, dbSaveMedia } from './mediaDbClient';
import { getStoredOpenRouterCredentials, requestOpenRouterJson } from './aiHelper';
import { getHermesSupervisorModelOrDefault, isHermesSupervisorEnabled } from './hermesSupervisorFlags';
import { runOpenRouterSupervisorJson } from './openrouterSupervisor';
import type { HourSlot, MediaItem, PlaylistItem, WeeklySchedule } from './types';

export type AgentOperationType =
    | 'set_slot_playlist'
    | 'append_to_slot'
    | 'rebalance_hour_ads'
    | 'assign_client'
    | 'assign_program_series'
    | 'pause_media'
    | 'set_media_priority';

export interface AgentOperation {
    type: AgentOperationType;
    reason: string;
    slotId?: string;
    mediaIds?: string[];
    minSeparationItems?: number;
    client?: string;
    programSeries?: string;
    paused?: boolean;
    priority?: number;
}

export interface AgentPlan {
    summary: string;
    assumptions: string[];
    risks: string[];
    operations: AgentOperation[];
}

export interface AgentExecuteOptions {
    dryRun?: boolean;
}

export interface AgentExecuteResult {
    success: boolean;
    dryRun: boolean;
    summary: string;
    plan: AgentPlan | null;
    appliedOperations: string[];
    changedSlots: string[];
    changedMedia: string[];
    warnings: string[];
    error?: string;
}

interface RuntimeContext {
    nowIso: string;
    currentSlotId: string;
    scheduleVersion: number;
    totalSlots: number;
    mediaCount: number;
    adCount: number;
    programCount: number;
    topSeries: Array<{ series: string; count: number }>;
    topClients: Array<{ client: string; count: number }>;
    slotSummaries: Array<{
        slotId: string;
        itemCount: number;
        totalSeconds: number;
        adCount: number;
        programCount: number;
        startsAt?: number;
        endsAt?: number;
    }>;
}

function generateId(): string {
    return `${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 10)}`;
}

function currentSlotIdNow(): string {
    const now = new Date();
    return `${now.getDay()}-${now.getHours()}`;
}

function getDuration(item: PlaylistItem): number {
    const timeIn = item.timeIn || 0;
    const timeOut = item.timeOut ?? item.media.duration;
    return Math.max(0, timeOut - timeIn);
}

function summarizeContext(schedule: WeeklySchedule, media: MediaItem[]): RuntimeContext {
    const slots = Object.values(schedule.slots || {});
    const ads = media.filter((m) => m.type === 'ad');
    const programs = media.filter((m) => m.type === 'program' || m.type === 'paid_program');

    const seriesMap = new Map<string, number>();
    const clientMap = new Map<string, number>();
    media.forEach((m) => {
        if (m.programSeries) {
            seriesMap.set(m.programSeries, (seriesMap.get(m.programSeries) || 0) + 1);
        }
        if (m.client) {
            clientMap.set(m.client, (clientMap.get(m.client) || 0) + 1);
        }
    });

    const topSeries = Array.from(seriesMap.entries())
        .sort((a, b) => b[1] - a[1])
        .slice(0, 10)
        .map(([series, count]) => ({ series, count }));

    const topClients = Array.from(clientMap.entries())
        .sort((a, b) => b[1] - a[1])
        .slice(0, 10)
        .map(([client, count]) => ({ client, count }));

    const slotSummaries = slots.slice(0, 120).map((slot) => {
        const itemCount = slot.items.length;
        const totalSeconds = slot.items.reduce((sum, item) => sum + getDuration(item), 0);
        const adCount = slot.items.filter((item) => item.media.type === 'ad').length;
        const programCount = slot.items.filter((item) => item.media.type === 'program' || item.media.type === 'paid_program').length;
        return {
            slotId: slot.id,
            itemCount,
            totalSeconds,
            adCount,
            programCount,
            startsAt: slot.items[0]?.startTime,
            endsAt: slot.items[slot.items.length - 1]?.endTime,
        };
    });

    return {
        nowIso: new Date().toISOString(),
        currentSlotId: currentSlotIdNow(),
        scheduleVersion: schedule.version || 1,
        totalSlots: slots.length,
        mediaCount: media.length,
        adCount: ads.length,
        programCount: programs.length,
        topSeries,
        topClients,
        slotSummaries,
    };
}

export function parseSchedulingAgentPlan(raw: unknown): AgentPlan | null {
    if (!raw || typeof raw !== 'object') return null;
    const obj = raw as any;
    if (!Array.isArray(obj.operations)) return null;

    const operations: AgentOperation[] = obj.operations
        .filter((o: any) => o && typeof o.type === 'string' && typeof o.reason === 'string')
        .map((o: any) => ({
            type: o.type,
            reason: o.reason,
            slotId: typeof o.slotId === 'string' ? o.slotId : undefined,
            mediaIds: Array.isArray(o.mediaIds) ? o.mediaIds.filter((id: unknown) => typeof id === 'string') : undefined,
            minSeparationItems: typeof o.minSeparationItems === 'number' ? o.minSeparationItems : undefined,
            client: typeof o.client === 'string' ? o.client : undefined,
            programSeries: typeof o.programSeries === 'string' ? o.programSeries : undefined,
            paused: typeof o.paused === 'boolean' ? o.paused : undefined,
            priority: typeof o.priority === 'number' ? o.priority : undefined,
        }));

    return {
        summary: typeof obj.summary === 'string' ? obj.summary : 'Generated schedule plan.',
        assumptions: Array.isArray(obj.assumptions) ? obj.assumptions.filter((x: unknown) => typeof x === 'string') : [],
        risks: Array.isArray(obj.risks) ? obj.risks.filter((x: unknown) => typeof x === 'string') : [],
        operations,
    };
}

function cloneSlot(slot: HourSlot): HourSlot {
    return {
        ...slot,
        items: slot.items.map((item) => ({ ...item, media: { ...item.media } })),
    };
}

function createPlaylistItem(media: MediaItem): PlaylistItem {
    return {
        id: `ai_agent_${generateId()}`,
        mediaId: media.id,
        media,
        isFixedTime: false,
        isResolved: true,
    };
}

export async function runSchedulingAgent(request: string, options: AgentExecuteOptions = {}): Promise<AgentExecuteResult> {
    const dryRun = options.dryRun !== false;
    const warnings: string[] = [];
    const appliedOperations: string[] = [];
    const changedSlots = new Set<string>();
    const changedMedia = new Set<string>();

    try {
        const [schedule, dbMedia] = await Promise.all([
            loadWeeklySchedule(true),
            dbFetchMedia().catch(() => [] as MediaItem[]),
        ]);
        const localLibrary = loadLibraryMedia([]);
        const localMedia = loadLocalMedia();

        const mediaById = new Map<string, MediaItem>();
        [...localLibrary, ...localMedia, ...dbMedia].forEach((m) => {
            if (m?.id && !mediaById.has(m.id)) mediaById.set(m.id, m);
        });
        const media = Array.from(mediaById.values());
        const ctx = summarizeContext(schedule, media);

        const supervisorSystem = [
            'You are PLAY PLAY\'s broadcast automation scheduling agent.',
            'Return compact JSON only.',
            'You must output an object with: summary, assumptions[], risks[], operations[].',
            'Allowed operation types: set_slot_playlist, append_to_slot, rebalance_hour_ads, assign_client, assign_program_series, pause_media, set_media_priority.',
            'Never invent media IDs or slot IDs. Use only IDs present in context.',
            'If user request is ambiguous, output assumptions and minimal-safe operations.',
        ].join('\n');

        const buildUserPromptBody = (critiques: string[]) => {
            const lines = [
                `Operator request:\n${request}`,
                '',
                `Runtime context JSON:\n${JSON.stringify(ctx)}`,
                '',
                `Available media IDs sample:\n${JSON.stringify(media.slice(0, 500).map((m) => ({ id: m.id, title: m.title, type: m.type, client: m.client || '', series: m.programSeries || '', duration: m.duration })))}`,
            ];
            if (critiques.length) {
                lines.push('', '---', 'Prior validation failures (return corrected JSON only):');
                critiques.forEach((c, i) => lines.push(`${i + 1}. ${c}`));
            }
            return lines.join('\n');
        };

        let planRaw: unknown;
        const storedCreds = getStoredOpenRouterCredentials();
        if (await isHermesSupervisorEnabled()) {
            const model = getHermesSupervisorModelOrDefault(storedCreds.model || undefined);
            const sup = await runOpenRouterSupervisorJson({
                system: supervisorSystem,
                buildUserPrompt: (critiques) => buildUserPromptBody(critiques),
                validate: (parsed) =>
                    parseSchedulingAgentPlan(parsed)
                        ? { ok: true }
                        : {
                              ok: false,
                              critique:
                                  'JSON must be an object with summary, assumptions, risks, and operations[]. Each operation needs string type and reason. Use only slot and media IDs from context.',
                          },
                model,
                temperature: 0.15,
                maxTokens: 1800,
                learningTag: 'scheduling_agent',
            });
            if (!sup.success) {
                return {
                    success: false,
                    dryRun,
                    summary: 'Hermes supervisor could not produce a valid plan.',
                    plan: null,
                    appliedOperations,
                    changedSlots: [],
                    changedMedia: [],
                    warnings: [`Hermes ended after ${sup.attempts} attempt(s).`, ...sup.critiques.slice(-2)],
                    error: sup.lastError || 'Supervisor exhausted.',
                };
            }
            planRaw = sup.data;
        } else {
            planRaw = await requestOpenRouterJson({
                temperature: 0.15,
                maxTokens: 1800,
                system: supervisorSystem,
                prompt: buildUserPromptBody([]),
            });
        }

        const plan = parseSchedulingAgentPlan(planRaw);
        if (!plan) {
            return {
                success: false,
                dryRun,
                summary: 'Agent failed to produce a valid execution plan.',
                plan: null,
                appliedOperations,
                changedSlots: [],
                changedMedia: [],
                warnings: ['Plan parse failure from model response.'],
                error: 'Invalid plan format from OpenRouter.',
            };
        }

        const nextSchedule: WeeklySchedule = {
            ...schedule,
            slots: { ...schedule.slots },
        };
        const nextMediaById = new Map<string, MediaItem>(media.map((m) => [m.id, { ...m }]));

        const ensureSlot = (slotId: string): HourSlot | null => {
            const existing = nextSchedule.slots[slotId];
            if (!existing) {
                warnings.push(`Slot not found: ${slotId}`);
                return null;
            }
            const copy = cloneSlot(existing);
            nextSchedule.slots[slotId] = copy;
            changedSlots.add(slotId);
            return copy;
        };

        for (const op of plan.operations) {
            switch (op.type) {
                case 'set_slot_playlist': {
                    if (!op.slotId || !op.mediaIds?.length) {
                        warnings.push('set_slot_playlist skipped (missing slotId/mediaIds)');
                        break;
                    }
                    const slot = ensureSlot(op.slotId);
                    if (!slot) break;
                    const items = op.mediaIds
                        .map((id) => nextMediaById.get(id))
                        .filter((m): m is MediaItem => !!m)
                        .map((m) => createPlaylistItem(m));
                    if (items.length === 0) {
                        warnings.push(`set_slot_playlist produced no valid items for ${op.slotId}`);
                        break;
                    }
                    slot.items = items;
                    appliedOperations.push(`set_slot_playlist(${op.slotId})`);
                    break;
                }
                case 'append_to_slot': {
                    if (!op.slotId || !op.mediaIds?.length) {
                        warnings.push('append_to_slot skipped (missing slotId/mediaIds)');
                        break;
                    }
                    const slot = ensureSlot(op.slotId);
                    if (!slot) break;
                    const items = op.mediaIds
                        .map((id) => nextMediaById.get(id))
                        .filter((m): m is MediaItem => !!m)
                        .map((m) => createPlaylistItem(m));
                    slot.items = [...slot.items, ...items];
                    appliedOperations.push(`append_to_slot(${op.slotId}, +${items.length})`);
                    break;
                }
                case 'rebalance_hour_ads': {
                    if (!op.slotId) {
                        warnings.push('rebalance_hour_ads skipped (missing slotId)');
                        break;
                    }
                    const slot = ensureSlot(op.slotId);
                    if (!slot) break;
                    const minSep = Math.max(1, op.minSeparationItems || 2);
                    const ads = slot.items.filter((x) => x.media.type === 'ad');
                    const nonAds = slot.items.filter((x) => x.media.type !== 'ad');
                    if (ads.length === 0 || nonAds.length === 0) {
                        warnings.push(`rebalance_hour_ads skipped for ${op.slotId} (insufficient ad/non-ad mix)`);
                        break;
                    }
                    const rebuilt: PlaylistItem[] = [];
                    let adIdx = 0;
                    let nonIdx = 0;
                    let sinceAd = minSep;
                    while (nonIdx < nonAds.length || adIdx < ads.length) {
                        const shouldAd = adIdx < ads.length && sinceAd >= minSep;
                        if (shouldAd) {
                            rebuilt.push(ads[adIdx++]);
                            sinceAd = 0;
                        } else if (nonIdx < nonAds.length) {
                            rebuilt.push(nonAds[nonIdx++]);
                            sinceAd += 1;
                        } else if (adIdx < ads.length) {
                            rebuilt.push(ads[adIdx++]);
                        }
                    }
                    slot.items = rebuilt;
                    appliedOperations.push(`rebalance_hour_ads(${op.slotId}, sep=${minSep})`);
                    break;
                }
                case 'assign_client':
                case 'assign_program_series':
                case 'pause_media':
                case 'set_media_priority': {
                    if (!op.mediaIds?.length) {
                        warnings.push(`${op.type} skipped (missing mediaIds)`);
                        break;
                    }
                    let touched = 0;
                    for (const mediaId of op.mediaIds) {
                        const item = nextMediaById.get(mediaId);
                        if (!item) continue;
                        const updated = { ...item };
                        if (op.type === 'assign_client' && op.client) updated.client = op.client;
                        if (op.type === 'assign_program_series' && op.programSeries) updated.programSeries = op.programSeries;
                        if (op.type === 'pause_media' && typeof op.paused === 'boolean') updated.paused = op.paused;
                        if (op.type === 'set_media_priority' && typeof op.priority === 'number') updated.priority = op.priority;
                        nextMediaById.set(mediaId, updated);
                        changedMedia.add(mediaId);
                        touched += 1;
                    }
                    appliedOperations.push(`${op.type}(${touched})`);
                    break;
                }
                default:
                    warnings.push(`Unsupported operation ignored: ${String(op.type)}`);
                    break;
            }
        }

        if (!dryRun) {
            await saveWeeklySchedule(nextSchedule);
            const nextLibrary = loadLibraryMedia([]);
            const mergedMap = new Map<string, MediaItem>(nextLibrary.map((m) => [m.id, m]));
            changedMedia.forEach((id) => {
                const updated = nextMediaById.get(id);
                if (updated) mergedMap.set(id, updated);
            });
            const merged = Array.from(mergedMap.values());
            saveLibraryMedia(merged);
            await Promise.all(
                Array.from(changedMedia).map(async (id) => {
                    const updated = nextMediaById.get(id);
                    if (updated) await dbSaveMedia(updated);
                }),
            );
        }

        const summaryPrefix = dryRun ? 'Dry run only. ' : 'Applied plan. ';
        return {
            success: true,
            dryRun,
            summary: `${summaryPrefix}${plan.summary}`,
            plan,
            appliedOperations,
            changedSlots: Array.from(changedSlots),
            changedMedia: Array.from(changedMedia),
            warnings,
        };
    } catch (error) {
        return {
            success: false,
            dryRun,
            summary: 'Scheduling agent execution failed.',
            plan: null,
            appliedOperations,
            changedSlots: [],
            changedMedia: [],
            warnings,
            error: error instanceof Error ? error.message : String(error),
        };
    }
}
