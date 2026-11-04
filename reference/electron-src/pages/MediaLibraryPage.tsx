import React from 'react';
import { MediaLibrary } from '../components/MediaLibrary';

export function MediaLibraryPage() {
    return (
        <div style={{ width: '100vw', height: '100vh', overflow: 'hidden', background: '#09090b' }}>
            <MediaLibrary />
        </div>
    );
}
