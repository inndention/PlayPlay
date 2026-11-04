/**
 * REPORTING SERVICE
 * 
 * Manages the generation and scheduling of regulatory reports.
 * - Hourly "As-Played" CSV exports
 * - Weekly compliance summaries
 * - Traffic delivery reports
 */

import { persistentLogger } from './persistentLogger';
import { loadPlayLogs, PlayLog, getDaypart } from './trafficEngine';

export interface ReportConfig {
    autoExport: boolean;
    exportInterval: 'hourly' | 'daily';
    storagePath?: string;
}

export class ReportingService {
    private config: ReportConfig = {
        autoExport: true,
        exportInterval: 'hourly',
    };

    private lastExportHour: number = -1;
    private checkInterval: number | null = null;

    constructor() {
        this.init();
    }

    private init() {
        // Legacy setInterval removed in favor of direct engine tick subscription
    }

    /**
     * Called by AutomationEngine on its precision tick.
     * Replaces legacy polling.
     */
    public onEngineTick(currentEngineTimeMs: number) {
        const now = new Date(currentEngineTimeMs);
        const currentHour = now.getHours();

        if (this.config.autoExport && this.config.exportInterval === 'hourly') {
            if (this.lastExportHour !== -1 && this.lastExportHour !== currentHour) {
                // Hour changed! Export the previous hour's data
                const lastHour = this.lastExportHour;
                const date = new Date(now);
                if (currentHour === 0) date.setDate(date.getDate() - 1); // If now is midnight, last hour was 23 of yesterday
                
                this.generateAsPlayedReport(date, lastHour);
            }
            this.lastExportHour = currentHour;
        }
    }

    /**
     * Generate "As-Played" CSV for a specific hour
     */
    public async generateAsPlayedReport(date: Date, hour: number): Promise<string | null> {
        persistentLogger.info(`Generating As-Played report for ${date.toDateString()} Hour: ${hour}`, undefined, 'Reporting');

        const logs = loadPlayLogs();
        const startOfHour = new Date(date).setHours(hour, 0, 0, 0);
        const endOfHour = new Date(date).setHours(hour, 59, 59, 999);

        const filtered = logs.filter(l => l.timestamp >= startOfHour && l.timestamp <= endOfHour);

        if (filtered.length === 0) {
            persistentLogger.warn(`No play logs found for requested hour: ${hour}`, undefined, 'Reporting');
            return null;
        }

        const csv = this.convertToCSV(filtered);
        const filename = `${date.toISOString().split('T')[0]}_H${hour.toString().padStart(2, '0')}_AsPlayed.csv`;

        try {
            if ((window as any).electronAPI?.saveFile) {
                await (window as any).electronAPI.saveFile(filename, csv, 'reports');
                persistentLogger.info(`Report saved successfully: ${filename}`, undefined, 'Reporting');
            } else {
                // Browser fallback: download
                this.downloadCSV(filename, csv);
            }
            return csv;
        } catch (e) {
            persistentLogger.error(`Failed to save report: ${filename}`, e, 'Reporting');
            return null;
        }
    }

    private convertToCSV(logs: PlayLog[]): string {
        const headers = ['Timestamp', 'ISO_Time', 'MediaID', 'Title', 'Class', 'Duration', 'Scheduled', 'Position', 'Daypart', 'Completed'];
        const rows = logs.map(l => {
            return [
                l.timestamp,
                new Date(l.timestamp).toISOString(),
                l.mediaId,
                `"${(l as any).title || 'Unknown'}"`, // Title might not be in PlayLog but we can try to derive
                l.contentClass,
                l.duration.toFixed(2),
                l.scheduled,
                l.position,
                l.daypart,
                l.completed
            ].join(',');
        });

        return [headers.join(','), ...rows].join('\n');
    }

    private downloadCSV(filename: string, csv: string) {
        const blob = new Blob([csv], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }
}

export const reportingService = new ReportingService();
