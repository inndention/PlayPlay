// ============================================
// Browser File Storage Utilities
// IndexedDB and File System Access API for browser mode
// ============================================

interface FileData {
    id: string;
    file: File;
    blob: Blob;
    url: string;
    metadata: {
        name: string;
        type: string;
        size: number;
        lastModified: number;
    };
}

const DB_NAME = 'playplay_files';
const DB_VERSION = 1;
const STORE_NAME = 'files';

// Initialize IndexedDB
async function initDB(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(DB_NAME, DB_VERSION);
        
        request.onerror = () => reject(request.error);
        request.onsuccess = () => resolve(request.result);
        
        request.onupgradeneeded = (event) => {
            const db = (event.target as IDBOpenDBRequest).result;
            if (!db.objectStoreNames.contains(STORE_NAME)) {
                const store = db.createObjectStore(STORE_NAME, { keyPath: 'id' });
                store.createIndex('name', 'metadata.name', { unique: false });
            }
        };
    });
}

// Store file in IndexedDB
export async function storeFileInIndexedDB(file: File, fileId: string): Promise<string> {
    const db = await initDB();
    
    return new Promise((resolve, reject) => {
        const transaction = db.transaction([STORE_NAME], 'readwrite');
        const store = transaction.objectStore(STORE_NAME);
        
        // Convert File to Blob for storage
        const reader = new FileReader();
        reader.onload = async (e) => {
            const arrayBuffer = e.target?.result as ArrayBuffer;
            const blob = new Blob([arrayBuffer], { type: file.type });
            
            const fileData: FileData = {
                id: fileId,
                file: file,
                blob: blob,
                url: '', // Will be set when retrieved
                metadata: {
                    name: file.name,
                    type: file.type,
                    size: file.size,
                    lastModified: file.lastModified,
                },
            };
            
            const request = store.put(fileData);
            request.onsuccess = () => {
                console.log('File stored in IndexedDB:', fileId, file.name);
                resolve(fileId);
            };
            request.onerror = () => reject(request.error);
        };
        reader.onerror = () => reject(reader.error);
        reader.readAsArrayBuffer(file);
    });
}

// Retrieve file from IndexedDB and create blob URL
export async function getFileFromIndexedDB(fileId: string): Promise<string | null> {
    const db = await initDB();
    
    return new Promise((resolve, reject) => {
        const transaction = db.transaction([STORE_NAME], 'readonly');
        const store = transaction.objectStore(STORE_NAME);
        const request = store.get(fileId);
        
        request.onsuccess = () => {
            const fileData = request.result as FileData | undefined;
            if (!fileData) {
                resolve(null);
                return;
            }
            
            // Create blob URL from stored blob
            const url = URL.createObjectURL(fileData.blob);
            resolve(url);
        };
        
        request.onerror = () => reject(request.error);
    });
}

// Delete file from IndexedDB
export async function deleteFileFromIndexedDB(fileId: string): Promise<void> {
    const db = await initDB();
    
    return new Promise((resolve, reject) => {
        const transaction = db.transaction([STORE_NAME], 'readwrite');
        const store = transaction.objectStore(STORE_NAME);
        const request = store.delete(fileId);
        
        request.onsuccess = () => resolve();
        request.onerror = () => reject(request.error);
    });
}

// Check if File System Access API is available
export function isFileSystemAccessAvailable(): boolean {
    return 'showOpenFilePicker' in window && 'showSaveFilePicker' in window;
}

// Use File System Access API to get file handle
export async function getFileHandle(): Promise<FileSystemFileHandle | null> {
    if (!isFileSystemAccessAvailable()) {
        return null;
    }
    
    try {
        const [handle] = await (window as any).showOpenFilePicker({
            types: [{
                description: 'Media Files',
                accept: {
                    'video/*': ['.mp4', '.webm', '.mov', '.avi', '.mkv'],
                    'image/*': ['.jpg', '.jpeg', '.png', '.gif', '.webp'],
                },
            }],
            multiple: false,
        });
        
        return handle;
    } catch (err: any) {
        // User cancelled or error
        if (err.name !== 'AbortError') {
            console.error('File System Access API error:', err);
        }
        return null;
    }
}

// Get file from File System Access API handle
export async function getFileFromHandle(handle: FileSystemFileHandle): Promise<File> {
    return await handle.getFile();
}

// Store file handle permission (for File System Access API)
export async function requestFileHandlePermission(handle: FileSystemFileHandle): Promise<boolean> {
    // Type guard for File System Access API methods
    if ('queryPermission' in handle && typeof (handle as any).queryPermission === 'function') {
        const permission = await (handle as any).queryPermission({ mode: 'read' });
        if (permission === 'granted') {
            return true;
        }
        if (permission === 'prompt' && 'requestPermission' in handle && typeof (handle as any).requestPermission === 'function') {
            return (await (handle as any).requestPermission({ mode: 'read' })) === 'granted';
        }
    }
    // If methods don't exist, assume permission is granted (older API versions)
    return true;
}

// Get all stored file IDs from IndexedDB
export async function getAllStoredFileIds(): Promise<string[]> {
    const db = await initDB();
    
    return new Promise((resolve, reject) => {
        const transaction = db.transaction([STORE_NAME], 'readonly');
        const store = transaction.objectStore(STORE_NAME);
        const request = store.getAllKeys();
        
        request.onsuccess = () => {
            resolve(request.result as string[]);
        };
        
        request.onerror = () => reject(request.error);
    });
}

