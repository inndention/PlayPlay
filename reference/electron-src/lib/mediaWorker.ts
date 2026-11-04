/* lib/mediaWorker.ts */

// Basic scene detection logic ported to Web Worker
// Since we can't access Video elements easily in workers, 
// we still rely on the main thread for frame extraction,
// but the worker handles the pixel-diffing and segment calculation.

export type WorkerMessage = 
    | {
        type: 'detect_scenes';
        frames: { time: number; data: Uint8ClampedArray }[];
        sensitivity: number;
        duration: number;
      }
    | {
        type: 'generate_preview';
        bitmap: ImageBitmap;
        time: number;
        clientX: number;
      };

let previewCanvas: OffscreenCanvas | null = null;
let previewCtx: OffscreenCanvasRenderingContext2D | null = null;

self.onmessage = async (e: MessageEvent<WorkerMessage>) => {
    const data = e.data;

    if (data.type === 'detect_scenes') {
        const { frames, sensitivity, duration } = data;
        const segments: any[] = [];
        let lastCutTime = 0;
        const w = 64;
        const h = 36;

        for (let i = 1; i < frames.length; i++) {
            const current = frames[i].data;
            const prev = frames[i-1].data;

            let diff = 0;
            for (let j = 0; j < current.length; j += 4) {
                diff += Math.abs(current[j] - prev[j]) +
                        Math.abs(current[j + 1] - prev[j + 1]) +
                        Math.abs(current[j + 2] - prev[j + 2]);
            }
            const avgDiff = diff / (w * h * 3);

            if (avgDiff > sensitivity) {
                segments.push({
                    id: `seg_${Date.now()}_${i}`,
                    label: `Scene ${segments.length + 1}`,
                    startTime: lastCutTime,
                    endTime: frames[i].time,
                    color: `hsl(${Math.random() * 360}, 70%, 50%)`
                });
                lastCutTime = frames[i].time;
            }
        }

        if (lastCutTime < duration) {
            segments.push({
                id: `seg_${Date.now()}_end`,
                label: `Scene ${segments.length + 1}`,
                startTime: lastCutTime,
                endTime: duration,
                color: `hsl(${Math.random() * 360}, 70%, 50%)`
            });
        }

        self.postMessage({ type: 'scenes_detected', segments });
    } else if (data.type === 'generate_preview') {
        const { bitmap, time, clientX } = data;
        
        try {
            if (!previewCanvas) {
                previewCanvas = new OffscreenCanvas(160, 90);
                previewCtx = previewCanvas.getContext('2d', { alpha: false });
            }

            if (previewCtx && previewCanvas) {
                previewCtx.drawImage(bitmap, 0, 0, previewCanvas.width, previewCanvas.height);
                const blob = await previewCanvas.convertToBlob({ type: 'image/jpeg', quality: 0.7 });
                
                self.postMessage({
                    type: 'preview_generated',
                    blob,
                    time,
                    clientX
                });
            }
        } catch (err) {
            console.error('Worker error generating preview:', err);
        } finally {
            // Must close bitmap to free memory
            bitmap.close();
        }
    }
};
