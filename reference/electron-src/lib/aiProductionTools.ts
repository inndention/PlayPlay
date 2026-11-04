import { MediaItem } from './types';
import { requestOpenRouterJson } from './aiHelper';
import { generateTTS, TTSOptions } from './whisperTTS';
import { buildSttRecordWorkflow } from './voiceIntelligence';

export type ProductionToolCategory =
    | 'voice'
    | 'break'
    | 'breaking_news'
    | 'webrtc'
    | 'weather'
    | 'sports'
    | 'psa';

export interface ProductionContext {
    stationName?: string;
    siteName?: string;
    programTitle?: string;
    nextProgramTitle?: string;
    remainingSeconds?: number;
    storySlug?: string;
    reporterName?: string;
    webrtcSessionId?: string;
    sourceText?: string;
}

export interface ProductionTool {
    id: string;
    title: string;
    category: ProductionToolCategory;
    trigger: string;
    description: string;
    defaultScript: string;
    steps: string[];
    automationTags: string[];
    requires?: string[];
}

export interface ProductionCuePlan {
    id: string;
    toolId: string;
    title: string;
    category: ProductionToolCategory;
    script: string;
    steps: string[];
    cgLayers: string[];
    audioCue: string;
    playoutActions: string[];
    webrtcActions: string[];
    automationTags: string[];
    createdAt: string;
}

interface OpenRouterCueDraft {
    script?: string;
    steps?: string[];
}

const DEFAULT_CONTEXT: Required<Pick<ProductionContext, 'stationName' | 'siteName' | 'programTitle' | 'nextProgramTitle' | 'storySlug' | 'reporterName'>> = {
    stationName: 'Star News',
    siteName: 'local viewers',
    programTitle: 'the current program',
    nextProgramTitle: 'your next program',
    storySlug: 'a developing local story',
    reporterName: 'our reporter',
};

