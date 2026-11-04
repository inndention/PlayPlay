/**
 * Hardware Acceleration & Video Enhancement Settings
 * For the built-in HTML5 player
 */

// Hardware acceleration capabilities detection
export interface HardwareCapabilities {
    nvdec: boolean;     // NVIDIA hardware decoding
    d3d11va: boolean;   // DirectX 11 Video Acceleration (Windows)
    vaapi: boolean;     // Video Acceleration API (Linux)
    videotoolbox: boolean; // VideoToolbox (macOS)
    webgl2: boolean;    // WebGL 2.0 for GPU effects
    webgpu: boolean;    // WebGPU (modern browsers)
    hdrSupport: boolean; // HDR display support
}

// Video enhancement settings
export interface VideoEnhancementSettings {
    upscaling: {
        enabled: boolean;
        algorithm: 'none' | 'lanczos' | 'bilinear' | 'bicubic' | 'sharp' | 'ai_super_resolution';
        strength: number; // 0-100
    };
    frameInterpolation: {
        enabled: boolean;
        mode: 'none' | 'frame_blending' | 'motion_blur' | 'optical_flow';
        targetFps: 30 | 60 | 120;
    };
    deblocking: {
        enabled: boolean;
        strength: 'light' | 'medium' | 'heavy';
    };
    colorCorrection: {
        enabled: boolean;
        brightness: number; // -100 to 100
        contrast: number;   // -100 to 100
        saturation: number; // -100 to 100
        hue: number;        // -180 to 180
    };
    sharpening: {
        enabled: boolean;
        amount: number; // 0-100
    };
}

// Default enhancement settings
export const DEFAULT_VIDEO_ENHANCEMENTS: VideoEnhancementSettings = {
    upscaling: {
        enabled: false,
        algorithm: 'lanczos',
        strength: 50,
    },
    frameInterpolation: {
        enabled: false,
        mode: 'none',
        targetFps: 60,
    },
    deblocking: {
        enabled: false,
        strength: 'light',
    },
    colorCorrection: {
        enabled: false,
        brightness: 0,
        contrast: 0,
        saturation: 0,
        hue: 0,
    },
    sharpening: {
        enabled: false,
        amount: 25,
    },
};

/**
 * Detect hardware acceleration capabilities
 */
export async function detectHardwareCapabilities(): Promise<HardwareCapabilities> {
    const capabilities: HardwareCapabilities = {
        nvdec: false,
        d3d11va: false,
        vaapi: false,
        videotoolbox: false,
        webgl2: false,
        webgpu: false,
        hdrSupport: false,
    };

    // Check WebGL2 support
    try {
        const canvas = document.createElement('canvas');
        const gl = canvas.getContext('webgl2');
        capabilities.webgl2 = !!gl;
        
        // Check for specific extensions that indicate good GPU support
        if (gl) {
            const debugInfo = gl.getExtension('WEBGL_debug_renderer_info');
            if (debugInfo) {
                const renderer = gl.getParameter(debugInfo.UNMASKED_RENDERER_WEBGL);
                // Check for NVIDIA
                if (/nvidia/i.test(renderer)) {
                    capabilities.nvdec = true;
                }
            }
        }
    } catch {
        capabilities.webgl2 = false;
    }

    // Check WebGPU support
    try {
        if ('gpu' in navigator) {
            const adapter = await (navigator as any).gpu?.requestAdapter();
            capabilities.webgpu = !!adapter;
        }
    } catch {
        capabilities.webgpu = false;
    }

    // Check HDR support
    try {
        capabilities.hdrSupport = window.matchMedia('(dynamic-range: high)').matches;
    } catch {
        capabilities.hdrSupport = false;
    }

    // Platform-specific detection via Electron API
    if (typeof window !== 'undefined' && (window as any).electronAPI?.getSystemInfo) {
        try {
            const sysInfo = await (window as any).electronAPI.getSystemInfo();
            if (sysInfo) {
                const platform = sysInfo.platform;
                
                if (platform === 'win32') {
                    capabilities.d3d11va = true; // Windows has D3D11VA
                } else if (platform === 'darwin') {
                    capabilities.videotoolbox = true; // macOS has VideoToolbox
                } else if (platform === 'linux') {
                    // Check for VAAPI - usually available on Linux with Intel/AMD
                    capabilities.vaapi = true;
                }
            }
        } catch {
            // Electron API not available
        }
    }

    return capabilities;
}

