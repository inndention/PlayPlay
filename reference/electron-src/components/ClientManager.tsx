import React, { useState, useEffect, useMemo } from 'react';
import { AdClient, MediaItem } from '../lib/types';
import { loadClients, saveClients, createClient } from '../lib/storage';
import { dbFetchMedia } from '../lib/mediaDbClient';
import { Users, Plus, Save, Trash, Search, Mail, Phone, Globe, MapPin, Calendar, DollarSign, Clock } from 'lucide-react';
import styles from './ClientManager.module.css';

const PRESET_COLORS = ['#ef4444', '#f97316', '#eab308', '#22c55e', '#06b6d4', '#3b82f6', '#8b5cf6', '#ec4899', '#64748b'];

export function ClientManager() {
    const [clients, setClients] = useState<AdClient[]>([]);
    const [selectedClientId, setSelectedClientId] = useState<string | null>(null);
    const [isEditing, setIsEditing] = useState(false);
    const [formData, setFormData] = useState<Partial<AdClient>>({});
    const [searchQuery, setSearchQuery] = useState('' );
    const [mediaLibrary, setMediaLibrary] = useState<MediaItem[]>([]);

    useEffect(() => {
        setClients(loadClients());
        // Load media for assigned-assets summary (Electron DB if available)
        dbFetchMedia().then(setMediaLibrary).catch(() => setMediaLibrary([]));
    }, []);

    const filteredClients = useMemo(() => {
        const q = (searchQuery || '').trim().toLowerCase();
        if (!q) return clients;
        return clients.filter(c => (
            c.name.toLowerCase().includes(q) ||
            (c.contactEmail || '').toLowerCase().includes(q) ||
            (c.contactName || '').toLowerCase().includes(q) ||
            (c.billingCode || '').toLowerCase().includes(q)
        ));
    }, [clients, searchQuery]);

    const handleSaveClients = (newClients: AdClient[]) => {
        setClients(newClients);
        saveClients(newClients);
    };

    const handleCreate = () => {
        const newClient = createClient('New Client');
        const updated = [...clients, newClient];
        handleSaveClients(updated);
        setSelectedClientId(newClient.id);
        setFormData(newClient);
        setIsEditing(true);
    };

    const handleDelete = (id: string) => {
        if (confirm('Delete this client? This will remove them from all assigned media.')) {
            const updated = clients.filter(c => c.id !== id);
            handleSaveClients(updated);
            if (selectedClientId === id) {
                setSelectedClientId(null);
                setIsEditing(false);
            }
        }
    };

    const handleSaveForm = () => {
        if (!selectedClientId) return;

        const updated = clients.map(c =>
            c.id === selectedClientId
                ? { ...c, ...formData } as AdClient
                : c
        );
        handleSaveClients(updated);
        setIsEditing(false);
    };

    const handleSelect = (client: AdClient) => {
        setSelectedClientId(client.id);
        setFormData(client);
        setIsEditing(true);
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>
                    <Users size={20} />
                    <span>Client Manager</span>
                </div>
                <button className={`btn btn-primary ${styles.btn} ${styles.btnPrimary}`} onClick={handleCreate} style={{ width: 'auto', padding: '0.4rem 1rem' }}>
                    <Plus size={16} /> New Client
                </button>
            </div>

            <div className={styles.content}>
                <div className={styles.clientList}>
                    <div className={styles.searchRow}>
                        <Search size={14} />
                        <input
                            className={styles.searchInput}
                            value={searchQuery}
                            onChange={e => setSearchQuery(e.target.value)}
                            placeholder="Search clients, contacts, billing code..."
                        />
                    </div>
                    {clients.length === 0 && (
                        <div className={styles.emptyState}>
                            No clients defined. Create one to get started.
                        </div>
                    )}
                    {filteredClients.map(client => (
                        <div
                            key={client.id}
                            className={`${styles.clientCard} ${selectedClientId === client.id ? styles.active : ''}`}
                            onClick={() => handleSelect(client)}
                        >
                            <div className={styles.clientInfo}>
                                <div className={styles.clientColor} style={{ backgroundColor: client.color }} />
                                <div>
                                    <div className={styles.clientName}>{client.name}</div>
                                    <div className={styles.clientMeta}>{client.contactEmail || 'No contact info'}</div>
                                </div>
                            </div>
                            <button className={styles.btnDanger} onClick={(e) => { e.stopPropagation(); handleDelete(client.id); }} style={{ padding: '0.3rem' }}>
                                <Trash size={14} />
                            </button>
                        </div>
                    ))}
                </div>

                {selectedClientId && isEditing && (
                    <div className={styles.editorPanel}>
                        <div className={styles.editorTitle}>Edit Client</div>

                        <div className={styles.formGroup}>
                            <label className={styles.label}>Name</label>
                            <input
                                className={styles.input}
                                value={formData.name || ''}
                                onChange={e => setFormData({ ...formData, name: e.target.value })}
                            />
                        </div>

                        <div className={styles.formGroup}>
                            <label className={styles.label}><Mail size={14} /> Email</label>
                            <input
                                className={styles.input}
                                value={formData.contactEmail || ''}
                                onChange={e => setFormData({ ...formData, contactEmail: e.target.value })}
                                placeholder="contact@example.com"
                            />
                        </div>

                        <div className={styles.formGroup}>
                            <label className={styles.label}>Color Key</label>
                            <div className={styles.colorGrid}>
                                {PRESET_COLORS.map(color => (
                                    <div
                                        key={color}
                                        className={`${styles.colorOption} ${formData.color === color ? styles.selected : ''}`}
                                        style={{ backgroundColor: color }}
                                        onClick={() => setFormData({ ...formData, color })}
                                    />
                                ))}
                            </div>
                        </div>

                        <div className={styles.formGroup}>
                            <label className={styles.label}>Notes</label>
                            <textarea
                                className={styles.textArea}
                                value={formData.notes || ''}
                                onChange={e => setFormData({ ...formData, notes: e.target.value })}
                            />
                        </div>

                        <div className={styles.actions}>
                            <button className={`${styles.btn} ${styles.btnPrimary}`} onClick={handleSaveForm}>
                                <Save size={14} /> Save Changes
                            </button>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}
