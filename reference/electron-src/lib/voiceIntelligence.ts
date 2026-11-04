import { ExtendedSettings, MediaContentIndex, MediaItem, STTRecord } from './types';
import { requestOpenRouterJson } from './aiHelper';
import { loadSettings } from './storage';

export interface VoiceProviderProfile {
    id: 'kokoro-local' | 'starnews' | 'openai' | 'local-whisper' | 'openai-whisper';
    label: string;
    role: 'tts' | 'stt';
    quality: 'high' | 'utility' | 'fallback';
    localFirst: boolean;
    endpointHint: string;
    useFor: string[];
}

export interface SpeechIndexDraft {
    summary?: string;
    topics?: string[];
    people?: string[];
    places?: string[];
    clients?: string[];
    events?: string[];
    sponsorCandidates?: string[];
    programCandidates?: string[];
    contextTags?: string[];
}

export const VOICE_PROVIDER_PROFILES: VoiceProviderProfile[] = [
    {
        id: 'kokoro-local',
        label: 'Kokoro local TTS',
        role: 'tts',
        quality: 'high',
        localFirst: true,
        endpointHint: 'http://127.0.0.1:8880/v1/audio/speech',
        useFor: ['top-of-hour reads', 'weather fills', 'PSA fillers', 'studio cue warnings', 'station voice'],
    },
    {
        id: 'local-whisper',
        label: 'Local Whisper STT',
        role: 'stt',
        quality: 'high',
        localFirst: true,
        endpointHint: 'http://127.0.0.1:9000/v1/audio/transcriptions',
        useFor: ['import transcript records', 'as-played speech logs', 'live-shot notes', 'search indexing'],
    },
    {
        id: 'starnews',
        label: 'Star News narrate endpoint',
        role: 'tts',
        quality: 'utility',
        localFirst: false,
        endpointHint: 'Configured Star News API base URL',
        useFor: ['site-integrated narration', 'queued filler narration'],
    },
    {
        id: 'openai',
        label: 'OpenAI speech fallback',
        role: 'tts',
        quality: 'fallback',
        localFirst: false,
        endpointHint: 'https://api.openai.com/v1/audio/speech',
        useFor: ['backup speech rendering when local services are offline'],
    },
    {
        id: 'openai-whisper',
        label: 'OpenAI Whisper fallback',
        role: 'stt',
        quality: 'fallback',
        localFirst: false,
        endpointHint: 'https://api.openai.com/v1/audio/transcriptions',
        useFor: ['backup transcription for imports and production logs'],
    },
];

const IMPORTANT_WORDS = new Set([
    'martinsville', 'henry', 'danville', 'pittsylvania', 'rockingham', 'eden',
    'reidsville', 'school', 'sports', 'weather', 'traffic', 'vdot', 'ncdot',
    'sponsor', 'festival', 'parade', 'closing', 'delay', 'breaking',
]);

function compactWords(text: string): string[] {
    return text
        .toLowerCase()
        .split(/[^a-z0-9$]+/)
        .map(word => word.trim())
        .filter(word => word.length > 2);
}

function unique(values: string[]): string[] {
    return Array.from(new Set(values.filter(Boolean))).slice(0, 24);
}

function titleCandidate(value: string): string {
    return value.replace(/\b\w/g, char => char.toUpperCase());
}

export function getVoiceProviderProfiles(): VoiceProviderProfile[] {
    return VOICE_PROVIDER_PROFILES;
}

export function getRecommendedVoiceStack(settings: ExtendedSettings = loadSettings() as ExtendedSettings): VoiceProviderProfile[] {
    const preferredTts = settings.ttsProvider || 'auto';
    const preferredStt = settings.sttProvider || 'auto';
    return VOICE_PROVIDER_PROFILES.filter(profile => {
        if (profile.id === 'kokoro-local') return preferredTts === 'auto' || preferredTts === 'kokoro-local';
        if (profile.id === 'local-whisper') return preferredStt === 'auto' || preferredStt === 'local-whisper';
        if (profile.id === 'starnews') return preferredTts === 'auto' || preferredTts === 'starnews';
        if (profile.id === 'openai') return preferredTts === 'auto' || preferredTts === 'openai';
        if (profile.id === 'openai-whisper') return preferredStt === 'auto' || preferredStt === 'openai-whisper';
        return true;
    });
}

