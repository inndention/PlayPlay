import { getStoredOpenRouterCredentials } from './aiHelper';

/**
 * Client-side wrappers for the FFmpeg analysis engine + LLM context layer.
 *
 * analyzeAsset()   — triggers background analysis, returns cached data if available
 * pollUntilDone()  — polls getResult every few seconds until analysis completes
 * runLlmContext()  — sends analysis data to OpenRouter and saves context back
 */

export interface AssetAnalysis {
    assetId: string;
    path: string;
    filename: string;
    analyzedAt: string;
    duration: number;
    format: { name: string; longName: string; size: number; bitrate: number; tags: Record<string, string> };
    video: {
        codec: string; profile: string; width: number; height: number;
        fps: number; pixFmt: string; bitrate: number; scanType: string;
    } | null;
    audio: {
        codec: string; channels: number; channelLayout: string; sampleRate: number;
        bitrate: number;
        loudness: { integrated: number | null; lra: number | null; truePeak: number | null; threshold: number | null };
        silenceSegments: Array<{ start: number; end: number }>;
        transcript: string | null;
    } | null;
    video_analysis: {
        thumbnails: string[];
        blackSegments: Array<{ start: number; end: number; duration: number }>;
        freezeSegments: Array<{ start: number; end: number }>;
    };
    context: {
        summary: string;
        topics: string[];
        people: string[];
        contentType: string;
        riskFlags: string[];
        adBreakSuggestions: number[];
        broadcastNotes: string;
        llmAnalyzedAt: string | null;
        confidence?: number;
    };
    ffmpegVersion?: string;
    featuresUsed?: string[];
}

export interface FFmpegFeatures {
    version: string;
    ffmpegPath: string;
    ffprobePath: string;
    caps: Record<string, boolean>;
}

const api = () => (window as any).electronAPI;

export async function detectFFmpegFeatures(): Promise<FFmpegFeatures | null> {
    const r = await api()?.analyzeDetectFeatures?.();
    return r?.ok ? r.data : null;
}

export async function analyzeAsset(filePath: string, opts: { force?: boolean; transcribe?: boolean } = {}): Promise<AssetAnalysis | null> {
    const r = await api()?.analyzeAsset?.(filePath, opts);
    return r?.ok ? r.data : null;
}

export async function getAnalysisResult(filePath: string): Promise<AssetAnalysis | null> {
    const r = await api()?.analyzeGetResult?.(filePath);
    return r?.ok ? r.data : null;
}

export async function pollUntilDone(
    filePath: string,
    onProgress?: (status: string) => void,
    timeoutMs = 300000
): Promise<AssetAnalysis | null> {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        const job = await api()?.analyzeGetJob?.(filePath);
        const s = job?.data?.status;
        onProgress?.(s || 'queued');
        if (s === 'complete') return job.data.result ?? await getAnalysisResult(filePath);
        if (s === 'failed') return null;
        await new Promise(r => setTimeout(r, 3000));
    }
    return null;
}

/** Send analysis + optional transcript to LLM, save context back. */
export async function runLlmContext(analysis: AssetAnalysis): Promise<AssetAnalysis['context'] | null> {
    const { apiKey, model } = getStoredOpenRouterCredentials();

    if (!apiKey) return null;

    const prompt = buildContextPrompt(analysis);

    const r = await api()?.ai?.openRouterJson?.({
        apiKey,
        model,
        system: `You are a broadcast media analyst. Analyze the provided media asset metadata and return a JSON object with exactly these fields:
{
  "summary": "string — 1-3 sentence plain English summary",
  "topics": ["array of relevant topics"],
  "people": ["array of detected or likely people/speakers if identifiable from context"],
  "contentType": "one of: news_package|interview|commercial|promo|psa|weather|sports|movie|broll|raw_feed|unknown",
  "broadcastNotes": "string — any broadcast safety concerns, quality issues, or scheduling notes",
  "confidence": 0-100
}
Return only valid JSON. Do not wrap in markdown.`,
        prompt,
        maxTokens: 600,
        temperature: 0.1,
    });

    if (!r?.success || !r.parsed) return null;

    const context: Partial<AssetAnalysis['context']> = {
        summary:        r.parsed.summary        || '',
        topics:         r.parsed.topics         || [],
        people:         r.parsed.people         || [],
        contentType:    r.parsed.contentType    || 'unknown',
        broadcastNotes: r.parsed.broadcastNotes || '',
        confidence:     r.parsed.confidence     ?? undefined,
        riskFlags:      analysis.context.riskFlags,
        adBreakSuggestions: analysis.context.adBreakSuggestions,
    };

    // Persist to disk via main process
    await api()?.analyzeSaveLlmContext?.(analysis.path, context);
    return context as AssetAnalysis['context'];
}

function buildContextPrompt(a: AssetAnalysis): string {
    const lines: string[] = [];
    lines.push(`Filename: ${a.filename}`);
    lines.push(`Duration: ${Math.round(a.duration)}s`);
    if (a.format.tags) {
        const tags = a.format.tags;
        if (tags.title)   lines.push(`Title tag: ${tags.title}`);
        if (tags.comment) lines.push(`Comment: ${tags.comment}`);
        if (tags.artist)  lines.push(`Artist: ${tags.artist}`);
    }
    if (a.video) {
        lines.push(`Video: ${a.video.width}x${a.video.height} ${a.video.codec} ${a.video.fps?.toFixed(2)}fps`);
    }
    if (a.audio) {
        lines.push(`Audio: ${a.audio.codec} ${a.audio.channels}ch ${a.audio.sampleRate}Hz`);
        if (a.audio.loudness.integrated !== null) {
            lines.push(`Loudness: ${a.audio.loudness.integrated?.toFixed(1)} LUFS, LRA ${a.audio.loudness.lra?.toFixed(1)} LU`);
        }
        if (a.audio.transcript) {
            const snip = a.audio.transcript.slice(0, 600);
            lines.push(`Transcript excerpt: "${snip}${snip.length < a.audio.transcript.length ? '...' : ''}"`);
        }
        if (a.audio.silenceSegments.length) {
            lines.push(`Silence segments: ${a.audio.silenceSegments.length} detected`);
        }
    }
    if (a.context.riskFlags.length) {
        lines.push(`Quality flags: ${a.context.riskFlags.join(', ')}`);
    }
    return lines.join('\n');
}