export const AI_PRODUCTION_TOOLS: ProductionTool[] = [
    {
        id: 'top_of_hour_voice',
        title: 'Top-of-hour voice open',
        category: 'voice',
        trigger: 'At :00 before program or news block',
        description: 'Kokoro-grade in-studio station reset with time, region, and next program tease.',
        defaultScript: '{stationName}. It is the top of the hour for {siteName}. Coming up next, {nextProgramTitle}.',
        steps: ['Render short Kokoro/local TTS voiceover.', 'Index generated script as station context.', 'Load station ID or clock slate.', 'Play voice over bed or clean channel.', 'Start next scheduled item.'],
        automationTags: ['top-of-hour', 'tts', 'kokoro', 'station-id'],
        requires: ['Kokoro-compatible TTS', 'schedule clock'],
    },
    {
        id: 'station_id_voice',
        title: 'Station ID voice',
        category: 'voice',
        trigger: 'Any legal ID, gap, or filler slot',
        description: 'Quick high-quality local voice ID that can fill short holes without sounding repetitive.',
        defaultScript: 'You are watching {stationName}, serving {siteName}.',
        steps: ['Choose current site voice profile.', 'Render a 7 to 10 second read.', 'Pair with logo or live bug.', 'Return to schedule.'],
        automationTags: ['station-id', 'tts', 'kokoro', 'filler'],
    },
    {
        id: 'break_almost_over_60',
        title: 'Break warning 60 seconds',
        category: 'break',
        trigger: 'Commercial break has about 60 seconds remaining',
        description: 'Operator and optional studio warning that the show return is near.',
        defaultScript: 'Break is almost over. About one minute until {programTitle} returns.',
        steps: ['Warn control room.', 'Cue next program source.', 'Prepare return CG.', 'Confirm next audio source.'],
        automationTags: ['break', 'warning', 'cue'],
    },
    {
        id: 'break_almost_over_30',
        title: 'Break warning 30 seconds',
        category: 'break',
        trigger: 'Commercial break has about 30 seconds remaining',
        description: 'Final clean cue before returning from break.',
        defaultScript: 'Thirty seconds. Stand by to return to {programTitle}.',
        steps: ['Flash break countdown.', 'Pre-roll next source if needed.', 'Arm return transition.', 'Mute non-program mics.'],
        automationTags: ['break', 'countdown', 'return'],
    },
    {
        id: 'break_cue_next_program',
        title: 'Break return cue',
        category: 'break',
        trigger: 'Last spot or bumper in a break',
        description: 'Cues the next show, segment, or live host out of a commercial block.',
        defaultScript: 'Stand by. Returning now to {programTitle}.',
        steps: ['Mark final ad as playing.', 'Queue return bumper if needed.', 'Take program source.', 'Resume as-played logging.'],
        automationTags: ['break', 'return', 'as-played'],
    },
    {
        id: 'breaking_news_cue',
        title: 'Breaking news cue package',
        category: 'breaking_news',
        trigger: 'Producer marks a breaking story ready',
        description: 'One-button package for voice, lower third, stinger, live-shot preview, and ticker.',
        defaultScript: 'Breaking news from {stationName}. We are following {storySlug}. We will bring you verified updates as they come in.',
        steps: ['Pause nonessential filler.', 'Load breaking news CG preset.', 'Play breaking sting or bumper.', 'Cue anchor voice or TTS read.', 'Open live preview if available.'],
        automationTags: ['breaking-news', 'cg', 'voice', 'webrtc'],
        requires: ['CG layer', 'audio sting'],
    },
    {
        id: 'breaking_news_update_loop',
        title: 'Breaking update loop',
        category: 'breaking_news',
        trigger: 'Breaking story remains active',
        description: 'Rotates fresh update copy, ticker text, and verified recap language.',
        defaultScript: 'Update on {storySlug}: details are still developing. Stay with {stationName} for verified local information.',
        steps: ['Refresh source notes.', 'Regenerate concise update copy.', 'Update crawl text.', 'Schedule next update window.'],
        automationTags: ['breaking-news', 'ticker', 'recap'],
    },
    {
        id: 'webrtc_field_check',
        title: 'WebRTC field check',
        category: 'webrtc',
        trigger: 'Reporter joins from scene',
        description: 'Preflight for a phone or laptop live shot before it is shown on program.',
        defaultScript: '{reporterName}, you are in preview. We are checking picture, audio, and return cue.',
        steps: ['Join WebRTC room in preview only.', 'Check audio level and echo risk.', 'Confirm landscape framing.', 'Set lower third.', 'Keep backup video armed.'],
        automationTags: ['webrtc', 'field', 'preview'],
        requires: ['WebRTC session', 'monitor audio'],
    },
    {
        id: 'webrtc_take_live',
        title: 'Take WebRTC live',
        category: 'webrtc',
        trigger: 'Producer takes a field source to air',
        description: 'Smooth transition from scheduled playout to live WebRTC source with backup return.',
        defaultScript: 'We go now live to {reporterName} with more on {storySlug}.',
        steps: ['Roll intro or anchor toss.', 'Fade program audio under.', 'Take WebRTC source to program.', 'Show lower third.', 'Log live-shot start.'],
        automationTags: ['webrtc', 'live-shot', 'transition'],
        requires: ['WebRTC session', 'CG lower third'],
    },
    {
        id: 'webrtc_wrap_and_return',
        title: 'Wrap live shot',
        category: 'webrtc',
        trigger: 'Reporter tosses back',
        description: 'Returns cleanly from WebRTC field source to scheduled playout or anchor.',
        defaultScript: 'Thank you, {reporterName}. We will continue to follow {storySlug}.',
        steps: ['Cue wrap line.', 'Fade WebRTC audio down.', 'Take anchor or scheduled source.', 'Clear live lower third.', 'Log live-shot end.'],
        automationTags: ['webrtc', 'return', 'as-played'],
    },
    {
        id: 'weather_voice_fill',
        title: 'Weather voice filler',
        category: 'weather',
        trigger: 'Short gap, dead time, or weather block',
        description: 'High-quality local TTS weather read paired with a dynamic vector graphic.',
        defaultScript: 'Here is a quick weather check for {siteName}. Conditions can change, so keep an eye on local alerts through the day.',
        steps: ['Fetch weather summary.', 'Render voice read.', 'Generate weather graphic.', 'Schedule as short filler.'],
        automationTags: ['weather', 'tts', 'kokoro', 'filler'],
        requires: ['Weather provider'],
    },
    {
        id: 'sports_voice_tease',
        title: 'School sports tease',
        category: 'sports',
        trigger: 'Before sports block or open filler',
        description: 'Quick local sports tease based on scraped school events.',
        defaultScript: 'Local school sports are coming up. Send verified scores and photos to {stationName}.',
        steps: ['Pull school events.', 'Choose top regional matches.', 'Render short voice read.', 'Attach coming-up graphic.'],
        automationTags: ['sports', 'school', 'voice'],
    },
    {
        id: 'psa_coming_up',
        title: 'Coming up / PSA fill',
        category: 'psa',
        trigger: 'Open slot where a station-safe filler is needed',
        description: 'Clever public service, coming-up, or community filler that can rotate safely.',
        defaultScript: 'Coming up soon on {stationName}: {nextProgramTitle}. Stay with us for more local coverage.',
        steps: ['Check next scheduled program.', 'Select PSA or coming-up template.', 'Render CG and optional voice.', 'Insert as low-priority filler.'],
        automationTags: ['psa', 'coming-up', 'filler'],
    },
    {
        id: 'stt_context_index',
        title: 'STT context index',
        category: 'voice',
        trigger: 'After import, live shot, or as-played recording',
        description: 'Transcribes audio and builds searchable context records for shows, clients, events, sponsors, places, and reports.',
        defaultScript: 'Index this audio for transcript search, sponsor hints, event matching, and as-played reports.',
        steps: buildSttRecordWorkflow(),
        automationTags: ['stt', 'transcript', 'context-index', 'reports'],
        requires: ['Local Whisper-compatible STT'],
    },
];

