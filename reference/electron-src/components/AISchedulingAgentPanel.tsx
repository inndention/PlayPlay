import React, { useMemo, useState } from 'react';
import { Bot, PlayCircle, Shield, ClipboardList, AlertTriangle, CheckCircle2, Loader2 } from 'lucide-react';
import { runSchedulingAgent, type AgentExecuteResult } from '../lib/aiSchedulingAgent';
import styles from './AISchedulingAgentPanel.module.css';

const EXAMPLE_PROMPTS = [
    'Build a balanced 6am local morning block for current day-hour with weather, local headlines, and ad separation.',
    'Rebuild slot 1-18 to 58 minutes total, no adjacent ads, prioritize campaigns tagged Martinsville.',
    'Pause all media that belongs to Client X and refill affected hour slots with station-safe fillers.',
    'Assign programSeries "School Sports" to top 20 uncategorized sports clips and prep tonight 7pm slot.',
];

export function AISchedulingAgentPanel() {
    const [prompt, setPrompt] = useState(EXAMPLE_PROMPTS[0]);
    const [running, setRunning] = useState(false);
    const [result, setResult] = useState<AgentExecuteResult | null>(null);

    const run = async (dryRun: boolean) => {
        if (!prompt.trim() || running) return;
        setRunning(true);
        try {
            const next = await runSchedulingAgent(prompt.trim(), { dryRun });
            setResult(next);
        } finally {
            setRunning(false);
        }
    };

    const status = useMemo(() => {
        if (!result) return null;
        if (!result.success) return { label: 'Failed', icon: <AlertTriangle size={16} />, cls: styles.statusError };
        if (result.dryRun) return { label: 'Dry Run Ready', icon: <Shield size={16} />, cls: styles.statusDry };
        return { label: 'Applied', icon: <CheckCircle2 size={16} />, cls: styles.statusOk };
    }, [result]);

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.titleWrap}>
                    <Bot size={18} />
                    <h2>OpenRouter Scheduling Agent</h2>
                </div>
                <span className={styles.badge}>Broadcast-grade</span>
            </div>

            <p className={styles.subtitle}>
                Handles complex scheduling + media-library operations with a strict dry-run first workflow.
                When the in-app Hermes supervisor is enabled in Settings → OpenRouter, the same agent runs behind a bounded OpenRouter loop (validate JSON, retry, cap tokens) — not the Nous Hermes Agent CLI.
            </p>

            <div className={styles.examples}>
                {EXAMPLE_PROMPTS.map((example) => (
                    <button key={example} type="button" className={styles.exampleBtn} onClick={() => setPrompt(example)}>
                        {example}
                    </button>
                ))}
            </div>

            <textarea
                value={prompt}
                onChange={(e) => setPrompt(e.target.value)}
                className={styles.prompt}
                rows={5}
                placeholder="Describe the schedule/library request in plain English."
            />

            <div className={styles.actions}>
                <button type="button" className={styles.dryRunBtn} onClick={() => void run(true)} disabled={running}>
                    {running ? <Loader2 size={16} className={styles.spin} /> : <Shield size={16} />}
                    Dry Run
                </button>
                <button type="button" className={styles.applyBtn} onClick={() => void run(false)} disabled={running}>
                    {running ? <Loader2 size={16} className={styles.spin} /> : <PlayCircle size={16} />}
                    Apply Plan
                </button>
            </div>

            {result && (
                <div className={styles.result}>
                    <div className={styles.resultHeader}>
                        <div className={`${styles.statusPill} ${status?.cls || ''}`}>
                            {status?.icon}
                            <span>{status?.label || 'Unknown'}</span>
                        </div>
                        <span className={styles.summary}>{result.summary}</span>
                    </div>

                    {result.plan && (
                        <div className={styles.section}>
                            <h3><ClipboardList size={15} /> Plan</h3>
                            <ul>
                                {result.plan.operations.map((op, idx) => (
                                    <li key={`${op.type}_${idx}`}>
                                        <code>{op.type}</code> - {op.reason}
                                    </li>
                                ))}
                            </ul>
                        </div>
                    )}

                    <div className={styles.section}>
                        <h3>Changes</h3>
                        <div className={styles.metrics}>
                            <span>Operations: {result.appliedOperations.length}</span>
                            <span>Slots: {result.changedSlots.length}</span>
                            <span>Media: {result.changedMedia.length}</span>
                        </div>
                    </div>

                    {result.warnings.length > 0 && (
                        <div className={styles.section}>
                            <h3><AlertTriangle size={15} /> Warnings</h3>
                            <ul>
                                {result.warnings.map((w, idx) => (
                                    <li key={`${w}_${idx}`}>{w}</li>
                                ))}
                            </ul>
                        </div>
                    )}

                    {!!result.error && <div className={styles.error}>{result.error}</div>}
                </div>
            )}
        </div>
    );
}