export function buildContextIndexFromText(text: string, media: Partial<MediaItem> = {}): MediaContentIndex {
    const words = compactWords(`${media.title || ''} ${media.client || ''} ${media.programSeries || ''} ${text}`);
    const topics = unique(words.filter(word => IMPORTANT_WORDS.has(word) || word.startsWith('$')).map(titleCandidate));
    const sponsorCandidates = unique(words
        .filter(word => word.includes('sponsor') || word.includes('bank') || word.includes('auto') || word.includes('health'))
        .map(titleCandidate));
    const programCandidates = unique([
        media.programSeries || '',
        ...(words.includes('news') ? ['News'] : []),
        ...(words.includes('sports') ? ['Sports'] : []),
        ...(words.includes('weather') ? ['Weather'] : []),
    ]);

    return {
        summary: text.split(/[.!?]/).map(part => part.trim()).filter(Boolean).slice(0, 2).join('. '),
        topics,
        people: [],
        places: unique(words.filter(word => ['martinsville', 'danville', 'eden', 'reidsville', 'rockingham'].includes(word)).map(titleCandidate)),
        clients: unique([media.client || '']),
        events: unique(words.filter(word => ['festival', 'parade', 'game', 'closing', 'delay'].includes(word)).map(titleCandidate)),
        sponsorCandidates,
        programCandidates,
        contextTags: unique([...topics, ...sponsorCandidates, ...programCandidates].map(tag => tag.toLowerCase().replace(/\s+/g, '-'))),
        lastIndexedAt: new Date().toISOString(),
    };
}

export async function buildAiContextIndex(text: string, media: Partial<MediaItem> = {}): Promise<MediaContentIndex> {
    const fallback = buildContextIndexFromText(text, media);
    const draft = await requestOpenRouterJson<SpeechIndexDraft>({
        system: 'You index broadcast media for a local TV playout library. Return compact JSON only.',
        prompt: [
            `Media title: ${media.title || 'unknown'}`,
            `Known client: ${media.client || 'none'}`,
            `Known program series: ${media.programSeries || 'none'}`,
            `Transcript or notes: ${text}`,
            'Return summary, topics, people, places, clients, events, sponsorCandidates, programCandidates, contextTags.',
        ].join('\n'),
        temperature: 0.2,
        maxTokens: 700,
    }).catch(() => null);

    if (!draft || typeof draft === 'string') return fallback;
    return {
        summary: draft.summary || fallback.summary,
        topics: unique([...(draft.topics || []), ...fallback.topics]),
        people: unique(draft.people || []),
        places: unique([...(draft.places || []), ...fallback.places]),
        clients: unique([...(draft.clients || []), ...fallback.clients]),
        events: unique([...(draft.events || []), ...fallback.events]),
        sponsorCandidates: unique([...(draft.sponsorCandidates || []), ...fallback.sponsorCandidates]),
        programCandidates: unique([...(draft.programCandidates || []), ...fallback.programCandidates]),
        contextTags: unique([...(draft.contextTags || []), ...fallback.contextTags]),
        lastIndexedAt: new Date().toISOString(),
    };
}

export async function transcribeAudioBlob(blob: Blob, filename = 'recording.wav'): Promise<STTRecord | null> {
    const settings = loadSettings() as ExtendedSettings;
    const provider = settings.sttProvider || 'auto';
    const makeForm = (model: string) => {
        const form = new FormData();
        form.append('file', blob, filename);
        form.append('model', model);
        return form;
    };

    const tryLocal = async () => {
        const baseUrl = (settings.localWhisperBaseUrl || 'http://127.0.0.1:9000').replace(/\/$/, '');
        const response = await fetch(`${baseUrl}/v1/audio/transcriptions`, { method: 'POST', body: makeForm('whisper') });
        if (!response.ok) throw new Error(`Local STT failed: ${response.status}`);
        return response.json();
    };

    const tryOpenAI = async () => {
        if (!settings.openaiApiKey) return null;
        const response = await fetch('https://api.openai.com/v1/audio/transcriptions', {
            method: 'POST',
            headers: { Authorization: `Bearer ${settings.openaiApiKey}` },
            body: makeForm('whisper-1'),
        });
        if (!response.ok) throw new Error(`OpenAI STT failed: ${response.status}`);
        return response.json();
    };

    const data = provider === 'openai-whisper'
        ? await tryOpenAI().catch(() => null)
        : await tryLocal().catch(() => provider === 'auto' ? tryOpenAI().catch(() => null) : null);

    const transcript = typeof data?.text === 'string' ? data.text : '';
    if (!transcript) return null;

    return {
        id: `stt_${Date.now()}`,
        provider: provider === 'openai-whisper' ? 'openai-whisper' : 'local-whisper',
        transcript,
        language: data.language,
        createdAt: new Date().toISOString(),
        source: 'operator',
    };
}

export function attachTranscriptIndex(media: MediaItem, record: STTRecord, index?: MediaContentIndex): MediaItem {
    const records = [record, ...(media.transcriptRecords || [])];
    return {
        ...media,
        transcriptRecords: records,
        contentIndex: index || buildContextIndexFromText(record.transcript, media),
    };
}

export function buildSttRecordWorkflow(): string[] {
    return [
        'Record or ingest audio from import, live shot, or as-played monitor.',
        'Transcribe with local Whisper first, then OpenAI Whisper only as fallback.',
        'Build context index from transcript, title, client, program, and region tags.',
        'Ask operator to confirm likely sponsor, event, show, person, and place matches.',
        'Attach transcript record and context index to media and future as-played reports.',
    ];
}
