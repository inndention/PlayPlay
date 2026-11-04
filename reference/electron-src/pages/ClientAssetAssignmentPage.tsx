import React, { useState, useEffect } from 'react';
import { AdClient, MediaItem, SchedulingRule } from '../lib/types';
import { loadClients, saveClients, loadLibraryMedia } from '../lib/storage';
import { 
    Users, Plus, Save, Trash, X, Mail, Edit2, Calendar, Clock, 
    BarChart3, Settings, Filter, Search, FileVideo, CheckCircle2,
    ArrowLeft, Copy, Check, Download, FileText
} from 'lucide-react';
import styles from './ClientAssetAssignmentPage.module.css';

export interface AssetAssignment {
    id: string;
    clientId: string;
    mediaId: string;
    media: MediaItem;
    rules: SchedulingRule;
    priority: number;
    campaignId?: string;
    notes?: string;
    createdAt: string;
    updatedAt: string;
}

const DAYS_OF_WEEK = [
    { value: 0, label: 'Sunday', short: 'Sun' },
    { value: 1, label: 'Monday', short: 'Mon' },
    { value: 2, label: 'Tuesday', short: 'Tue' },
    { value: 3, label: 'Wednesday', short: 'Wed' },
    { value: 4, label: 'Thursday', short: 'Thu' },
    { value: 5, label: 'Friday', short: 'Fri' },
    { value: 6, label: 'Saturday', short: 'Sat' },
];

const HOURS = Array.from({ length: 24 }, (_, i) => i);

