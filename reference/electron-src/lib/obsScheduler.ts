import { loadOBSCommands } from './storage';
import { OBSWebSocketClient } from './obsWebSocket';
import { executeOBSCommand } from './obsCommandExecutor';
import { persistentLogger } from './persistentLogger';

interface ScheduledOBSCommand {
    id: string;
    commandId: string;
    runAt: number;
    label: string;
}

const STORAGE_KEY = 'scheduled_obs_commands';

function loadSchedule(): ScheduledOBSCommand[] {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return [];
        const parsed = JSON.parse(raw);
        return Array.isArray(parsed) ? parsed : [];
    } catch {
        return [];
    }
}

function saveSchedule(list: ScheduledOBSCommand[]) {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
    } catch {
        /* ignore */
    }
}

let timer: ReturnType<typeof setInterval> | null = null;

export function scheduleOBSCommand(commandId: string, runAt: number, label?: string) {
    const list = loadSchedule();
    list.push({
        id: `${Date.now()}_${Math.random().toString(36).slice(2, 6)}`,
        commandId,
        runAt,
        label: label || commandId,
    });
    saveSchedule(list);
    persistentLogger.info('Scheduled OBS command', { commandId, runAt, label }, 'OBS');
}

export function startOBSScheduler(client: OBSWebSocketClient) {
    if (timer) return;
    timer = setInterval(async () => {
        const now = Date.now();
        const list = loadSchedule();
        const due = list.filter(c => c.runAt <= now);
        const remaining = list.filter(c => c.runAt > now);
        if (due.length === 0) return;

        const commands = loadOBSCommands();
        for (const sched of due) {
            const cmd = commands.find(c => c.id === sched.commandId);
            if (!cmd) {
                persistentLogger.warn('Scheduled OBS command not found', { commandId: sched.commandId }, 'OBS');
                continue;
            }
            try {
                await executeOBSCommand(cmd, client);
                persistentLogger.info('Executed scheduled OBS command', { commandId: sched.commandId }, 'OBS');
            } catch (e) {
                persistentLogger.error('Failed scheduled OBS command', { commandId: sched.commandId, error: String(e) }, 'OBS');
            }
        }
        saveSchedule(remaining);
    }, 1000);
}

export function stopOBSScheduler() {
    if (timer) {
        clearInterval(timer);
        timer = null;
    }
}
