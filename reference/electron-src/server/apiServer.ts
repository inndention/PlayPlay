import fastify from 'fastify';
import path from 'path';
import fs from 'fs-extra';
import { SyncService } from '../lib/SyncService';
import {
    readReportsSnapshot,
    writeReportsSnapshot,
    AsPlayedReportEntry,
    ScheduleSlotSummary,
    ReportsSnapshot,
} from './reportsData';

// Configuration
const PORT = 3000;
const MEDIA_DIR = path.join(process.cwd(), 'media'); // Default media directory
const TRACKER_URL = `ws://localhost:${PORT}`; // Built-in tracker (if we implement one) or external

// Ensure media directory exists
fs.ensureDirSync(MEDIA_DIR);

// Initialize Sync Service
const syncService = new SyncService(MEDIA_DIR, [TRACKER_URL]);

const server = fastify({ logger: true });

// --- ROUTES ---

// Health Check
server.get('/api/v1/status', async (request, reply) => {
    const syncStatus = syncService.getStatus();
    return {
        status: 'online',
        version: '1.0.0',
        mode: 'HQ', // Configurable
        sync: syncStatus
    };
});

// --- REPORTS (external access for sales team) ---

// GET as-played report (optional query: start, end, client)
server.get<{ Querystring: { start?: string; end?: string; client?: string } }>(
    '/api/v1/reports/as-played',
    async (request, reply) => {
        const snapshot = await readReportsSnapshot();
        if (!snapshot || !snapshot.asPlayed) {
            return { asPlayed: [], message: 'No report data. Publish from playout app to enable external access.' };
        }
        let list: AsPlayedReportEntry[] = snapshot.asPlayed;
        const { start, end, client } = request.query;
        if (start) {
            const t = new Date(start).getTime();
            list = list.filter((e) => e.timestamp >= t);
        }
        if (end) {
            const t = new Date(end).getTime() + 86400000;
            list = list.filter((e) => e.timestamp < t);
        }
        if (client) {
            list = list.filter((e) => (e as any).clientId === client);
        }
        return { asPlayed: list, exportedAt: snapshot.exportedAt };
    }
);

// GET scheduled report (summary of schedule slots)
server.get('/api/v1/reports/scheduled', async (request, reply) => {
    const snapshot = await readReportsSnapshot();
    if (!snapshot || !snapshot.scheduleSlots) {
        return { scheduleSlots: [], message: 'No schedule data. Publish from playout app to enable external access.' };
    }
    return { scheduleSlots: snapshot.scheduleSlots, exportedAt: snapshot.exportedAt };
});

// GET full report snapshot (for report generation)
server.get('/api/v1/reports/snapshot', async (request, reply) => {
    const snapshot = await readReportsSnapshot();
    if (!snapshot) {
        return reply.code(404).send({ error: 'No report snapshot. Publish from playout app first.' });
    }
    return snapshot;
});

// POST publish snapshot (playout app sends as-played + schedule for external access)
server.post<{ Body: ReportsSnapshot }>('/api/v1/reports/snapshot', async (request, reply) => {
    const body = request.body;
    if (!body || !Array.isArray(body.asPlayed)) {
        return reply.code(400).send({ error: 'Body must include asPlayed array.' });
    }
    const snapshot: ReportsSnapshot = {
        asPlayed: body.asPlayed,
        scheduleSlots: Array.isArray(body.scheduleSlots) ? body.scheduleSlots : [],
        clients: body.clients,
        exportedAt: Date.now(),
    };
    await writeReportsSnapshot(snapshot);
    return { ok: true, exportedAt: snapshot.exportedAt };
});

// Playlists
server.get('/api/v1/playlists', async (request, reply) => {
    return { message: "Playlist listing not fully implemented in sidecar yet" };
});

// Sync: Push (Trigger download on remote)
// In a real scenario, HQ tells Remote to download. 
// Here, we simulate "Adding a file to the network" (Seeding)
server.post<{ Body: { filePath: string } }>('/api/v1/sync/seed', async (request, reply) => {
    const { filePath } = request.body;
    if (!filePath || !fs.existsSync(filePath)) {
        return reply.code(400).send({ error: 'File not found' });
    }

    const magnet = await syncService.seedFile(filePath);
    return { magnet };
});

// Sync: Pull (Add magnet to download)
server.post<{ Body: { magnet: string } }>('/api/v1/sync/download', async (request, reply) => {
    const { magnet } = request.body;
    if (!magnet) {
        return reply.code(400).send({ error: 'Magnet link required' });
    }

    await syncService.addMagnet(magnet);
    return { status: 'downloading' };
});

// Start Server
export async function startApiServer() {
    try {
        await server.listen({ port: PORT, host: '0.0.0.0' });
        console.log(`[API] Server listening on http://localhost:${PORT}`);
    } catch (err) {
        server.log.error(err);
        process.exit(1);
    }
}
