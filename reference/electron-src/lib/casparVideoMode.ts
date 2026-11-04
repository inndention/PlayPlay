/**
 * CasparCG channel `<video-mode>` for PLAY PLAY–managed installs.
 * Default 50 Hz progressive (720p5000) for broad GPU/build compatibility; 30/60 optional.
 */
export const CASPAR_DEFAULT_VIDEO_MODE = '720p5000';

export const CASPAR_VIDEO_MODE_OPTIONS = [
    { value: '720p5000', label: '50' },
    { value: '720p3000', label: '30' },
    { value: '720p6000', label: '60' },
] as const;

export type CasparVideoModeToken = (typeof CASPAR_VIDEO_MODE_OPTIONS)[number]['value'];

const ALLOWED = new Set<string>(CASPAR_VIDEO_MODE_OPTIONS.map((o) => o.value));

export function getCasparVideoModePreference(): CasparVideoModeToken {
    try {
        const v = localStorage.getItem('casparVideoMode');
        if (v && ALLOWED.has(v as CasparVideoModeToken)) return v as CasparVideoModeToken;
    } catch {
        /* */
    }
    return CASPAR_DEFAULT_VIDEO_MODE;
}

export function setCasparVideoModePreference(mode: string): CasparVideoModeToken | null {
    if (!ALLOWED.has(mode)) return null;
    const m = mode as CasparVideoModeToken;
    try {
        localStorage.setItem('casparVideoMode', m);
    } catch {
        /* */
    }
    return m;
}
