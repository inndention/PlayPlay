// ============================================
// Speaker Detection Panel
// UI for speaker detection and PTZ camera control
// ============================================

import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './SpeakerDetectionPanel.module.css';
import {
    Video, VideoOff, Camera, Settings, RefreshCw, Play, Pause,
    ZoomIn, ZoomOut, Maximize, Minimize, Eye, EyeOff, Radio,
    Users, User, Target, AlertCircle, CheckCircle, XCircle,
    ChevronUp, ChevronDown, ChevronLeft, ChevronRight
} from 'lucide-react';
import { ptzCameraController, PTZCameraConfig, PTZPosition } from '../lib/ptzCameraController';
import { useToast } from './Toast';

interface SpeakerDetection {
    person_id: number;
    bbox: [number, number, number, number];
    confidence: number;
    is_speaking: boolean;
    speaking_confidence: number;
}

interface DetectionResult {
    timestamp: number;
    detections: SpeakerDetection[];
    primary_speaker: SpeakerDetection | null;
}

interface SpeakerDetectionPanelProps {
    cameraId?: string;
    onSpeakerChange?: (speaker: SpeakerDetection | null) => void;
}

export function SpeakerDetectionPanel({ cameraId, onSpeakerChange }: SpeakerDetectionPanelProps) {
    const { showToast } = useToast();
    const [isDetecting, setIsDetecting] = useState(false);
    const [detectionResult, setDetectionResult] = useState<DetectionResult | null>(null);
    const [selectedCamera, setSelectedCamera] = useState<string | null>(cameraId || null);
    const [cameras, setCameras] = useState<PTZCameraConfig[]>([]);
    const [currentPosition, setCurrentPosition] = useState<PTZPosition | null>(null);
    const [autoFocus, setAutoFocus] = useState(true);
    const [sensitivity, setSensitivity] = useState(0.6);
    const [transitionSpeed, setTransitionSpeed] = useState(1000);
    const videoRef = useRef<HTMLVideoElement>(null);
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const detectionIntervalRef = useRef<NodeJS.Timeout | null>(null);

    // Load cameras
    useEffect(() => {
        const loadedCameras = ptzCameraController.getCameras();
        setCameras(loadedCameras);
        if (loadedCameras.length > 0 && !selectedCamera) {
            setSelectedCamera(loadedCameras[0].id);
        }
    }, []);

    // Subscribe to position updates
    useEffect(() => {
        if (!selectedCamera) return;

        const unsubscribe = ptzCameraController.onPositionUpdate(selectedCamera, (position) => {
            setCurrentPosition(position);
        });

        return unsubscribe;
    }, [selectedCamera]);

    // Start/stop detection
    const handleToggleDetection = useCallback(() => {
        if (isDetecting) {
            // Stop detection
            if (detectionIntervalRef.current) {
                clearInterval(detectionIntervalRef.current);
                detectionIntervalRef.current = null;
            }
            setIsDetecting(false);
        } else {
            // Start detection
            startDetection();
            setIsDetecting(true);
        }
    }, [isDetecting]);

    // Start detection loop
    const startDetection = useCallback(async () => {
        if (!videoRef.current || !canvasRef.current) {
            showToast('Video element not ready', 'warning', 2000);
            return;
        }

        const video = videoRef.current;
        const canvas = canvasRef.current;
        const ctx = canvas.getContext('2d');

        if (!ctx) return;

        // Set canvas size to match video
        canvas.width = video.videoWidth || 1920;
        canvas.height = video.videoHeight || 1080;

        const detectFrame = async () => {
            try {
                // Draw current frame to canvas
                ctx.drawImage(video, 0, 0, canvas.width, canvas.height);

                // Get frame data
                const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
                const frameData = imageData.data.buffer;

                // Call Python service via Electron IPC
                if (typeof window !== 'undefined' && (window as any).electronAPI?.speakerDetect) {
                    const result = await (window as any).electronAPI.speakerDetect(
                        Array.from(new Uint8Array(frameData)),
                        canvas.width,
                        canvas.height
                    );

                    if (result.success) {
                        setDetectionResult(result.result);

                        // Auto-focus on primary speaker
                        if (autoFocus && result.result.primary_speaker && selectedCamera) {
                            const speaker = result.result.primary_speaker;
                            await ptzCameraController.focusOnSpeaker(
                                selectedCamera,
                                {
                                    x: speaker.bbox[0],
                                    y: speaker.bbox[1],
                                    width: speaker.bbox[2],
                                    height: speaker.bbox[3],
                                },
                                { width: canvas.width, height: canvas.height }
                            );
                        }

                        // Notify parent
                        if (onSpeakerChange) {
                            onSpeakerChange(result.result.primary_speaker);
                        }
                    }
                }
            } catch (error) {
                console.error('[SpeakerDetection] Detection error:', error);
            }
        };

        // Run detection every 100ms (10 FPS)
        detectionIntervalRef.current = setInterval(detectFrame, 100);
        detectFrame(); // Initial detection
    }, [autoFocus, selectedCamera, onSpeakerChange, showToast]);

    // Manual camera control
    const handleCameraMove = useCallback(async (direction: 'pan' | 'tilt' | 'zoom', delta: number) => {
        if (!selectedCamera) return;

        const current = currentPosition || { pan: 0, tilt: 0, zoom: 0.5 };
        const newPosition: Partial<PTZPosition> = { ...current };

        switch (direction) {
            case 'pan':
                newPosition.pan = Math.max(-1, Math.min(1, current.pan + delta));
                break;
            case 'tilt':
                newPosition.tilt = Math.max(-1, Math.min(1, current.tilt + delta));
                break;
            case 'zoom':
                newPosition.zoom = Math.max(0, Math.min(1, current.zoom + delta));
                break;
        }

        const result = await ptzCameraController.moveTo(selectedCamera, newPosition, transitionSpeed);
        if (result.success) {
            showToast('Camera moved', 'success', 1000);
        } else {
            showToast(`Camera move failed: ${result.error}`, 'error', 2000);
        }
    }, [selectedCamera, currentPosition, transitionSpeed, showToast]);

    // Recall preset
    const handleRecallPreset = useCallback(async (presetId: string) => {
        if (!selectedCamera) return;

        const result = await ptzCameraController.recallPreset(selectedCamera, presetId);
        if (result.success) {
            showToast('Preset recalled', 'success', 1000);
        } else {
            showToast(`Preset recall failed: ${result.error}`, 'error', 2000);
        }
    }, [selectedCamera, showToast]);

    // Draw detection overlays
    useEffect(() => {
        if (!canvasRef.current || !detectionResult) return;

        const canvas = canvasRef.current;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;

        // Clear previous overlays
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        // Draw detections
        detectionResult.detections.forEach(detection => {
            const [x, y, w, h] = detection.bbox;
            const isPrimary = detectionResult.primary_speaker?.person_id === detection.person_id;

            // Draw bounding box
            ctx.strokeStyle = isPrimary ? '#00ff00' : '#ffff00';
            ctx.lineWidth = 3;
            ctx.strokeRect(x, y, w, h);

            // Draw label
            ctx.fillStyle = isPrimary ? '#00ff00' : '#ffff00';
            ctx.font = '16px Arial';
            ctx.fillText(
                `Person ${detection.person_id} ${detection.is_speaking ? '(Speaking)' : ''}`,
                x,
                y - 5
            );

            // Draw speaking indicator
            if (detection.is_speaking) {
                ctx.fillStyle = 'rgba(0, 255, 0, 0.3)';
                ctx.fillRect(x, y, w, h);
            }
        });
    }, [detectionResult]);

    const selectedCameraConfig = cameras.find(c => c.id === selectedCamera);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <Camera size={20} />
                    <h2>Speaker Detection & PTZ Control</h2>
                </div>
                <div className={styles.headerRight}>
                    <button
                        className={styles.toggleBtn}
                        onClick={handleToggleDetection}
                        disabled={!selectedCamera}
                    >
                        {isDetecting ? <Pause size={16} /> : <Play size={16} />}
                        {isDetecting ? 'Stop' : 'Start'} Detection
                    </button>
                </div>
            </div>

            <div className={styles.content}>
                {/* Video Feed */}
                <div className={styles.videoSection}>
                    <div className={styles.videoContainer}>
                        <video
                            ref={videoRef}
                            className={styles.video}
                            autoPlay
                            muted
                            playsInline
                        />
                        <canvas
                            ref={canvasRef}
                            className={styles.overlay}
                        />
                    </div>

                    {/* Detection Info */}
                    {detectionResult && (
                        <div className={styles.detectionInfo}>
                            <div className={styles.infoRow}>
                                <span>Detections: {detectionResult.detections.length}</span>
                                {detectionResult.primary_speaker && (
                                    <span className={styles.primarySpeaker}>
                                        <User size={14} />
                                        Primary Speaker: Person {detectionResult.primary_speaker.person_id}
                                    </span>
                                )}
                            </div>
                        </div>
                    )}
                </div>

                {/* Controls */}
                <div className={styles.controls}>
                    <div className={styles.controlGroup}>
                        <label>Camera</label>
                        <select
                            value={selectedCamera || ''}
                            onChange={(e) => setSelectedCamera(e.target.value)}
                        >
                            <option value="">Select camera...</option>
                            {cameras.map(camera => (
                                <option key={camera.id} value={camera.id}>
                                    {camera.name} ({camera.protocol})
                                </option>
                            ))}
                        </select>
                    </div>

                    {selectedCameraConfig && (
                        <>
                            <div className={styles.controlGroup}>
                                <label>
                                    <input
                                        type="checkbox"
                                        checked={autoFocus}
                                        onChange={(e) => setAutoFocus(e.target.checked)}
                                    />
                                    Auto-focus on speaker
                                </label>
                            </div>

                            <div className={styles.controlGroup}>
                                <label>Sensitivity: {sensitivity.toFixed(2)}</label>
                                <input
                                    type="range"
                                    min="0"
                                    max="1"
                                    step="0.1"
                                    value={sensitivity}
                                    onChange={(e) => setSensitivity(parseFloat(e.target.value))}
                                />
                            </div>

                            <div className={styles.controlGroup}>
                                <label>Transition Speed: {transitionSpeed}ms</label>
                                <input
                                    type="range"
                                    min="100"
                                    max="5000"
                                    step="100"
                                    value={transitionSpeed}
                                    onChange={(e) => setTransitionSpeed(parseInt(e.target.value))}
                                />
                            </div>

                            {/* Manual Controls */}
                            <div className={styles.manualControls}>
                                <h3>Manual Control</h3>
                                <div className={styles.controlGrid}>
                                    <button onClick={() => handleCameraMove('tilt', -0.1)}>
                                        <ChevronUp size={20} />
                                    </button>
                                    <div></div>
                                    <button onClick={() => handleCameraMove('zoom', 0.1)}>
                                        <ZoomIn size={20} />
                                    </button>
                                    <button onClick={() => handleCameraMove('pan', -0.1)}>
                                        <ChevronLeft size={20} />
                                    </button>
                                    <button onClick={() => handleCameraMove('pan', 0.1)}>
                                        <ChevronRight size={20} />
                                    </button>
                                    <button onClick={() => handleCameraMove('zoom', -0.1)}>
                                        <ZoomOut size={20} />
                                    </button>
                                    <div></div>
                                    <button onClick={() => handleCameraMove('tilt', 0.1)}>
                                        <ChevronDown size={20} />
                                    </button>
                                </div>
                            </div>

                            {/* Presets */}
                            {selectedCameraConfig.presets.length > 0 && (
                                <div className={styles.presets}>
                                    <h3>Presets</h3>
                                    <div className={styles.presetList}>
                                        {selectedCameraConfig.presets.map(preset => (
                                            <button
                                                key={preset.id}
                                                className={styles.presetBtn}
                                                onClick={() => handleRecallPreset(preset.id)}
                                            >
                                                {preset.name}
                                            </button>
                                        ))}
                                    </div>
                                </div>
                            )}
                        </>
                    )}
                </div>
            </div>
        </div>
    );
}

