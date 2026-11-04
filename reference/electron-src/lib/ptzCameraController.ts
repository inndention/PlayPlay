// ============================================
// PTZ Camera Controller
// Control PTZ cameras with smooth transitions
// ============================================

export type PTZProtocol = 'VISCA' | 'ONVIF' | 'Pelco-D' | 'Pelco-P';
export type PTZCommand = 'pan' | 'tilt' | 'zoom' | 'focus' | 'preset' | 'home';

export interface PTZCameraConfig {
    id: string;
    name: string;
    protocol: PTZProtocol;
    address: string;
    port: number;
    username?: string;
    password?: string;
    connectionType: 'serial' | 'network' | 'usb';
    serialPort?: string;
    baudRate?: number;
    panSpeed: number; // 1-24 for VISCA, 0-100 for others
    tiltSpeed: number;
    zoomSpeed: number;
    transitionDuration: number; // milliseconds
    presets: PTZPreset[];
}

export interface PTZPreset {
    id: string;
    name: string;
    pan: number; // -1.0 to 1.0 (normalized)
    tilt: number; // -1.0 to 1.0 (normalized)
    zoom: number; // 0.0 to 1.0 (normalized)
    focus?: number; // 0.0 to 1.0 (normalized)
}

export interface PTZPosition {
    pan: number;
    tilt: number;
    zoom: number;
    focus?: number;
}

export interface PTZCommandResult {
    success: boolean;
    error?: string;
    position?: PTZPosition;
}

class PTZCameraController {
    private cameras: Map<string, PTZCameraConfig> = new Map();
    private currentPositions: Map<string, PTZPosition> = new Map();
    private transitionTimers: Map<string, NodeJS.Timeout> = new Map();
    private onPositionUpdateCallbacks: Map<string, Set<(position: PTZPosition) => void>> = new Map();

