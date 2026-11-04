/**
 * StudioDirectorPanel.tsx
 * Full AI studio director UI — announcements, break cues,
 * breaking news, live shots, weather TTS, filler copy.
 */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
    AnnouncementKind,
    AnnouncementResult,
    BreakingNewsAlert,
    LiveShotSession,
    SiteId,
    SITE_PROFILES,
    getStudioDirector,
} from '../lib/studioDirector';
import styles from './StudioDirectorPanel.module.css';

// ─── Local types ──────────────────────────────────────────────────────────────

interface LogEntry {
    id: string;
    ts: number;
    kind: string;
    copy: string;
    site?: SiteId;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

function fmtTime(ts: number) {
    return new Date(ts).toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

const ANNOUNCEMENT_LABELS: Record<AnnouncementKind, string> = {
    top_of_hour      : '🕐 Top of Hour',
    half_hour        : '🕧 Half Hour',
    break_warning_30 : '⏳ Break Warning 30s',
    break_warning_10 : '⏱ Break Warning 10s',
    break_now        : '🔴 Break Now',
    back_from_break  : '🟢 Back from Break',
    breaking_news    : '🚨 Breaking News',
    live_shot_intro  : '📡 Live Shot Intro',
    live_shot_outro  : '📡 Live Shot Outro',
    weather_update   : '☁️ Weather Update',
    coming_up        : '📺 Coming Up',
    custom           : '✏️ Custom',
};

const FILLER_KINDS = [
    { id: 'got_scoop'     as const, label: '📰 Got Scoop?'          },
    { id: 'vdot_alert'    as const, label: '🚧 VDOT/NCDOT Alert'    },
    { id: 'school_sports' as const, label: '🏅 School Sports'        },
    { id: 'closing_delay' as const, label: '📢 Closings & Delays'    },
    { id: 'local_history' as const, label: '📜 On This Day'          },
    { id: 'fun_fact'      as const, label: '💡 Local Fun Fact'       },
];

// ─── Component ────────────────────────────────────────────────────────────────

export default function StudioDirectorPanel() {
    const director = getStudioDirector();

    const [selectedSite, setSelectedSite]           = useState<SiteId>('martinsville');
    const [log, setLog]                             = useState<LogEntry[]>([]);
    const [liveShots, setLiveShots]                 = useState<LiveShotSession[]>([]);
    const [customCopy, setCustomCopy]               = useState('');
    const [breakingHeadline, setBreakingHeadline]   = useState('');
    const [liveShotReporter, setLiveShotReporter]   = useState('');
    const [liveShotLocation, setLiveShotLocation]   = useState('');
    const [fillerContext, setFillerContext]          = useState('');
    const [generating, setGenerating]               = useState<string | null>(null);
    const [lastCopy, setLastCopy]                   = useState('');
    const logEndRef = useRef<HTMLDivElement>(null);

    // Listen to director events
    useEffect(() => {
        const unsub = director.on((evt) => {
            if (evt.type === 'announcement_ready') {
                const r = evt.payload as AnnouncementResult;
                addLog(r.kind, r.copy, r.siteId);
                setLastCopy(r.copy);
            } else if (evt.type === 'live_shot_taken' || evt.type === 'live_shot_released') {
                setLiveShots(director.getAllLiveShots());
            }
        });

        // Hourly IDs for selected site
        const stopHourly = director.scheduleHourlyAnnouncements(selectedSite);

        return () => {
            unsub();
            stopHourly();
        };
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [selectedSite]);

    useEffect(() => {
        logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }, [log]);

    const addLog = useCallback((kind: string, copy: string, site?: SiteId) => {
        setLog(prev => [...prev.slice(-99), {
            id  : `${Date.now()}_${Math.random()}`,
            ts  : Date.now(),
            kind,
            copy,
            site,
        }]);
    }, []);

    // ── One-click announcement buttons ────────────────────────────────────────

    const fire = useCallback(async (kind: AnnouncementKind) => {
        setGenerating(kind);
        try {
            await director.generateAnnouncement({ kind, siteId: selectedSite });
        } finally {
            setGenerating(null);
        }
    }, [director, selectedSite]);

    // ── Breaking news ─────────────────────────────────────────────────────────

    const fireBreaking = useCallback(async () => {
        if (!breakingHeadline.trim()) return;
        setGenerating('breaking_news');
        const alert: BreakingNewsAlert = {
            id          : `bn_${Date.now()}`,
            headline    : breakingHeadline.trim(),
            siteIds     : [selectedSite],
            priority    : 'urgent',
            triggeredAt : Date.now(),
        };
        try {
            await director.triggerBreakingNews(alert);
            setBreakingHeadline('');
        } finally {
            setGenerating(null);
        }
    }, [director, breakingHeadline, selectedSite]);

    // ── Live shot ─────────────────────────────────────────────────────────────

    const takeLiveShot = useCallback(async () => {
        if (!liveShotReporter.trim() || !liveShotLocation.trim()) return;
        setGenerating('live_shot_intro');
        try {
            await director.takeLiveShot({
                reporterName   : liveShotReporter.trim(),
                location       : liveShotLocation.trim(),
                siteId         : selectedSite,
                webrtcSessionId: `ws_${Date.now()}`,
            });
            setLiveShots(director.getAllLiveShots());
        } finally {
            setGenerating(null);
        }
    }, [director, liveShotReporter, liveShotLocation, selectedSite]);

    const releaseLiveShot = useCallback(async (id: string) => {
        setGenerating('live_shot_outro');
        try {
            await director.releaseLiveShot(id);
            setLiveShots(director.getAllLiveShots());
        } finally {
            setGenerating(null);
        }
    }, [director]);

    // ── Filler copy ───────────────────────────────────────────────────────────

    const fireFiller = useCallback(async (kind: typeof FILLER_KINDS[number]['id']) => {
        setGenerating(kind);
        try {
            const copy = await director.generateFillerCopy({ kind, siteId: selectedSite, extraContext: fillerContext });
            if (copy) addLog(kind, copy, selectedSite);
        } finally {
            setGenerating(null);
        }
    }, [director, selectedSite, fillerContext, addLog]);

    // ── Weather ───────────────────────────────────────────────────────────────

    const fireWeather = useCallback(async () => {
        setGenerating('weather_update');
        try {
            await director.announceWeather(selectedSite);
        } finally {
            setGenerating(null);
        }
    }, [director, selectedSite]);

    // ─── Render ───────────────────────────────────────────────────────────────

    const siteOptions = Object.values(SITE_PROFILES);

    return (
        <div className={styles.panel}>
            {/* ── Header ── */}
            <div className={styles.header}>
                <span className={styles.title}>🎙️ AI Studio Director</span>
                <select
                    className={styles.siteSelect}
                    value={selectedSite}
                    onChange={e => setSelectedSite(e.target.value as SiteId)}
                >
                    {siteOptions.map(s => (
                        <option key={s.id} value={s.id}>{s.displayName}</option>
                    ))}
                </select>
            </div>

            <div className={styles.body}>
                {/* ── Left col: cue buttons ── */}
                <div className={styles.cueCol}>
                    {/* Break cues */}
                    <section className={styles.section}>
                        <h3 className={styles.sectionTitle}>Break Cues</h3>
                        <div className={styles.cueGrid}>
                            {(['break_warning_30', 'break_warning_10', 'break_now', 'back_from_break'] as AnnouncementKind[]).map(k => (
                                <button
                                    key={k}
                                    className={`${styles.cueBtn} ${k === 'break_now' ? styles.cueBtnRed : k === 'back_from_break' ? styles.cueBtnGreen : ''}`}
                                    onClick={() => fire(k)}
                                    disabled={!!generating}
                                >
                                    {generating === k ? '…' : ANNOUNCEMENT_LABELS[k]}
                                </button>
                            ))}
                        </div>
                    </section>

                    {/* Station IDs */}
                    <section className={styles.section}>
                        <h3 className={styles.sectionTitle}>Station ID</h3>
                        <div className={styles.cueGrid}>
                            {(['top_of_hour', 'half_hour', 'coming_up'] as AnnouncementKind[]).map(k => (
                                <button
                                    key={k}
                                    className={styles.cueBtn}
                                    onClick={() => fire(k)}
                                    disabled={!!generating}
                                >
                                    {generating === k ? '…' : ANNOUNCEMENT_LABELS[k]}
                                </button>
                            ))}
                            <button className={styles.cueBtn} onClick={fireWeather} disabled={!!generating}>
                                {generating === 'weather_update' ? '…' : '☁️ Weather Update'}
                            </button>
                        </div>
                    </section>

                    {/* Breaking News */}
                    <section className={styles.section}>
                        <h3 className={`${styles.sectionTitle} ${styles.urgent}`}>🚨 Breaking News</h3>
                        <input
                            className={styles.input}
                            placeholder="Headline…"
                            value={breakingHeadline}
                            onChange={e => setBreakingHeadline(e.target.value)}
                            onKeyDown={e => e.key === 'Enter' && fireBreaking()}
                        />
                        <button
                            className={`${styles.cueBtn} ${styles.cueBtnRed} ${styles.fullWidth}`}
                            onClick={fireBreaking}
                            disabled={!breakingHeadline.trim() || !!generating}
                        >
                            {generating === 'breaking_news' ? 'Generating…' : '🚨 TRIGGER BREAKING NEWS'}
                        </button>
                    </section>

                    {/* Live shot */}
                    <section className={styles.section}>
                        <h3 className={styles.sectionTitle}>📡 Live Shot</h3>
                        <input
                            className={styles.input}
                            placeholder="Reporter name"
                            value={liveShotReporter}
                            onChange={e => setLiveShotReporter(e.target.value)}
                        />
                        <input
                            className={styles.input}
                            placeholder="Location"
                            value={liveShotLocation}
                            onChange={e => setLiveShotLocation(e.target.value)}
                        />
                        <button
                            className={`${styles.cueBtn} ${styles.cueBtnGreen} ${styles.fullWidth}`}
                            onClick={takeLiveShot}
                            disabled={!liveShotReporter.trim() || !liveShotLocation.trim() || !!generating}
                        >
                            {generating === 'live_shot_intro' ? 'Going Live…' : '📡 TAKE LIVE SHOT'}
                        </button>

                        {liveShots.length > 0 && (
                            <div className={styles.liveList}>
                                {liveShots.map(s => (
                                    <div key={s.id} className={styles.liveItem}>
                                        <span className={styles.liveDot} />
                                        <span>{s.reporterName} @ {s.location}</span>
                                        <button
                                            className={styles.releaseBtn}
                                            onClick={() => releaseLiveShot(s.id)}
                                            disabled={!!generating}
                                        >
                                            Release
                                        </button>
                                    </div>
                                ))}
                            </div>
                        )}
                    </section>

                    {/* AI filler generators */}
                    <section className={styles.section}>
                        <h3 className={styles.sectionTitle}>AI Filler Generators</h3>
                        <input
                            className={styles.input}
                            placeholder="Optional context / extra info…"
                            value={fillerContext}
                            onChange={e => setFillerContext(e.target.value)}
                        />
                        <div className={styles.cueGrid}>
                            {FILLER_KINDS.map(f => (
                                <button
                                    key={f.id}
                                    className={styles.cueBtn}
                                    onClick={() => fireFiller(f.id)}
                                    disabled={!!generating}
                                >
                                    {generating === f.id ? '…' : f.label}
                                </button>
                            ))}
                        </div>
                    </section>

                    {/* Custom copy */}
                    <section className={styles.section}>
                        <h3 className={styles.sectionTitle}>✏️ Custom Announcement</h3>
                        <textarea
                            className={styles.textarea}
                            placeholder="Type any copy — AI will read it aloud…"
                            value={customCopy}
                            onChange={e => setCustomCopy(e.target.value)}
                            rows={3}
                        />
                        <button
                            className={`${styles.cueBtn} ${styles.fullWidth}`}
                            onClick={() => director.generateAnnouncement({ kind: 'custom', siteId: selectedSite, forcedCopy: customCopy })}
                            disabled={!customCopy.trim() || !!generating}
                        >
                            🔊 Speak Custom Copy
                        </button>
                    </section>
                </div>

                {/* ── Right col: log & last copy ── */}
                <div className={styles.logCol}>
                    {lastCopy && (
                        <div className={styles.lastCopyBox}>
                            <span className={styles.lastCopyLabel}>Last Generated Copy</span>
                            <p className={styles.lastCopyText}>"{lastCopy}"</p>
                        </div>
                    )}

                    <div className={styles.logHeader}>
                        <span className={styles.logTitle}>📋 Director Log</span>
                        <button className={styles.clearBtn} onClick={() => setLog([])}>Clear</button>
                    </div>
                    <div className={styles.logList}>
                        {log.length === 0 && (
                            <p className={styles.emptyLog}>No announcements fired yet. Use the cue buttons above.</p>
                        )}
                        {log.map(entry => (
                            <div key={entry.id} className={styles.logEntry}>
                                <span className={styles.logTime}>{fmtTime(entry.ts)}</span>
                                <span className={styles.logKind}>{ANNOUNCEMENT_LABELS[entry.kind as AnnouncementKind] ?? entry.kind}</span>
                                {entry.site && <span className={styles.logSite}>{SITE_PROFILES[entry.site].weather.city}</span>}
                                <p className={styles.logCopy}>"{entry.copy}"</p>
                            </div>
                        ))}
                        <div ref={logEndRef} />
                    </div>
                </div>
            </div>
        </div>
    );
}
