import React, { useMemo } from 'react';
import type { PlaylistItem } from '../lib/types';

interface Props {
    playlist: PlaylistItem[];
    activeItemId: string | null;
    currentPlaybackTime: number;
}

function fmtClock(ms?: number): string {
    if (!ms) return '--:--:--';
    return new Date(ms).toLocaleTimeString([], { hour12: false });
}

export default function HourlyRundownPanel({ playlist, activeItemId, currentPlaybackTime }: Props) {
    const rows = useMemo(() => {
        return playlist.map((item) => {
            const start = item.startTime;
            const end = item.endTime ?? (item.startTime ? item.startTime + item.media.duration * 1000 : undefined);
            const remaining = item.id === activeItemId
                ? Math.max(0, (item.media.duration || 0) - currentPlaybackTime)
                : null;
            return { item, start, end, remaining };
        });
    }, [playlist, activeItemId, currentPlaybackTime]);

    return (
        <div style={{ marginTop: 8, border: '1px solid var(--border)', borderRadius: 8, padding: 8, background: 'var(--surface)' }}>
            <div style={{ fontWeight: 700, fontSize: 12, marginBottom: 6 }}>Current-Hour Rundown</div>
            {rows.slice(0, 12).map((row) => (
                <div key={row.item.id} style={{
                    display: 'grid',
                    gridTemplateColumns: '80px 1fr auto',
                    gap: 8,
                    padding: '4px 0',
                    borderBottom: '1px solid var(--border-subtle)',
                    color: row.item.id === activeItemId ? 'var(--accent)' : 'var(--text-primary)',
                }}>
                    <span style={{ fontFamily: 'monospace', fontSize: 11 }}>{fmtClock(row.start)}-{fmtClock(row.end)}</span>
                    <span style={{ fontSize: 12, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{row.item.media.title}</span>
                    <span style={{ fontFamily: 'monospace', fontSize: 11 }}>
                        {row.remaining != null ? `T-${Math.round(row.remaining)}s` : ''}
                    </span>
                </div>
            ))}
        </div>
    );
}