function applyContext(template: string, context: ProductionContext): string {
    const merged = { ...DEFAULT_CONTEXT, ...context };
    return template.replace(/\{(\w+)\}/g, (_, key: keyof ProductionContext) => {
        const value = merged[key as keyof typeof merged];
        if (typeof value === 'number') return String(value);
        return value || '';
    });
}

function makePlan(tool: ProductionTool, context: ProductionContext, script?: string, steps?: string[]): ProductionCuePlan {
    const categoryTags = [tool.category, ...tool.automationTags];
    const webrtcActions = tool.category === 'webrtc'
        ? ['preview WebRTC source', 'confirm monitor audio', 'take or clear live source through transition']
        : [];
    const playoutActions = tool.category === 'break'
        ? ['update break countdown', 'cue next scheduled source', 'write as-played cue marker']
        : ['create operator cue', 'prepare linked CG or audio asset', 'log cue plan'];

    return {
        id: `prod_cue_${tool.id}_${Date.now()}`,
        toolId: tool.id,
        title: tool.title,
        category: tool.category,
        script: script || applyContext(tool.defaultScript, context),
        steps: steps?.length ? steps : tool.steps,
        cgLayers: categoryTags.includes('cg') || tool.category === 'breaking_news'
            ? ['lower third', 'ticker/crawl', 'safe-title headline']
            : ['optional bug or slate'],
        audioCue: tool.category === 'voice' || tool.category === 'weather' || tool.category === 'sports' || tool.category === 'psa'
            ? 'Render natural voice and duck bed if present'
            : 'Operator cue only unless voice preview is requested',
        playoutActions,
        webrtcActions,
        automationTags: tool.automationTags,
        createdAt: new Date().toISOString(),
    };
}

export function getProductionTools(category?: ProductionToolCategory): ProductionTool[] {
    return category ? AI_PRODUCTION_TOOLS.filter(tool => tool.category === category) : AI_PRODUCTION_TOOLS;
}

export async function buildProductionCuePlan(toolId: string, context: ProductionContext = {}): Promise<ProductionCuePlan> {
    const tool = AI_PRODUCTION_TOOLS.find(candidate => candidate.id === toolId);
    if (!tool) throw new Error(`Unknown production tool: ${toolId}`);

    const fallback = makePlan(tool, context);
    const draft = await requestOpenRouterJson<OpenRouterCueDraft>({
        system: 'You are an experienced local TV technical director and producer. Return compact JSON only.',
        prompt: [
            `Build a production cue for: ${tool.title}`,
            `Context: ${JSON.stringify(context)}`,
            `Default script: ${fallback.script}`,
            'Return JSON with script and steps. Keep copy short, broadcast-safe, and operationally specific.',
        ].join('\n'),
        temperature: 0.35,
        maxTokens: 650,
    }).catch(() => null);

    if (!draft || typeof draft === 'string') return fallback;
    return makePlan(
        tool,
        context,
        typeof draft.script === 'string' ? draft.script : fallback.script,
        Array.isArray(draft.steps) ? draft.steps.filter(step => typeof step === 'string') : fallback.steps,
    );
}

export function buildBreakTimingWarnings(remainingSeconds: number, context: ProductionContext = {}): ProductionCuePlan[] {
    const ids = [
        { threshold: 60, toolId: 'break_almost_over_60' },
        { threshold: 30, toolId: 'break_almost_over_30' },
        { threshold: 10, toolId: 'break_cue_next_program' },
    ];

    return ids
        .filter(item => remainingSeconds <= item.threshold)
        .map(item => {
            const tool = AI_PRODUCTION_TOOLS.find(candidate => candidate.id === item.toolId)!;
            return makePlan(tool, { ...context, remainingSeconds });
        });
}

export function createVoiceMediaFromCue(plan: ProductionCuePlan): MediaItem {
    return {
        id: `voice_${plan.id}`,
        title: plan.title,
        duration: Math.max(8, Math.ceil(plan.script.length / 12)),
        src: 'signal:voice-cue',
        type: plan.category === 'break' ? 'bumper' : 'station_id',
        mediaClass: 'filler',
        notes: plan.script,
        addedAt: new Date().toISOString(),
    };
}

export async function renderProductionVoice(plan: ProductionCuePlan, options: TTSOptions = {}): Promise<Blob | null> {
    return generateTTS(plan.script, {
        voice: options.voice || 'af_heart',
        speed: options.speed || 1,
        model: options.model || 'kokoro',
        provider: options.provider || 'auto',
    });
}
