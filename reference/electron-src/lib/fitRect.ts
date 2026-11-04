/**
 * Aspect-ratio-preserving layout helper.
 *
 * All coordinates are normalized to the full canvas (0–1 range).
 * Slot defines where on the canvas this source lives.
 *
 * Aspect safety rule:
 *   Never output media using raw slot width/height unless fitMode === "stretch".
 *   All video/image layers must pass through fitRect() using real source
 *   dimensions from ffprobe / CINF / media metadata.
 */

export type Rect = {
    x: number;
    y: number;
    w: number;
    h: number;
};

export type FitMode = 'contain' | 'cover' | 'stretch';

/**
 * Return the normalized rectangle (relative to the full canvas) that places
 * a srcW×srcH source inside a canvas slot according to the given fit mode.
 *
 * - contain : preserve full picture, black/transparent padding on the short sides
 * - cover   : fill the slot completely, crop overflow on the long sides
 * - stretch : return the slot unchanged (legacy, aspect ratio is NOT preserved)
 *
 * For CasparCG, pass the returned {x, y, w, h} directly to MIXER FILL.
 * Negative x/y or w/h > slot edge are valid — Caspar clips at the frame boundary.
 */
export function fitRect(
    srcW: number,
    srcH: number,
    canvasW: number,
    canvasH: number,
    slot: Rect,
    mode: FitMode = 'contain',
): Rect {
    if (mode === 'stretch') return slot;

    const slotPx = {
        x: slot.x * canvasW,
        y: slot.y * canvasH,
        w: slot.w * canvasW,
        h: slot.h * canvasH,
    };

    const scale =
        mode === 'cover'
            ? Math.max(slotPx.w / srcW, slotPx.h / srcH)
            : Math.min(slotPx.w / srcW, slotPx.h / srcH);

    const fittedW = srcW * scale;
    const fittedH = srcH * scale;

    const fittedX = slotPx.x + (slotPx.w - fittedW) / 2;
    const fittedY = slotPx.y + (slotPx.h - fittedH) / 2;

    return {
        x: fittedX / canvasW,
        y: fittedY / canvasH,
        w: fittedW / canvasW,
        h: fittedH / canvasH,
    };
}

/**
 * Format a fitRect result as a CasparCG MIXER FILL argument string.
 * e.g. "0.67090 0.00000 0.15820 0.50000"
 */
export function toMixerFill(r: Rect): string {
    return `${r.x.toFixed(5)} ${r.y.toFixed(5)} ${r.w.toFixed(5)} ${r.h.toFixed(5)}`;
}

/**
 * Return true when the rect covers the full canvas (no transform needed).
 */
export function isFullFrame(r: Rect, tolerance = 0.001): boolean {
    return (
        Math.abs(r.x) < tolerance &&
        Math.abs(r.y) < tolerance &&
        Math.abs(r.w - 1) < tolerance &&
        Math.abs(r.h - 1) < tolerance
    );
}