    /**
     * Add a camera
     */
    public addCamera(config: PTZCameraConfig): void {
        this.cameras.set(config.id, config);
        this.currentPositions.set(config.id, { pan: 0, tilt: 0, zoom: 0.5 });
        this.onPositionUpdateCallbacks.set(config.id, new Set());
        
        // Initialize camera via Electron IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI?.ptzCameraInit) {
            (window as any).electronAPI.ptzCameraInit(config).catch((error: Error) => {
                console.error(`[PTZ] Failed to initialize camera ${config.name}:`, error);
            });
        }
    }

    /**
     * Remove a camera
     */
    public removeCamera(cameraId: string): void {
        this.cameras.delete(cameraId);
        this.currentPositions.delete(cameraId);
        const timer = this.transitionTimers.get(cameraId);
        if (timer) {
            clearTimeout(timer);
            this.transitionTimers.delete(cameraId);
        }
        this.onPositionUpdateCallbacks.delete(cameraId);
    }

    /**
     * Get camera config
     */
    public getCamera(cameraId: string): PTZCameraConfig | undefined {
        return this.cameras.get(cameraId);
    }

    /**
     * Get all cameras
     */
    public getCameras(): PTZCameraConfig[] {
        return Array.from(this.cameras.values());
    }

    /**
     * Move camera smoothly to position
     */
    public async moveTo(
        cameraId: string,
        targetPosition: Partial<PTZPosition>,
        duration?: number
    ): Promise<PTZCommandResult> {
        const camera = this.cameras.get(cameraId);
        if (!camera) {
            return { success: false, error: `Camera ${cameraId} not found` };
        }

        const currentPos = this.currentPositions.get(cameraId) || { pan: 0, tilt: 0, zoom: 0.5 };
        const transitionDuration = duration || camera.transitionDuration;

        // Cancel any existing transition
        const existingTimer = this.transitionTimers.get(cameraId);
        if (existingTimer) {
            clearTimeout(existingTimer);
        }

        // Calculate steps for smooth transition
        const steps = Math.max(10, Math.floor(transitionDuration / 50)); // 50ms per step
        const stepDuration = transitionDuration / steps;

        const startPos = { ...currentPos };
        const endPos: PTZPosition = {
            pan: targetPosition.pan ?? currentPos.pan,
            tilt: targetPosition.tilt ?? currentPos.tilt,
            zoom: targetPosition.zoom ?? currentPos.zoom,
            focus: targetPosition.focus ?? currentPos.focus,
        };

        // Execute transition via Electron IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI?.ptzCameraMove) {
            try {
                const result = await (window as any).electronAPI.ptzCameraMove(
                    cameraId,
                    endPos,
                    transitionDuration
                );

                if (result.success) {
                    // Update position immediately (smooth transition handled by hardware/service)
                    this.currentPositions.set(cameraId, endPos);
                    this.notifyPositionUpdate(cameraId, endPos);
                    return { success: true, position: endPos };
                } else {
                    return { success: false, error: result.error };
                }
            } catch (error) {
                return { success: false, error: error instanceof Error ? error.message : 'Unknown error' };
            }
        }

        // Fallback: simulate transition
        return this.simulateTransition(cameraId, startPos, endPos, steps, stepDuration);
    }

    /**
     * Simulate smooth transition (fallback)
     */
    private async simulateTransition(
        cameraId: string,
        startPos: PTZPosition,
        endPos: PTZPosition,
        steps: number,
        stepDuration: number
    ): Promise<PTZCommandResult> {
        return new Promise((resolve) => {
            let currentStep = 0;

            const step = () => {
                currentStep++;
                const progress = currentStep / steps;
                const easedProgress = this.easeInOutCubic(progress);

                const currentPos: PTZPosition = {
                    pan: startPos.pan + (endPos.pan - startPos.pan) * easedProgress,
                    tilt: startPos.tilt + (endPos.tilt - startPos.tilt) * easedProgress,
                    zoom: startPos.zoom + (endPos.zoom - startPos.zoom) * easedProgress,
                    focus: startPos.focus !== undefined && endPos.focus !== undefined
                        ? startPos.focus + (endPos.focus - startPos.focus) * easedProgress
                        : undefined,
                };

                this.currentPositions.set(cameraId, currentPos);
                this.notifyPositionUpdate(cameraId, currentPos);

                if (currentStep < steps) {
                    const timer = setTimeout(step, stepDuration);
                    this.transitionTimers.set(cameraId, timer);
                } else {
                    this.transitionTimers.delete(cameraId);
                    resolve({ success: true, position: endPos });
                }
            };

            step();
        });
    }

    /**
     * Easing function for smooth transitions
     */
    private easeInOutCubic(t: number): number {
        return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
    }

    /**
     * Recall preset
     */
    public async recallPreset(cameraId: string, presetId: string): Promise<PTZCommandResult> {
        const camera = this.cameras.get(cameraId);
        if (!camera) {
            return { success: false, error: `Camera ${cameraId} not found` };
        }

        const preset = camera.presets.find(p => p.id === presetId);
        if (!preset) {
            return { success: false, error: `Preset ${presetId} not found` };
        }

        return this.moveTo(cameraId, {
            pan: preset.pan,
            tilt: preset.tilt,
            zoom: preset.zoom,
            focus: preset.focus,
        });
    }

    /**
     * Save current position as preset
     */
    public savePreset(cameraId: string, presetName: string): PTZPreset | null {
        const camera = this.cameras.get(cameraId);
        const position = this.currentPositions.get(cameraId);
        
        if (!camera || !position) {
            return null;
        }

        const preset: PTZPreset = {
            id: `preset_${Date.now()}`,
            name: presetName,
            pan: position.pan,
            tilt: position.tilt,
            zoom: position.zoom,
            focus: position.focus,
        };

        camera.presets.push(preset);
        this.cameras.set(cameraId, camera);

        return preset;
    }

    /**
     * Get current position
     */
    public getPosition(cameraId: string): PTZPosition | undefined {
        return this.currentPositions.get(cameraId);
    }

    /**
     * Subscribe to position updates
     */
    public onPositionUpdate(
        cameraId: string,
        callback: (position: PTZPosition) => void
    ): () => void {
        const callbacks = this.onPositionUpdateCallbacks.get(cameraId);
        if (callbacks) {
            callbacks.add(callback);
            return () => callbacks.delete(callback);
        }
        return () => {};
    }

    /**
     * Notify position update subscribers
     */
    private notifyPositionUpdate(cameraId: string, position: PTZPosition): void {
        const callbacks = this.onPositionUpdateCallbacks.get(cameraId);
        if (callbacks) {
            callbacks.forEach(callback => {
                try {
                    callback(position);
                } catch (error) {
                    console.error('[PTZ] Position update callback error:', error);
                }
            });
        }
    }

    /**
     * Focus on speaker (called by speaker detection)
     */
    public async focusOnSpeaker(
        cameraId: string,
        speakerBbox: { x: number; y: number; width: number; height: number },
        frameSize: { width: number; height: number }
    ): Promise<PTZCommandResult> {
        // Calculate center of speaker bbox
        const centerX = speakerBbox.x + speakerBbox.width / 2;
        const centerY = speakerBbox.y + speakerBbox.height / 2;

        // Normalize to -1.0 to 1.0 range
        const normalizedPan = (centerX / frameSize.width) * 2 - 1;
        const normalizedTilt = (centerY / frameSize.height) * 2 - 1;

        // Calculate zoom based on bbox size (larger = closer, zoom out; smaller = farther, zoom in)
        const bboxArea = speakerBbox.width * speakerBbox.height;
        const frameArea = frameSize.width * frameSize.height;
        const coverage = bboxArea / frameArea;
        
        // Target coverage: 30% of frame
        const targetCoverage = 0.3;
        const zoomAdjustment = Math.log(coverage / targetCoverage) / Math.log(2); // Logarithmic zoom
        const currentPos = this.currentPositions.get(cameraId) || { pan: 0, tilt: 0, zoom: 0.5 };
        const targetZoom = Math.max(0, Math.min(1, currentPos.zoom + zoomAdjustment * 0.1));

        return this.moveTo(cameraId, {
            pan: normalizedPan,
            tilt: normalizedTilt,
            zoom: targetZoom,
        });
    }
}

// Singleton instance
let controllerInstance: PTZCameraController | null = null;

export function getPTZCameraController(): PTZCameraController {
    if (!controllerInstance) {
        controllerInstance = new PTZCameraController();
    }
    return controllerInstance;
}

export const ptzCameraController = getPTZCameraController();

