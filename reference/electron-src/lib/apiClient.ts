const API_BASE = 'http://localhost:3000/api/v1';
const DEFAULT_TIMEOUT_MS = 8000;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
async function apiFetch<T = any>(url: string, init?: RequestInit, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<T> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
        const res = await fetch(url, { ...init, signal: controller.signal });
        if (!res.ok) {
            throw new Error(`API error ${res.status} ${res.statusText} — ${url}`);
        }
        return await res.json() as T;
    } catch (err) {
        if ((err as Error).name === 'AbortError') {
            throw new Error(`API request timed out after ${timeoutMs}ms — ${url}`);
        }
        throw err;
    } finally {
        clearTimeout(timer);
    }
}

function json(body: unknown): RequestInit {
    return {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    };
}

export const apiClient = {
    async getStatus() {
        return apiFetch(`${API_BASE}/status`);
    },

    async findFile(hash: string) {
        try {
            return await apiFetch(`${API_BASE}/files/find?hash=${encodeURIComponent(hash)}`);
        } catch {
            return null;
        }
    },

    async seedFile(filePath: string) {
        return apiFetch(`${API_BASE}/sync/seed`, json({ filePath }));
    },

    async downloadFile(magnet: string) {
        return apiFetch(`${API_BASE}/sync/download`, json({ magnet }));
    },

    async getDevices() {
        return apiFetch(`${API_BASE}/devices`);
    },

    async getUsbDrives() {
        return apiFetch(`${API_BASE}/usb/drives`);
    },

    async exportToUsb(drivePath: string, files: string[]) {
        return apiFetch(`${API_BASE}/usb/export`, json({ drivePath, files }));
    },

    async ingestFromUsb(drivePath: string) {
        return apiFetch(`${API_BASE}/usb/ingest`, json({ drivePath }));
    },

    async getMeshState() {
        return apiFetch(`${API_BASE}/mesh/state`);
    },

    async getSwarmState() {
        return apiFetch(`${API_BASE}/swarm/state`);
    },

    async queueTransfer(targetNodeId: string, fileHash: string, magnetUri: string) {
        return apiFetch(`${API_BASE}/swarm/transfer`, json({ targetNodeId, fileHash, magnetUri }));
    },

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    async logActivity(logs: any[]) {
        return apiFetch(`${API_BASE}/analytics/log`, json({ logs }));
    },

    async getYouTubeAuthUrl() {
        return apiFetch(`${API_BASE}/youtube/auth-url`);
    },

    async authenticateYouTube(code: string) {
        return apiFetch(`${API_BASE}/youtube/authenticate`, json({ code }));
    },

    async uploadToYouTube(filePath: string, title: string, description: string) {
        return apiFetch(`${API_BASE}/youtube/upload`, json({ filePath, title, description }), 120_000);
    },
};
