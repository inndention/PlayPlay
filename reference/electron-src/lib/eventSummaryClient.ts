/**
 * Optional renderer → main one-line integration log (see electron/eventSummaryLog.js).
 */

type EventSummaryRow = { channel?: string; event: string; id?: string; ts?: number };

export async function appendEventSummary(row: EventSummaryRow): Promise<void> {
    const api = (window as { electronAPI?: { appendEventSummary?: (r: EventSummaryRow) => Promise<unknown> } })
        .electronAPI;
    if (!api?.appendEventSummary) return;
    try {
        await api.appendEventSummary(row);
    } catch {
        /* non-fatal */
    }
}
