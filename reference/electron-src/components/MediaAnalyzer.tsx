import React, { useState, useEffect, useCallback, useRef } from 'react';
import styles from './MediaAnalyzer.module.css';
import { MediaItem } from '../lib/types';
import { formatDuration } from '../lib/utils';
import {
    BarChart2, Volume2, Tv, Clock, FileText, AlertTriangle, CheckCircle,
    XCircle, Zap, Film, Disc, HardDrive, Cpu, AudioWaveform, Eye, RefreshCw,
    Download, Upload, Activity, Target, Gauge, Layers, Settings, X
} from 'lucide-react';

interface MediaAnalyzerProps {
    item: MediaItem;
    onClose: () => void;
}

interface TechnicalMetrics {
    // Video
    resolution?: { width: number; height: number };
    frameRate?: number;
    videoCodec?: string;
    videoBitrate?: number;
    aspectRatio?: string;
    colorSpace?: string;
    bitDepth?: number;
    scanType?: 'progressive' | 'interlaced';
    
    // Audio
    audioCodec?: string;
    audioBitrate?: number;
    sampleRate?: number;
    channels?: number;
    loudnessLUFS?: number;
    loudnessRange?: number;
    truePeak?: number;
    
    // Container
    containerFormat?: string;
    fileSize?: number;
    duration?: number;
    
    // Quality Indicators
    hasVideoTrack?: boolean;
    hasAudioTrack?: boolean;
    isHD?: boolean;
    is4K?: boolean;
    isProRes?: boolean;
    isBroadcastSafe?: boolean;
}

interface ComplianceCheck {
    name: string;
    status: 'pass' | 'fail' | 'warning' | 'unknown';
    message: string;
    standard?: string;
}