/**
 * Get optimal hardware decoding setting based on capabilities
 */
export function getOptimalHWDecoder(capabilities: HardwareCapabilities): string {
    if (capabilities.nvdec) return 'nvdec';
    if (capabilities.d3d11va) return 'd3d11va';
    if (capabilities.videotoolbox) return 'videotoolbox';
    if (capabilities.vaapi) return 'vaapi';
    return 'auto';
}

/**
 * Apply CSS-based video enhancements (for built-in player)
 */
export function getVideoEnhancementCSS(settings: VideoEnhancementSettings): string {
    const filters: string[] = [];

    if (settings.colorCorrection.enabled) {
        const { brightness, contrast, saturation } = settings.colorCorrection;
        if (brightness !== 0) {
            filters.push(`brightness(${1 + brightness / 100})`);
        }
        if (contrast !== 0) {
            filters.push(`contrast(${1 + contrast / 100})`);
        }
        if (saturation !== 0) {
            filters.push(`saturate(${1 + saturation / 100})`);
        }
        if (settings.colorCorrection.hue !== 0) {
            filters.push(`hue-rotate(${settings.colorCorrection.hue}deg)`);
        }
    }

    if (settings.sharpening.enabled && settings.sharpening.amount > 0) {
        // CSS doesn't have a native sharpen filter, use a subtle contrast boost
        const sharpAmount = 1 + (settings.sharpening.amount / 200);
        filters.push(`contrast(${sharpAmount})`);
    }

    return filters.length > 0 ? filters.join(' ') : 'none';
}

/**
 * Get image rendering CSS for upscaling
 */
export function getUpscalingCSS(algorithm: VideoEnhancementSettings['upscaling']['algorithm']): string {
    switch (algorithm) {
        case 'sharp':
        case 'lanczos':
            return 'crisp-edges';
        case 'bilinear':
        case 'bicubic':
            return 'auto';
        case 'ai_super_resolution':
            return 'high-quality';
        default:
            return 'auto';
    }
}

// Video enhancement manager singleton
class VideoEnhancementManager {
    private settings: VideoEnhancementSettings = DEFAULT_VIDEO_ENHANCEMENTS;
    private capabilities: HardwareCapabilities | null = null;
    private listeners: Set<(settings: VideoEnhancementSettings) => void> = new Set();

    async initialize(): Promise<void> {
        this.capabilities = await detectHardwareCapabilities();
        console.log('[VideoEnhancements] Hardware capabilities:', this.capabilities);
        
        // Load saved settings
        try {
            const saved = localStorage.getItem('videoEnhancementSettings');
            if (saved) {
                this.settings = { ...DEFAULT_VIDEO_ENHANCEMENTS, ...JSON.parse(saved) };
            }
        } catch {
            // Use defaults
        }
    }

    getSettings(): VideoEnhancementSettings {
        return this.settings;
    }

    getCapabilities(): HardwareCapabilities | null {
        return this.capabilities;
    }

    updateSettings(updates: Partial<VideoEnhancementSettings>): void {
        this.settings = { ...this.settings, ...updates };
        
        // Save to localStorage
        try {
            localStorage.setItem('videoEnhancementSettings', JSON.stringify(this.settings));
        } catch {
            // Ignore storage errors
        }

        // Notify listeners
        this.listeners.forEach(listener => {
            try {
                listener(this.settings);
            } catch (e) {
                console.error('[VideoEnhancements] Listener error:', e);
            }
        });
    }

    onSettingsChange(listener: (settings: VideoEnhancementSettings) => void): () => void {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    }

    /**
     * Get CSS filters and styles for video element
     */
    getVideoStyles(): { filter: string; imageRendering: string } {
        return {
            filter: getVideoEnhancementCSS(this.settings),
            imageRendering: getUpscalingCSS(this.settings.upscaling.algorithm),
        };
    }
}

// Singleton instance
export const videoEnhancementManager = new VideoEnhancementManager();

// Initialize on import
if (typeof window !== 'undefined') {
    videoEnhancementManager.initialize().catch(console.error);
}