export function ClientAssetAssignmentPage() {
    const [clients, setClients] = useState<AdClient[]>([]);
    const [mediaLibrary, setMediaLibrary] = useState<MediaItem[]>([]);
    const [assignments, setAssignments] = useState<AssetAssignment[]>([]);
    const [selectedClientId, setSelectedClientId] = useState<string | null>(null);
    const [selectedAssignmentId, setSelectedAssignmentId] = useState<string | null>(null);
    const [isEditing, setIsEditing] = useState(false);
    const [searchQuery, setSearchQuery] = useState('');
    const [filterClientId, setFilterClientId] = useState<string | null>(null);
    const [urlCopied, setUrlCopied] = useState(false);
    const [showReport, setShowReport] = useState(false);

    const externalUrl = `${window.location.origin}/#/client-assignment`;

    const handleCopyUrl = () => {
        navigator.clipboard.writeText(externalUrl).then(() => {
            setUrlCopied(true);
            setTimeout(() => setUrlCopied(false), 2000);
        });
    };

    const handleBack = () => {
        window.location.hash = '#/playout';
    };

    const generateReport = () => {
        const reportData = {
            generatedAt: new Date().toISOString(),
            totalAssignments: assignments.length,
            clients: clients.map(client => {
                const clientAssignments = assignments.filter(a => a.clientId === client.id);
                return {
                    id: client.id,
                    name: client.name,
                    assignmentCount: clientAssignments.length,
                    assignments: clientAssignments.map(a => {
                        const media = mediaLibrary.find((m: MediaItem) => m.id === a.mediaId);
                        return {
                            mediaTitle: media?.title || 'Unknown',
                            rules: a.rules,
                            priority: a.priority,
                        };
                    }),
                };
            }),
            summary: {
                totalClients: clients.length,
                totalMedia: new Set(assignments.map(a => a.mediaId)).size,
                assignmentsByDay: DAYS_OF_WEEK.map(day => ({
                    day: day.label,
                    count: assignments.filter(a => 
                        a.rules.allowedDays.length === 0 || a.rules.allowedDays.includes(day.value)
                    ).length,
                })),
                assignmentsByHour: HOURS.map(hour => ({
                    hour: `${hour.toString().padStart(2, '0')}:00`,
                    count: assignments.filter(a => 
                        a.rules.allowedHours.length === 0 || a.rules.allowedHours.includes(hour)
                    ).length,
                })),
            },
        };

        const blob = new Blob([JSON.stringify(reportData, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `client-assignment-report-${new Date().toISOString().split('T')[0]}.json`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    };

    // Form state
    const [formData, setFormData] = useState<Partial<AssetAssignment>>({
        rules: {
            allowedHours: [],
            allowedDays: [],
            maxPlaysPerHour: 0,
            maxPlaysPerDay: 0,
            minIntervalMinutes: 0,
            priority: 0,
        },
        priority: 0,
    });

    useEffect(() => {
        loadData();
    }, []);

    const loadData = () => {
        const loadedClients = loadClients();
        const loadedMedia = loadLibraryMedia();
        setClients(loadedClients);
        setMediaLibrary(loadedMedia);
        
        // Load assignments from storage
        const savedAssignments = localStorage.getItem('assetAssignments');
        if (savedAssignments) {
            try {
                const parsed = JSON.parse(savedAssignments);
                // Resolve media references
                const resolved = parsed.map((a: AssetAssignment) => ({
                    ...a,
                    media: loadedMedia.find((m: MediaItem) => m.id === a.mediaId) || a.media,
                }));
                setAssignments(resolved);
            } catch (e) {
                console.error('Failed to load assignments:', e);
            }
        }
    };

    const saveAssignments = (newAssignments: AssetAssignment[]) => {
        setAssignments(newAssignments);
        localStorage.setItem('assetAssignments', JSON.stringify(newAssignments));
    };

    const handleCreateAssignment = () => {
        if (!selectedClientId) {
            alert('Please select a client first');
            return;
        }

        const newAssignment: AssetAssignment = {
            id: `assignment_${Date.now()}`,
            clientId: selectedClientId,
            mediaId: '',
            media: {} as MediaItem,
            rules: {
                allowedHours: [],
                allowedDays: [],
                maxPlaysPerHour: 0,
                maxPlaysPerDay: 0,
                minIntervalMinutes: 0,
                priority: 0,
            },
            priority: 0,
            createdAt: new Date().toISOString(),
            updatedAt: new Date().toISOString(),
        };

        const updated = [...assignments, newAssignment];
        saveAssignments(updated);
        setSelectedAssignmentId(newAssignment.id);
        setFormData(newAssignment);
        setIsEditing(true);
    };

    const handleSelectAssignment = (assignment: AssetAssignment) => {
        setSelectedAssignmentId(assignment.id);
        setFormData(assignment);
        setIsEditing(true);
    };

    const handleSaveAssignment = () => {
        if (!selectedAssignmentId) return;

        const updated = assignments.map(a =>
            a.id === selectedAssignmentId
                ? {
                    ...a,
                    ...formData,
                    updatedAt: new Date().toISOString(),
                } as AssetAssignment
                : a
        );

        saveAssignments(updated);
        setIsEditing(false);
        loadData();
    };

    const handleDeleteAssignment = (id: string) => {
        if (confirm('Delete this assignment?')) {
            const updated = assignments.filter(a => a.id !== id);
            saveAssignments(updated);
            if (selectedAssignmentId === id) {
                setSelectedAssignmentId(null);
                setIsEditing(false);
            }
        }
    };

    const handleToggleHour = (hour: number) => {
        const currentHours = formData.rules?.allowedHours || [];
        const newHours = currentHours.includes(hour)
            ? currentHours.filter(h => h !== hour)
            : [...currentHours, hour].sort((a, b) => a - b);
        
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedHours: newHours,
            },
        });
    };

    const handleToggleDay = (day: number) => {
        const currentDays = formData.rules?.allowedDays || [];
        const newDays = currentDays.includes(day)
            ? currentDays.filter(d => d !== day)
            : [...currentDays, day].sort((a, b) => a - b);
        
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedDays: newDays,
            },
        });
    };

    const handleSelectAllHours = () => {
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedHours: HOURS,
            },
        });
    };

    const handleClearHours = () => {
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedHours: [],
            },
        });
    };

    const handleSelectAllDays = () => {
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedDays: DAYS_OF_WEEK.map(d => d.value),
            },
        });
    };

    const handleClearDays = () => {
        setFormData({
            ...formData,
            rules: {
                ...formData.rules!,
                allowedDays: [],
            },
        });
    };

    const filteredAssignments = assignments.filter(a => {
        if (filterClientId && a.clientId !== filterClientId) return false;
        if (searchQuery) {
            const media = mediaLibrary.find(m => m.id === a.mediaId);
            const client = clients.find(c => c.id === a.clientId);
            const searchLower = searchQuery.toLowerCase();
            if (!media?.title.toLowerCase().includes(searchLower) &&
                !client?.name.toLowerCase().includes(searchLower)) {
                return false;
            }
        }
        return true;
    });

    const selectedClient = clients.find(c => c.id === selectedClientId);
    const selectedAssignment = assignments.find(a => a.id === selectedAssignmentId);
    const selectedMedia = selectedAssignment
        ? mediaLibrary.find(m => m.id === selectedAssignment.mediaId)
        : null;

    return (
        <div className={styles.page}>
            <div className={styles.header}>
                <div className={styles.headerLeft}>
                    <button className={styles.backBtn} onClick={handleBack} title="Back to Playout">
                        <ArrowLeft size={18} />
                    </button>
                    <div>
                        <h1>Client Asset Assignment</h1>
                        <p className={styles.subtitle}>Assign media to clients with play rules and scheduling</p>
                    </div>
                </div>
                <div className={styles.headerActions}>
                    <div className={styles.urlDisplay}>
                        <span className={styles.urlLabel}>External URL:</span>
                        <code className={styles.urlCode}>{externalUrl}</code>
                        <button
                            className={styles.btnIcon}
                            onClick={handleCopyUrl}
                            title="Copy URL"
                        >
                            {urlCopied ? <Check size={16} /> : <Copy size={16} />}
                        </button>
                    </div>
                    <button
                        className={styles.btnSecondary}
                        onClick={() => setShowReport(!showReport)}
                        title="View Report"
                    >
                        <FileText size={16} />
                        {showReport ? 'Hide' : 'View'} Report
                    </button>
                    <button
                        className={styles.btnSecondary}
                        onClick={generateReport}
                        title="Download Report"
                    >
                        <Download size={16} />
                        Download
                    </button>
                    <button
                        className={styles.btnSecondary}
                        onClick={() => setFilterClientId(null)}
                        disabled={!filterClientId}
                    >
                        <X size={16} />
                        Clear Filter
                    </button>
                    <button
                        className={styles.btnPrimary}
                        onClick={handleCreateAssignment}
                        disabled={!selectedClientId}
                    >
                        <Plus size={16} />
                        New Assignment
                    </button>
                </div>
            </div>

            {showReport && (
                <div className={styles.reportPanel}>
                    <div className={styles.reportHeader}>
                        <h2>Assignment Report</h2>
                        <button className={styles.btnIcon} onClick={() => setShowReport(false)}>
                            <X size={18} />
                        </button>
                    </div>
                    <div className={styles.reportContent}>
                        <div className={styles.reportSection}>
                            <h3>Summary</h3>
                            <div className={styles.reportStats}>
                                <div className={styles.reportStat}>
                                    <span className={styles.statLabel}>Total Assignments</span>
                                    <span className={styles.statValue}>{assignments.length}</span>
                                </div>
                                <div className={styles.reportStat}>
                                    <span className={styles.statLabel}>Total Clients</span>
                                    <span className={styles.statValue}>{clients.length}</span>
                                </div>
                                <div className={styles.reportStat}>
                                    <span className={styles.statLabel}>Unique Media</span>
                                    <span className={styles.statValue}>
                                        {new Set(assignments.map(a => a.mediaId)).size}
                                    </span>
                                </div>
                            </div>
                        </div>

                        <div className={styles.reportSection}>
                            <h3>Assignments by Client</h3>
                            <div className={styles.reportTable}>
                                {clients.map(client => {
                                    const clientAssignments = assignments.filter(a => a.clientId === client.id);
                                    if (clientAssignments.length === 0) return null;
                                    return (
                                        <div key={client.id} className={styles.reportRow}>
                                            <div className={styles.reportClientName}>
                                                <div
                                                    className={styles.reportColorDot}
                                                    style={{ backgroundColor: client.color }}
                                                />
                                                {client.name}
                                            </div>
                                            <div className={styles.reportCount}>{clientAssignments.length} assignments</div>
                                        </div>
                                    );
                                })}
                            </div>
                        </div>

                        <div className={styles.reportSection}>
                            <h3>Assignments by Day of Week</h3>
                            <div className={styles.reportTable}>
                                {DAYS_OF_WEEK.map(day => {
                                    const count = assignments.filter(a =>
                                        a.rules.allowedDays.length === 0 || a.rules.allowedDays.includes(day.value)
                                    ).length;
                                    return (
                                        <div key={day.value} className={styles.reportRow}>
                                            <div className={styles.reportLabel}>{day.label}</div>
                                            <div className={styles.reportCount}>{count} assignments</div>
                                        </div>
                                    );
                                })}
                            </div>
                        </div>

                        <div className={styles.reportSection}>
                            <h3>Top Time Slots</h3>
                            <div className={styles.reportTable}>
                                {HOURS.map(hour => {
                                    const count = assignments.filter(a =>
                                        a.rules.allowedHours.length === 0 || a.rules.allowedHours.includes(hour)
                                    ).length;
                                    if (count === 0) return null;
                                    return (
                                        <div key={hour} className={styles.reportRow}>
                                            <div className={styles.reportLabel}>
                                                {hour.toString().padStart(2, '0')}:00
                                            </div>
                                            <div className={styles.reportCount}>{count} assignments</div>
                                        </div>
                                    );
                                }).filter(Boolean)}
                            </div>
                        </div>
                    </div>
                </div>
            )}

            <div className={styles.layout}>
                {/* Left: Client List */}
                <div className={styles.sidebar}>
                    <div className={styles.sectionHeader}>
                        <h3>Clients</h3>
                        <button
                            className={styles.btnIcon}
                            onClick={() => {
                                const newClient = {
                                    id: `client_${Date.now()}`,
                                    name: 'New Client',
                                    color: '#3b82f6',
                                    rules: {
                                        allowedHours: [],
                                        allowedDays: [],
                                        maxPlaysPerHour: 0,
                                        maxPlaysPerDay: 0,
                                        minIntervalMinutes: 0,
                                        priority: 0,
                                    },
                                };
                                const updated = [...clients, newClient];
                                setClients(updated);
                                saveClients(updated);
                                setSelectedClientId(newClient.id);
                            }}
                            title="Add Client"
                        >
                            <Plus size={16} />
                        </button>
                    </div>

                    <div className={styles.searchBox}>
                        <Search size={16} />
                        <input
                            type="text"
                            placeholder="Search clients..."
                            value={searchQuery}
                            onChange={(e) => setSearchQuery(e.target.value)}
                        />
                    </div>

                    <div className={styles.clientList}>
                        {clients
                            .filter(c => !searchQuery || c.name.toLowerCase().includes(searchQuery.toLowerCase()))
                            .map(client => (
                                <div
                                    key={client.id}
                                    className={`${styles.clientCard} ${selectedClientId === client.id ? styles.active : ''}`}
                                    onClick={() => {
                                        setSelectedClientId(client.id);
                                        setFilterClientId(client.id);
                                    }}
                                >
                                    <div className={styles.clientColor} style={{ backgroundColor: client.color }} />
                                    <div className={styles.clientInfo}>
                                        <div className={styles.clientName}>{client.name}</div>
                                        <div className={styles.clientMeta}>
                                            {assignments.filter(a => a.clientId === client.id).length} assignments
                                        </div>
                                    </div>
                                </div>
                            ))}
                        {clients.length === 0 && (
                            <div className={styles.empty}>
                                <Users size={48} />
                                <p>No clients defined</p>
                            </div>
                        )}
                    </div>
                </div>

                {/* Center: Assignment List */}
                <div className={styles.mainContent}>
                    <div className={styles.contentHeader}>
                        <h2>
                            {selectedClient ? `${selectedClient.name} - Assignments` : 'All Assignments'}
                        </h2>
                        <div className={styles.stats}>
                            <span>{filteredAssignments.length} assignments</span>
                        </div>
                    </div>

                    <div className={styles.assignmentList}>
                        {filteredAssignments.map(assignment => {
                            const media = mediaLibrary.find(m => m.id === assignment.mediaId);
                            const client = clients.find(c => c.id === assignment.clientId);
                            
                            return (
                                <div
                                    key={assignment.id}
                                    className={`${styles.assignmentCard} ${selectedAssignmentId === assignment.id ? styles.selected : ''}`}
                                    onClick={() => handleSelectAssignment(assignment)}
                                >
                                    <div className={styles.assignmentHeader}>
                                        <div className={styles.assignmentMedia}>
                                            <FileVideo size={20} />
                                            <div>
                                                <div className={styles.assignmentTitle}>
                                                    {media?.title || 'No media selected'}
                                                </div>
                                                <div className={styles.assignmentClient}>
                                                    {client?.name || 'Unknown Client'}
                                                </div>
                                            </div>
                                        </div>
                                        <div className={styles.assignmentActions}>
                                            <button
                                                className={styles.btnIconSmall}
                                                onClick={(e) => {
                                                    e.stopPropagation();
                                                    handleDeleteAssignment(assignment.id);
                                                }}
                                                title="Delete"
                                            >
                                                <Trash size={14} />
                                            </button>
                                        </div>
                                    </div>

                                    <div className={styles.assignmentRules}>
                                        <div className={styles.ruleItem}>
                                            <Clock size={14} />
                                            <span>
                                                {assignment.rules.allowedHours.length === 0
                                                    ? 'All hours'
                                                    : `${assignment.rules.allowedHours.length} hours`}
                                            </span>
                                        </div>
                                        <div className={styles.ruleItem}>
                                            <Calendar size={14} />
                                            <span>
                                                {assignment.rules.allowedDays.length === 0
                                                    ? 'All days'
                                                    : `${assignment.rules.allowedDays.length} days`}
                                            </span>
                                        </div>
                                        <div className={styles.ruleItem}>
                                            <BarChart3 size={14} />
                                            <span>
                                                Max: {assignment.rules.maxPlaysPerHour || '∞'}/hr,{' '}
                                                {assignment.rules.maxPlaysPerDay || '∞'}/day
                                            </span>
                                        </div>
                                        {assignment.rules.minIntervalMinutes > 0 && (
                                            <div className={styles.ruleItem}>
                                                <Settings size={14} />
                                                <span>Min interval: {assignment.rules.minIntervalMinutes} min</span>
                                            </div>
                                        )}
                                    </div>
                                </div>
                            );
                        })}
                        {filteredAssignments.length === 0 && (
                            <div className={styles.empty}>
                                <FileVideo size={48} />
                                <p>No assignments found</p>
                                {!selectedClientId && (
                                    <p className={styles.emptyHint}>Select a client to create an assignment</p>
                                )}
                            </div>
                        )}
                    </div>
                </div>

                {/* Right: Editor Panel */}
                {isEditing && selectedAssignmentId && (
                    <div className={styles.editorPanel}>
                        <div className={styles.editorHeader}>
                            <h3>Edit Assignment</h3>
                            <button
                                className={styles.btnIcon}
                                onClick={() => setIsEditing(false)}
                                title="Close"
                            >
                                <X size={18} />
                            </button>
                        </div>

                        <div className={styles.editorContent}>
                            {/* Media Selection */}
                            <div className={styles.formSection}>
                                <label className={styles.label}>Media Asset</label>
                                <select
                                    className={styles.select}
                                    value={formData.mediaId || ''}
                                    onChange={(e) => {
                                        const media = mediaLibrary.find(m => m.id === e.target.value);
                                        setFormData({
                                            ...formData,
                                            mediaId: e.target.value,
                                            media: media || {} as MediaItem,
                                        });
                                    }}
                                >
                                    <option value="">Select media...</option>
                                    {mediaLibrary
                                        .filter(m => m.type === 'ad' || m.type === 'promo')
                                        .map(media => (
                                            <option key={media.id} value={media.id}>
                                                {media.title} ({Math.round(media.duration)}s)
                                            </option>
                                        ))}
                                </select>
                                {selectedMedia && (
                                    <div className={styles.mediaPreview}>
                                        <FileVideo size={16} />
                                        <span>{selectedMedia.title}</span>
                                        <span className={styles.duration}>{Math.round(selectedMedia.duration)}s</span>
                                    </div>
                                )}
                            </div>

                            {/* Priority */}
                            <div className={styles.formSection}>
                                <label className={styles.label}>
                                    Priority
                                    <span className={styles.hint}>(Higher = more likely to play)</span>
                                </label>
                                <input
                                    type="number"
                                    className={styles.input}
                                    value={formData.priority || 0}
                                    onChange={(e) => setFormData({ ...formData, priority: parseInt(e.target.value) || 0 })}
                                    min="-10"
                                    max="10"
                                />
                            </div>

                            {/* Time of Day */}
                            <div className={styles.formSection}>
                                <div className={styles.sectionHeader}>
                                    <label className={styles.label}>Allowed Hours</label>
                                    <div className={styles.sectionActions}>
                                        <button
                                            className={styles.btnLink}
                                            onClick={handleSelectAllHours}
                                        >
                                            All
                                        </button>
                                        <button
                                            className={styles.btnLink}
                                            onClick={handleClearHours}
                                        >
                                            Clear
                                        </button>
                                    </div>
                                </div>
                                <div className={styles.hourGrid}>
                                    {HOURS.map(hour => (
                                        <button
                                            key={hour}
                                            className={`${styles.hourButton} ${
                                                formData.rules?.allowedHours.includes(hour) ? styles.selected : ''
                                            }`}
                                            onClick={() => handleToggleHour(hour)}
                                        >
                                            {hour.toString().padStart(2, '0')}:00
                                        </button>
                                    ))}
                                </div>
                                {formData.rules?.allowedHours.length === 0 && (
                                    <p className={styles.hint}>No restrictions - can play at any hour</p>
                                )}
                            </div>

                            {/* Day of Week */}
                            <div className={styles.formSection}>
                                <div className={styles.sectionHeader}>
                                    <label className={styles.label}>Allowed Days</label>
                                    <div className={styles.sectionActions}>
                                        <button
                                            className={styles.btnLink}
                                            onClick={handleSelectAllDays}
                                        >
                                            All
                                        </button>
                                        <button
                                            className={styles.btnLink}
                                            onClick={handleClearDays}
                                        >
                                            Clear
                                        </button>
                                    </div>
                                </div>
                                <div className={styles.dayGrid}>
                                    {DAYS_OF_WEEK.map(day => (
                                        <button
                                            key={day.value}
                                            className={`${styles.dayButton} ${
                                                formData.rules?.allowedDays.includes(day.value) ? styles.selected : ''
                                            }`}
                                            onClick={() => handleToggleDay(day.value)}
                                        >
                                            {day.short}
                                        </button>
                                    ))}
                                </div>
                                {formData.rules?.allowedDays.length === 0 && (
                                    <p className={styles.hint}>No restrictions - can play any day</p>
                                )}
                            </div>

                            {/* Frequency Limits */}
                            <div className={styles.formSection}>
                                <label className={styles.label}>Frequency Limits</label>
                                <div className={styles.frequencyGrid}>
                                    <div className={styles.frequencyField}>
                                        <label>Max per Hour</label>
                                        <input
                                            type="number"
                                            className={styles.input}
                                            value={formData.rules?.maxPlaysPerHour || 0}
                                            onChange={(e) => setFormData({
                                                ...formData,
                                                rules: {
                                                    ...formData.rules!,
                                                    maxPlaysPerHour: parseInt(e.target.value) || 0,
                                                },
                                            })}
                                            min="0"
                                            placeholder="0 = unlimited"
                                        />
                                    </div>
                                    <div className={styles.frequencyField}>
                                        <label>Max per Day</label>
                                        <input
                                            type="number"
                                            className={styles.input}
                                            value={formData.rules?.maxPlaysPerDay || 0}
                                            onChange={(e) => setFormData({
                                                ...formData,
                                                rules: {
                                                    ...formData.rules!,
                                                    maxPlaysPerDay: parseInt(e.target.value) || 0,
                                                },
                                            })}
                                            min="0"
                                            placeholder="0 = unlimited"
                                        />
                                    </div>
                                </div>
                            </div>

                            {/* Minimum Interval */}
                            <div className={styles.formSection}>
                                <label className={styles.label}>
                                    Minimum Interval (minutes)
                                    <span className={styles.hint}>Minimum time between plays</span>
                                </label>
                                <input
                                    type="number"
                                    className={styles.input}
                                    value={formData.rules?.minIntervalMinutes || 0}
                                    onChange={(e) => setFormData({
                                        ...formData,
                                        rules: {
                                            ...formData.rules!,
                                            minIntervalMinutes: parseInt(e.target.value) || 0,
                                        },
                                    })}
                                    min="0"
                                />
                            </div>

                            {/* Notes */}
                            <div className={styles.formSection}>
                                <label className={styles.label}>Notes</label>
                                <textarea
                                    className={styles.textarea}
                                    value={formData.notes || ''}
                                    onChange={(e) => setFormData({ ...formData, notes: e.target.value })}
                                    rows={3}
                                    placeholder="Additional notes about this assignment..."
                                />
                            </div>

                            <div className={styles.editorActions}>
                                <button
                                    className={styles.btnPrimary}
                                    onClick={handleSaveAssignment}
                                >
                                    <Save size={16} />
                                    Save Assignment
                                </button>
                                <button
                                    className={styles.btnSecondary}
                                    onClick={() => setIsEditing(false)}
                                >
                                    Cancel
                                </button>
                            </div>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

