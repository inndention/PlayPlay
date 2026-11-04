/**
 * Playout Engines Page
 * External playout engines have been removed - using built-in engine only
 */

import { useNavigate } from 'react-router-dom';
import { X, Info } from 'lucide-react';
import styles from './PlayoutEnginesPage.module.css';

export function PlayoutEnginesPage() {
    const navigate = useNavigate();
    
    const handleClose = () => {
        // Navigate back to main playout view
        navigate('/playout');
        // Also update hash for compatibility
        window.location.hash = '#/playout';
    };
    
    return (
        <div className={styles.page}>
            <div className={styles.header}>
                <h2>Playout Engine</h2>
                <button className={styles.closeBtn} onClick={handleClose} title="Close">
                    <X size={20} />
                </button>
            </div>
            <div className={styles.content}>
                <div style={{ 
                    display: 'flex', 
                    flexDirection: 'column', 
                    alignItems: 'center', 
                    justifyContent: 'center',
                    height: '100%',
                    gap: '1rem',
                    color: '#94a3b8'
                }}>
                    <Info size={48} strokeWidth={1} />
                    <h3 style={{ margin: 0, color: '#e2e8f0' }}>Built-in Engine Active</h3>
                    <p style={{ margin: 0, textAlign: 'center', maxWidth: '400px' }}>
                        Play Play uses its built-in HTML5 video engine for playback.
                        External playout engines (CasperCG, FFmpeg, MPV, etc.) have been removed for simplicity.
                    </p>
                    <button
                        onClick={handleClose}
                        style={{
                            marginTop: '1rem',
                            padding: '0.75rem 1.5rem',
                            background: 'linear-gradient(135deg, #3b82f6 0%, #1d4ed8 100%)',
                            border: 'none',
                            borderRadius: '8px',
                            color: '#ffffff',
                            cursor: 'pointer',
                            fontSize: '0.9rem',
                        }}
                    >
                        Back to Playout
                    </button>
                </div>
            </div>
        </div>
    );
}

export default PlayoutEnginesPage;