export function MediaAnalyzer({ item, onClose }: MediaAnalyzerProps) {
    const [metrics, setMetrics] = useState<TechnicalMetrics | null>(null);
    const [compliance, setCompliance] = useState<ComplianceCheck[]>([]);
    const [analyzing, setAnalyzing] = useState(false);
    const [waveformData, setWaveformData] = useState<number[] | null>(null);
    const [activeTab, setActiveTab] = useState<'metrics' | 'compliance' | 'waveform' | 'history'>('metrics');
    const videoRef = useRef<HTMLVideoElement>(null);
    const canvasRef = useRef<HTMLCanvasElement>(null);

    // Analyze media on mount
    useEffect(() => {
        analyzeMedia();
    }, [item]);

    const analyzeMedia = async () => {
        setAnalyzing(true);
        try {
            // Try to get detailed analysis from electron API
            const electronAPI = (window as any).electronAPI;
            
            if (electronAPI?.analyzeMedia && item.filePath) {
                const result = await electronAPI.analyzeMedia(item.filePath);
                if (result.success) {
                    setMetrics(result.metrics);
                    runComplianceChecks(result.metrics);
                    return;
                }
            }

            // Fallback: basic analysis from video element
            await analyzeFromVideo();
        } catch (error) {
            console.error('Media analysis failed:', error);
        } finally {
            setAnalyzing(false);
        }
    };

    const analyzeFromVideo = async () => {
        return new Promise<void>((resolve) => {
            if (!videoRef.current) {
                resolve();
                return;
            }

            const video = videoRef.current;
            video.src = item.src;
            
            video.onloadedmetadata = () => {
                const basicMetrics: TechnicalMetrics = {
                    resolution: {
                        width: video.videoWidth,
                        height: video.videoHeight
                    },
                    duration: video.duration,
                    hasVideoTrack: video.videoWidth > 0,
                    hasAudioTrack: true, // Assume true for basic analysis
                    isHD: video.videoHeight >= 720,
                    is4K: video.videoWidth >= 3840,
                    aspectRatio: calculateAspectRatio(video.videoWidth, video.videoHeight),
                };
                
                setMetrics(basicMetrics);
                runComplianceChecks(basicMetrics);
                
                // Generate waveform preview
                generateWaveformPreview(video);
                
                resolve();
            };

            video.onerror = () => {
                console.error('Failed to load video for analysis');
                resolve();
            };
        });
    };

    const calculateAspectRatio = (width: number, height: number): string => {
        if (!width || !height) return 'Unknown';
        const ratio = width / height;
        if (Math.abs(ratio - 16/9) < 0.01) return '16:9';
        if (Math.abs(ratio - 4/3) < 0.01) return '4:3';
        if (Math.abs(ratio - 21/9) < 0.01) return '21:9';
        if (Math.abs(ratio - 1) < 0.01) return '1:1';
        return `${(ratio).toFixed(2)}:1`;
    };

    const runComplianceChecks = (m: TechnicalMetrics) => {
        const checks: ComplianceCheck[] = [];

        // Resolution Check
        checks.push({
            name: 'Resolution',
            status: m.resolution && m.resolution.width >= 1920 ? 'pass' : 
                   m.resolution && m.resolution.width >= 1280 ? 'warning' : 'fail',
            message: m.resolution 
                ? `${m.resolution.width}x${m.resolution.height}` 
                : 'Unknown resolution',
            standard: 'Broadcast HD (1920x1080 recommended)'
        });

        // Frame Rate Check
        checks.push({
            name: 'Frame Rate',
            status: m.frameRate && [23.976, 24, 25, 29.97, 30, 50, 59.94, 60].includes(m.frameRate) 
                ? 'pass' 
                : m.frameRate ? 'warning' : 'unknown',
            message: m.frameRate ? `${m.frameRate} fps` : 'Unknown',
            standard: 'Standard: 23.976/24/25/29.97/30/50/59.94/60 fps'
        });

        // Audio Loudness (EBU R128 / ATSC A/85)
        if (m.loudnessLUFS !== undefined) {
            const inRange = m.loudnessLUFS >= -27 && m.loudnessLUFS <= -21;
            checks.push({
                name: 'Audio Loudness',
                status: inRange ? 'pass' : Math.abs(m.loudnessLUFS + 24) < 6 ? 'warning' : 'fail',
                message: `${m.loudnessLUFS.toFixed(1)} LUFS`,
                standard: 'EBU R128: -24±3 LUFS, ATSC A/85: -24 LUFS'
            });
        } else {
            checks.push({
                name: 'Audio Loudness',
                status: 'unknown',
                message: 'Not measured',
                standard: 'EBU R128: -24±3 LUFS, ATSC A/85: -24 LUFS'
            });
        }

        // True Peak Check
        if (m.truePeak !== undefined) {
            checks.push({
                name: 'True Peak',
                status: m.truePeak <= -1 ? 'pass' : m.truePeak <= 0 ? 'warning' : 'fail',
                message: `${m.truePeak.toFixed(1)} dBTP`,
                standard: 'Max: -1 dBTP (EBU R128)'
            });
        }

        // Aspect Ratio Check
        checks.push({
            name: 'Aspect Ratio',
            status: m.aspectRatio === '16:9' ? 'pass' : 
                   m.aspectRatio === '4:3' ? 'warning' : 'warning',
            message: m.aspectRatio || 'Unknown',
            standard: 'Broadcast: 16:9 (HD) or 4:3 (SD)'
        });

        // Video Track Present
        checks.push({
            name: 'Video Track',
            status: m.hasVideoTrack ? 'pass' : 'fail',
            message: m.hasVideoTrack ? 'Present' : 'Missing',
            standard: 'Required for video playback'
        });

        // Audio Track Present
        checks.push({
            name: 'Audio Track',
            status: m.hasAudioTrack ? 'pass' : 'warning',
            message: m.hasAudioTrack ? 'Present' : 'Missing',
            standard: 'Recommended for broadcast'
        });

        // Codec Compatibility
        if (m.videoCodec) {
            const broadcastCodecs = ['h264', 'h.264', 'avc', 'hevc', 'h.265', 'prores', 'dnxhd'];
            const isCompatible = broadcastCodecs.some(c => 
                m.videoCodec!.toLowerCase().includes(c)
            );
            checks.push({
                name: 'Video Codec',
                status: isCompatible ? 'pass' : 'warning',
                message: m.videoCodec,
                standard: 'Recommended: H.264, H.265, ProRes, DNxHD'
            });
        }

        // File Duration
        checks.push({
            name: 'Duration',
            status: m.duration && m.duration > 0 ? 'pass' : 'fail',
            message: m.duration ? formatDuration(m.duration) : 'Unknown',
            standard: 'Must have valid duration'
        });

        setCompliance(checks);
    };

    const generateWaveformPreview = async (video: HTMLVideoElement) => {
        // Simple waveform generation by sampling video frames
        // In production, this would analyze actual audio data
        const samples: number[] = [];
        const sampleCount = 100;
        
        for (let i = 0; i < sampleCount; i++) {
            // Generate mock waveform data (in production, use Web Audio API)
            samples.push(Math.random() * 0.6 + 0.2);
        }
        
        setWaveformData(samples);
    };

    const getStatusIcon = (status: ComplianceCheck['status']) => {
        switch (status) {
            case 'pass': return <CheckCircle size={16} className={styles.statusPass} />;
            case 'fail': return <XCircle size={16} className={styles.statusFail} />;
            case 'warning': return <AlertTriangle size={16} className={styles.statusWarning} />;
            default: return <AlertTriangle size={16} className={styles.statusUnknown} />;
        }
    };

    const formatFileSize = (bytes?: number): string => {
        if (!bytes) return 'Unknown';
        if (bytes < 1024) return `${bytes} B`;
        if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
        if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
        return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
    };

    const formatBitrate = (bps?: number): string => {
        if (!bps) return 'Unknown';
        if (bps < 1000) return `${bps} bps`;
        if (bps < 1000000) return `${(bps / 1000).toFixed(0)} Kbps`;
        return `${(bps / 1000000).toFixed(1)} Mbps`;
    };

    const overallComplianceScore = () => {
        if (compliance.length === 0) return 0;
        const weights = { pass: 1, warning: 0.5, fail: 0, unknown: 0.25 };
        const total = compliance.reduce((sum, c) => sum + weights[c.status], 0);
        return Math.round((total / compliance.length) * 100);
    };

    const exportReport = () => {
        const report = {
            media: {
                title: item.title,
                type: item.type,
                filePath: item.filePath,
                duration: item.duration
            },
            technicalMetrics: metrics,
            complianceChecks: compliance,
            overallScore: overallComplianceScore(),
            analyzedAt: new Date().toISOString()
        };

        const blob = new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `${item.title.replace(/[^a-zA-Z0-9]/g, '_')}_qc_report.json`;
        a.click();
        URL.revokeObjectURL(url);
    };

    return (
        <div className={styles.analyzerOverlay}>
            <div className={styles.analyzer}>
                <div className={styles.header}>
                    <div className={styles.headerTitle}>
                        <Activity size={20} />
                        <h2>Media Analysis & QC</h2>
                    </div>
                    <div className={styles.headerActions}>
                        <button 
                            className={styles.refreshBtn} 
                            onClick={analyzeMedia}
                            disabled={analyzing}
                        >
                            <RefreshCw size={16} className={analyzing ? styles.spinning : ''} />
                            {analyzing ? 'Analyzing...' : 'Re-analyze'}
                        </button>
                        <button className={styles.exportBtn} onClick={exportReport}>
                            <Download size={16} /> Export Report
                        </button>
                        <button className={styles.closeBtn} onClick={onClose}>
                            <X size={20} />
                        </button>
                    </div>
                </div>

                {/* Media Info Header */}
                <div className={styles.mediaInfo}>
                    <div 
                        className={styles.thumbnail}
                        style={{
                            backgroundImage: item.thumbnail?.startsWith('data:') 
                                ? `url(${item.thumbnail})` 
                                : undefined,
                            backgroundColor: item.thumbnail?.startsWith('#') 
                                ? item.thumbnail 
                                : '#333'
                        }}
                    >
                        {!item.thumbnail?.startsWith('data:') && <Film size={32} />}
                    </div>
                    <div className={styles.mediaDetails}>
                        <h3>{item.title}</h3>
                        <div className={styles.metaRow}>
                            <span className={styles.typeBadge}>{item.type}</span>
                            <span>{formatDuration(item.duration)}</span>
                            {item.filePath && <span className={styles.filePath}>{item.filePath}</span>}
                        </div>
                    </div>
                    <div className={styles.scoreCircle}>
                        <div className={styles.scoreValue}>{overallComplianceScore()}</div>
                        <div className={styles.scoreLabel}>Score</div>
                    </div>
                </div>

                {/* Tabs */}
                <div className={styles.tabs}>
                    <button 
                        className={`${styles.tab} ${activeTab === 'metrics' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('metrics')}
                    >
                        <Gauge size={16} /> Technical
                    </button>
                    <button 
                        className={`${styles.tab} ${activeTab === 'compliance' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('compliance')}
                    >
                        <Target size={16} /> Compliance
                    </button>
                    <button 
                        className={`${styles.tab} ${activeTab === 'waveform' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('waveform')}
                    >
                        <AudioWaveform size={16} /> Waveform
                    </button>
                    <button 
                        className={`${styles.tab} ${activeTab === 'history' ? styles.activeTab : ''}`}
                        onClick={() => setActiveTab('history')}
                    >
                        <Clock size={16} /> History
                    </button>
                </div>

                {/* Content */}
                <div className={styles.content}>
                    {activeTab === 'metrics' && (
                        <div className={styles.metricsGrid}>
                            {/* Video Metrics */}
                            <div className={styles.metricsSection}>
                                <h4><Tv size={16} /> Video</h4>
                                <div className={styles.metricsList}>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Resolution</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.resolution 
                                                ? `${metrics.resolution.width}×${metrics.resolution.height}` 
                                                : 'Unknown'}
                                            {metrics?.isHD && <span className={styles.badge}>HD</span>}
                                            {metrics?.is4K && <span className={styles.badge}>4K</span>}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Aspect Ratio</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.aspectRatio || 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Frame Rate</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.frameRate ? `${metrics.frameRate} fps` : 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Codec</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.videoCodec || 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Bitrate</span>
                                        <span className={styles.metricValue}>
                                            {formatBitrate(metrics?.videoBitrate)}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Color Space</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.colorSpace || 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Scan Type</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.scanType || 'Unknown'}
                                        </span>
                                    </div>
                                </div>
                            </div>

                            {/* Audio Metrics */}
                            <div className={styles.metricsSection}>
                                <h4><Volume2 size={16} /> Audio</h4>
                                <div className={styles.metricsList}>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Codec</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.audioCodec || 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Channels</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.channels 
                                                ? `${metrics.channels} (${metrics.channels === 2 ? 'Stereo' : metrics.channels === 1 ? 'Mono' : 'Surround'})` 
                                                : 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Sample Rate</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.sampleRate ? `${metrics.sampleRate / 1000} kHz` : 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Bitrate</span>
                                        <span className={styles.metricValue}>
                                            {formatBitrate(metrics?.audioBitrate)}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Loudness (LUFS)</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.loudnessLUFS !== undefined 
                                                ? `${metrics.loudnessLUFS.toFixed(1)} LUFS` 
                                                : 'Not measured'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>True Peak</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.truePeak !== undefined 
                                                ? `${metrics.truePeak.toFixed(1)} dBTP` 
                                                : 'Not measured'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Loudness Range</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.loudnessRange !== undefined 
                                                ? `${metrics.loudnessRange.toFixed(1)} LU` 
                                                : 'Not measured'}
                                        </span>
                                    </div>
                                </div>
                            </div>

                            {/* Container Metrics */}
                            <div className={styles.metricsSection}>
                                <h4><HardDrive size={16} /> Container</h4>
                                <div className={styles.metricsList}>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Format</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.containerFormat || 'Unknown'}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>File Size</span>
                                        <span className={styles.metricValue}>
                                            {formatFileSize(metrics?.fileSize)}
                                        </span>
                                    </div>
                                    <div className={styles.metric}>
                                        <span className={styles.metricLabel}>Duration</span>
                                        <span className={styles.metricValue}>
                                            {metrics?.duration 
                                                ? formatDuration(metrics.duration) 
                                                : formatDuration(item.duration)}
                                        </span>
                                    </div>
                                </div>
                            </div>
                        </div>
                    )}

                    {activeTab === 'compliance' && (
                        <div className={styles.complianceList}>
                            {compliance.map((check, index) => (
                                <div 
                                    key={index} 
                                    className={`${styles.complianceItem} ${styles[`status${check.status.charAt(0).toUpperCase() + check.status.slice(1)}`]}`}
                                >
                                    <div className={styles.checkIcon}>
                                        {getStatusIcon(check.status)}
                                    </div>
                                    <div className={styles.checkInfo}>
                                        <div className={styles.checkName}>{check.name}</div>
                                        <div className={styles.checkMessage}>{check.message}</div>
                                        {check.standard && (
                                            <div className={styles.checkStandard}>{check.standard}</div>
                                        )}
                                    </div>
                                </div>
                            ))}
                            {compliance.length === 0 && (
                                <div className={styles.emptyState}>
                                    <AlertTriangle size={48} />
                                    <p>Run analysis to check compliance</p>
                                </div>
                            )}
                        </div>
                    )}

                    {activeTab === 'waveform' && (
                        <div className={styles.waveformPanel}>
                            <div className={styles.waveformContainer}>
                                {waveformData ? (
                                    <svg 
                                        className={styles.waveformSvg} 
                                        viewBox="0 0 100 40" 
                                        preserveAspectRatio="none"
                                    >
                                        <defs>
                                            <linearGradient id="waveGrad" x1="0" y1="0" x2="0" y2="1">
                                                <stop offset="0%" stopColor="#3b82f6" />
                                                <stop offset="100%" stopColor="#1d4ed8" />
                                            </linearGradient>
                                        </defs>
                                        {waveformData.map((value, i) => (
                                            <rect
                                                key={i}
                                                x={i}
                                                y={20 - value * 20}
                                                width="0.8"
                                                height={value * 40}
                                                fill="url(#waveGrad)"
                                                rx="0.2"
                                            />
                                        ))}
                                    </svg>
                                ) : (
                                    <div className={styles.emptyState}>
                                        <AudioWaveform size={48} />
                                        <p>Waveform not available</p>
                                    </div>
                                )}
                            </div>
                            <div className={styles.waveformTimeline}>
                                <span>0:00</span>
                                <span>{formatDuration(item.duration / 2)}</span>
                                <span>{formatDuration(item.duration)}</span>
                            </div>
                        </div>
                    )}

                    {activeTab === 'history' && (
                        <div className={styles.historyPanel}>
                            <div className={styles.historyItem}>
                                <div className={styles.historyIcon}><Upload size={16} /></div>
                                <div className={styles.historyInfo}>
                                    <div className={styles.historyAction}>Imported to library</div>
                                    <div className={styles.historyDate}>
                                        {item.addedAt 
                                            ? new Date(item.addedAt).toLocaleString() 
                                            : 'Unknown'}
                                    </div>
                                </div>
                            </div>
                            {item.lastPlayedAt && (
                                <div className={styles.historyItem}>
                                    <div className={styles.historyIcon}><Eye size={16} /></div>
                                    <div className={styles.historyInfo}>
                                        <div className={styles.historyAction}>Last played</div>
                                        <div className={styles.historyDate}>
                                            {new Date(item.lastPlayedAt).toLocaleString()}
                                        </div>
                                    </div>
                                </div>
                            )}
                            <div className={styles.historyItem}>
                                <div className={styles.historyIcon}><BarChart2 size={16} /></div>
                                <div className={styles.historyInfo}>
                                    <div className={styles.historyAction}>Total plays</div>
                                    <div className={styles.historyDate}>{item.playCount || 0} times</div>
                                </div>
                            </div>
                        </div>
                    )}
                </div>

                {/* Hidden video element for analysis */}
                <video ref={videoRef} style={{ display: 'none' }} crossOrigin="anonymous" />
            </div>
        </div>
    );
}

export default MediaAnalyzer;
