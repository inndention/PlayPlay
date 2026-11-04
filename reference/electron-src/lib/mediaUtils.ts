/* lib/mediaUtils.ts */

// Helper to create a video element and load file
const loadVideo = (file: File): Promise<HTMLVideoElement> => {
    return new Promise((resolve, reject) => {
        const video = document.createElement('video');
        video.preload = 'metadata';
        video.onloadedmetadata = () => resolve(video);
        video.onerror = reject;
        video.src = URL.createObjectURL(file);
    });
};

const seekTo = (video: HTMLVideoElement, time: number): Promise<void> => {
    return new Promise((resolve) => {
        const onSeek = () => {
            video.removeEventListener('seeked', onSeek);
            resolve();
        };
        video.addEventListener('seeked', onSeek);
        video.currentTime = time;
    });
};

export const generateThumbnail = async (file: File): Promise<string> => {
    try {
        const video = await loadVideo(file);
        // Seek to 10% or 1s, whichever is reliable, or reasonable "middle" if short
        // Let's try 1 second, or middle if duration < 2s
        const time = video.duration < 2 ? video.duration / 2 : 1;
        await seekTo(video, time);

        const canvas = document.createElement('canvas');
        canvas.width = video.videoWidth;
        canvas.height = video.videoHeight;
        // Limit max resolution for thumb to save memory (e.g. 320px width)
        const scale = 320 / video.videoWidth;
        canvas.width = 320;
        canvas.height = video.videoHeight * scale;

        const ctx = canvas.getContext('2d');
        if (ctx) {
            ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
            const dataUrl = canvas.toDataURL('image/jpeg', 0.7);
            URL.revokeObjectURL(video.src);
            return dataUrl;
        }
        URL.revokeObjectURL(video.src);
        return '';
    } catch (e) {
        console.error("Thumb gen failed", e);
        return '';
    }
};

export const extractFrames = async (file: File, count: number): Promise<string[]> => {
    try {
        const video = await loadVideo(file);
        const duration = video.duration;
        if (!Number.isFinite(duration) || duration <= 0) {
            URL.revokeObjectURL(video.src);
            return [];
        }

        const frames: string[] = [];
        const interval = duration / Math.max(1, count);

        const vw = video.videoWidth || 1;
        const vh = video.videoHeight || 1;
        const aspect = vw / vh;
        const h = 80;
        const w = Math.max(1, h * aspect);

        const canvas = document.createElement('canvas');
        canvas.width = w;
        canvas.height = h;
        const ctx = canvas.getContext('2d');

        if (!ctx) {
            URL.revokeObjectURL(video.src);
            return [];
        }

        for (let i = 0; i < count; i++) {
            await seekTo(video, i * interval);
            ctx.drawImage(video, 0, 0, w, h);
            frames.push(canvas.toDataURL('image/jpeg', 0.5));
        }

        URL.revokeObjectURL(video.src);
        return frames;
    } catch (e) {
        console.warn('extractFrames failed:', e);
        return [];
    }
};

export interface Segment {
    id: string;
    label: string;
    startTime: number;
    endTime: number;
    color?: string;
}

// Create exactly N evenly-spaced segments for a duration (used when targetCount is set or duration invalid)
function createEvenSegments(duration: number, targetCount: number): Segment[] {
    if (!Number.isFinite(duration) || duration <= 0 || !targetCount || targetCount < 1) return [];
    const segmentDuration = duration / targetCount;
    const segments: Segment[] = [];
    for (let i = 0; i < targetCount; i++) {
        segments.push({
            id: `seg_${Date.now()}_${i}`,
            label: `Segment ${i + 1}`,
            startTime: i * segmentDuration,
            endTime: (i + 1) * segmentDuration,
            color: `hsl(${(i * 360 / targetCount)}, 70%, 50%)`
        });
    }
    return segments;
}

// Basic scene detection: look for significant pixel changes
// If targetCount is provided, always returns exactly that many segments (evenly split). Otherwise uses scene detection.
export const detectScenes = async (file: File, targetCount?: number, sensitivity = 25): Promise<Segment[]> => {
    const video = await loadVideo(file);
    const duration = video.duration;

    if (!Number.isFinite(duration) || duration <= 0) {
        URL.revokeObjectURL(video.src);
        return [];
    }

    // When user specified a count, always return exactly that many evenly-spaced segments so the control has visible effect
    if (targetCount && targetCount >= 1) {
        const evenSegments = createEvenSegments(duration, targetCount);
        URL.revokeObjectURL(video.src);
        return evenSegments;
    }

    const segments: Segment[] = [];
    const w = 64;
    const h = 36;
    const canvas = document.createElement('canvas');
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });

    if (!ctx) {
        URL.revokeObjectURL(video.src);
        return [];
    }

    let step = 2;
    if (duration > 180) step = 10;
    if (duration > 600) step = 30;

    const timestamps: number[] = [];
    for (let t = 0; t < duration; t += step) timestamps.push(t);
    timestamps.push(duration);

    let prevData: Uint8ClampedArray | null = null;
    let lastCutTime = 0;

    for (let i = 0; i < timestamps.length; i++) {
        await seekTo(video, timestamps[i]);
        ctx.drawImage(video, 0, 0, w, h);
        const data = ctx.getImageData(0, 0, w, h).data;

        if (prevData) {
            let diff = 0;
            for (let j = 0; j < data.length; j += 4) {
                diff += Math.abs(data[j] - prevData[j]) +
                    Math.abs(data[j + 1] - prevData[j + 1]) +
                    Math.abs(data[j + 2] - prevData[j + 2]);
            }
            const avgDiff = diff / (w * h * 3);

            if (avgDiff > sensitivity) {
                segments.push({
                    id: `seg_${Date.now()}_${i}`,
                    label: `Scene ${segments.length + 1}`,
                    startTime: lastCutTime,
                    endTime: timestamps[i],
                    color: '#' + Math.floor(Math.random() * 16777215).toString(16)
                });
                lastCutTime = timestamps[i];
            }
        }
        prevData = data;
    }

    if (lastCutTime < duration) {
        segments.push({
            id: `seg_${Date.now()}_end`,
            label: `Scene ${segments.length + 1}`,
            startTime: lastCutTime,
            endTime: duration,
            color: '#' + Math.floor(Math.random() * 16777215).toString(16)
        });
    }

    URL.revokeObjectURL(video.src);
    return segments;
};
