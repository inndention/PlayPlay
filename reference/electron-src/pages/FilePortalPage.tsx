// ============================================
// File Portal Page
// Guest file access portal
// ============================================

import React, { useState, useEffect } from 'react';
import { MediaLibrary } from '../components/MediaLibrary';
import { fileSharingPortal } from '../lib/fileSharingPortal';
import { Upload, Download, FolderOpen, File } from 'lucide-react';
import styles from './FilePortalPage.module.css';

export function FilePortalPage() {
    const [uploading, setUploading] = useState(false);
    const [uploadProgress, setUploadProgress] = useState(0);
    const [canUpload, setCanUpload] = useState(false);
    const [canDownload, setCanDownload] = useState(false);

    useEffect(() => {
        const config = fileSharingPortal.getConfig();
        setCanUpload(config.allowUpload);
        setCanDownload(config.allowDownload);
    }, []);

    const handleFileUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
        const file = e.target.files?.[0];
        if (!file) return;

        const check = fileSharingPortal.canUpload(file.size);
        if (!check.allowed) {
            alert(check.error);
            return;
        }

        setUploading(true);
        setUploadProgress(0);

        try {
            const api = (window as any).electronAPI;
            if (api?.uploadFile) {
                // Simulate progress
                const interval = setInterval(() => {
                    setUploadProgress(prev => {
                        if (prev >= 90) {
                            clearInterval(interval);
                            return 90;
                        }
                        return prev + 10;
                    });
                }, 200);

                await api.uploadFile(file, {
                    public: true,
                    folder: '/public',
                });

                clearInterval(interval);
                setUploadProgress(100);
                
                setTimeout(() => {
                    setUploading(false);
                    setUploadProgress(0);
                    // Reload media library
                    window.location.reload();
                }, 1000);
            } else {
                alert('File upload not available in this environment');
                setUploading(false);
            }
        } catch (error) {
            console.error('[FilePortal] Upload failed:', error);
            alert('Upload failed: ' + (error instanceof Error ? error.message : String(error)));
            setUploading(false);
            setUploadProgress(0);
        }
    };

    return (
        <div className={styles.container}>
            <div className={styles.header}>
                <div className={styles.title}>
                    <FolderOpen size={24} />
                    <h1>File Sharing Portal</h1>
                </div>
                <div className={styles.actions}>
                    {canUpload && (
                        <label className={styles.uploadButton}>
                            <Upload size={16} />
                            Upload File
                            <input
                                type="file"
                                style={{ display: 'none' }}
                                onChange={handleFileUpload}
                                disabled={uploading}
                            />
                        </label>
                    )}
                </div>
            </div>

            {uploading && (
                <div className={styles.uploadProgress}>
                    <div className={styles.progressBar}>
                        <div 
                            className={styles.progressFill}
                            style={{ width: `${uploadProgress}%` }}
                        />
                    </div>
                    <span>Uploading... {uploadProgress}%</span>
                </div>
            )}

            <div className={styles.content}>
                <MediaLibrary 
                    guestMode={true}
                    onItemClick={(item) => {
                        if (canDownload && item.filePath) {
                            // Trigger download
                            const api = (window as any).electronAPI;
                            if (api?.downloadFile) {
                                api.downloadFile(item.filePath, item.filename || item.title);
                            }
                        }
                    }}
                />
            </div>
        </div>
    );
}



