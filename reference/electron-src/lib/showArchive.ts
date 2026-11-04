import { TranscriptionResult } from './transcriptionService';
import { generateId } from './utils';

export interface ShowArchive {
    id: string;
    startTime: number;
    endTime: number;
    duration: number;
    transcript: TranscriptionResult | null;
    recordingPath?: string;
    metadata: Record<string, unknown>;
}

class ShowArchiveService {
    private archives: Map<string, ShowArchive> = new Map();

    async saveShow(archive: ShowArchive): Promise<void> {
        this.archives.set(archive.id, archive);
        
        // Save via Electron IPC
        if (typeof window !== 'undefined' && (window as any).electronAPI?.archiveSaveShow) {
            await (window as any).electronAPI.archiveSaveShow(archive);
        }
    }

    async getArchive(id: string): Promise<ShowArchive | null> {
        return this.archives.get(id) || null;
    }

    async getAllArchives(): Promise<ShowArchive[]> {
        return Array.from(this.archives.values()).sort((a, b) => b.startTime - a.startTime);
    }

    async searchArchives(query: string): Promise<ShowArchive[]> {
        const all = await this.getAllArchives();
        const lowerQuery = query.toLowerCase();
        return all.filter(archive => 
            archive.transcript?.text.toLowerCase().includes(lowerQuery) ||
            JSON.stringify(archive.metadata).toLowerCase().includes(lowerQuery)
        );
    }
}

export const showArchiveService = new ShowArchiveService();

