import { generateId } from './utils';

export interface TranscriptionSegment {
    id: string;
    start: number;
    end: number;
    text: string;
    speaker?: string;
    confidence?: number;
}

export interface TranscriptionResult {
    id: string;
    text: string;
    segments: TranscriptionSegment[];
    language: string;
    timestamp: number;
    duration: number;
}

class TranscriptionService {
    private isRecording: boolean = false;
    private currentTranscription: TranscriptionResult | null = null;
    private onUpdateCallbacks: Set<(result: TranscriptionResult) => void> = new Set();

    async startTranscription(): Promise<void> {
        if (this.isRecording) return;
        
        this.isRecording = true;
        this.currentTranscription = {
            id: generateId(),
            text: '',
            segments: [],
            language: 'en',
            timestamp: Date.now(),
            duration: 0,
        };

        // Start via Electron IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI?.transcriptionStart) {
            await (window as any).electronAPI.transcriptionStart();
        }
    }

    async stopTranscription(): Promise<TranscriptionResult | null> {
        if (!this.isRecording) return null;

        this.isRecording = false;

        if (typeof window !== 'undefined' && (window as any).electronAPI?.transcriptionStop) {
            const result = await (window as any).electronAPI.transcriptionStop();
            if (result.success && this.currentTranscription) {
                this.currentTranscription.duration = (Date.now() - this.currentTranscription.timestamp) / 1000;
                return this.currentTranscription;
            }
        }

        return this.currentTranscription;
    }

    onUpdate(callback: (result: TranscriptionResult) => void): () => void {
        this.onUpdateCallbacks.add(callback);
        return () => this.onUpdateCallbacks.delete(callback);
    }

    getIsRecording(): boolean {
        return this.isRecording;
    }

    getCurrentTranscription(): TranscriptionResult | null {
        return this.currentTranscription;
    }
}

export const transcriptionService = new TranscriptionService();

