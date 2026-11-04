import React, { useState, useEffect } from 'react';
import styles from './ShowArchivePanel.module.css';
import { Archive, Search, Download, Play, Trash2 } from 'lucide-react';
import { showArchiveService, ShowArchive } from '../lib/showArchive';
import { useToast } from './Toast';

export function ShowArchivePanel() {
    const { showToast } = useToast();
    const [archives, setArchives] = useState<ShowArchive[]>([]);
    const [selectedArchive, setSelectedArchive] = useState<ShowArchive | null>(null);
    const [searchQuery, setSearchQuery] = useState('');

    useEffect(() => {
        loadArchives();
    }, []);

    const loadArchives = async () => {
        const all = await showArchiveService.getAllArchives();
        setArchives(all);
    };

    const handleSearch = async () => {
        if (!searchQuery) {
            loadArchives();
            return;
        }
        const results = await showArchiveService.searchArchives(searchQuery);
        setArchives(results);
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <Archive size={20} />
                <h2>Show Archive</h2>
            </div>
            <div className={styles.content}>
                <div className={styles.search}>
                    <input
                        type="text"
                        placeholder="Search transcripts..."
                        value={searchQuery}
                        onChange={(e) => setSearchQuery(e.target.value)}
                        onKeyPress={(e) => e.key === 'Enter' && handleSearch()}
                    />
                    <button onClick={handleSearch}><Search size={16} /></button>
                </div>
                <div className={styles.archiveList}>
                    {archives.map(archive => (
                        <div
                            key={archive.id}
                            className={`${styles.archiveItem} ${selectedArchive?.id === archive.id ? styles.selected : ''}`}
                            onClick={() => setSelectedArchive(archive)}
                        >
                            <div className={styles.archiveHeader}>
                                <span>{new Date(archive.startTime).toLocaleString()}</span>
                                <span>{Math.floor(archive.duration / 60)}m {Math.round(archive.duration % 60)}s</span>
                            </div>
                            {archive.transcript && (
                                <div className={styles.archivePreview}>
                                    {archive.transcript.text.substring(0, 100)}...
                                </div>
                            )}
                        </div>
                    ))}
                </div>
                {selectedArchive && (
                    <div className={styles.archiveViewer}>
                        <h3>Transcript</h3>
                        <div className={styles.transcript}>
                            {selectedArchive.transcript?.text || 'No transcript available'}
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

