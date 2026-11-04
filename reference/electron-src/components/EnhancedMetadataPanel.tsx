import React, { useState, useEffect } from 'react';
import styles from './EnhancedMetadataPanel.module.css';
import { 
    X, Film, Image, Volume2, Clock, Calendar, HardDrive, Monitor, 
    Palette, Tag, Edit2, Save, FileText, Layers, Activity, Cpu,
    Copy, Check, ChevronDown, ChevronUp, Star, Bookmark
} from 'lucide-react';
import { MediaItem } from '../lib/types';
import { formatDuration, formatFileSize } from '../lib/utils';

interface EnhancedMetadataPanelProps {
    item: MediaItem;
    onClose: () => void;
    onUpdate?: (item: MediaItem) => void;
    onToggleFavorite?: (itemId: string) => void;
    isFavorite?: boolean;
}

interface MetadataSection {
    id: string;
    title: string;
    icon: React.ElementType;
    fields: { label: string; value: string | number | undefined; copyable?: boolean }[];
}

export const EnhancedMetadataPanel: React.FC<EnhancedMetadataPanelProps> = ({
    item,
    onClose,
    onUpdate,
    onToggleFavorite,
    isFavorite = false
}) => {
    const [isEditing, setIsEditing] = useState(false);
    const [editedItem, setEditedItem] = useState<MediaItem>(item);
    const [expandedSections, setExpandedSections] = useState<Set<string>>(new Set(['general', 'video', 'audio']));
    const [copiedField, setCopiedField] = useState<string | null>(null);
    const [customTags, setCustomTags] = useState<string[]>([]);
    const [newTag, setNewTag] = useState('');

    useEffect(() => {
        setEditedItem(item);
        setCustomTags([]); // Tags not supported in MediaItem
    }, [item]);

    const toggleSection = (sectionId: string) => {
        setExpandedSections(prev => {
            const next = new Set(prev);
            if (next.has(sectionId)) {
                next.delete(sectionId);
            } else {
                next.add(sectionId);
            }
            return next;
        });
    };

    const copyToClipboard = async (value: string, fieldId: string) => {
        try {
            await navigator.clipboard.writeText(value);
            setCopiedField(fieldId);
            setTimeout(() => setCopiedField(null), 2000);
        } catch (err) {
            console.error('Failed to copy:', err);
        }
    };

    const handleSave = () => {
        if (onUpdate) {
            onUpdate({ ...editedItem });
        }
        setIsEditing(false);
    };

    const addTag = () => {
        if (newTag.trim() && !customTags.includes(newTag.trim())) {
            setCustomTags(prev => [...prev, newTag.trim()]);
            setNewTag('');
        }
    };

    const removeTag = (tag: string) => {
        setCustomTags(prev => prev.filter(t => t !== tag));
    };

    const getTypeIcon = () => {
        switch (item.type) {
            case 'video': return <Film size={20} />;
            case 'image': return <Image size={20} />;
            // case 'audio': return <Volume2 size={20} />; // Audio type not in MediaType
            default: return <FileText size={20} />;
        }
    };

    // Build sections based on media type
    const sections: MetadataSection[] = [
        {
            id: 'general',
            title: 'General Information',
            icon: FileText,
            fields: [
                { label: 'File Name', value: item.filename || item.title, copyable: true },
                { label: 'File Path', value: item.filePath, copyable: true },
                { label: 'Type', value: item.type?.charAt(0).toUpperCase() + item.type?.slice(1) },
                // File size would come from stats if available
                { label: 'Duration', value: item.duration ? formatDuration(item.duration) : undefined },
                { label: 'Date Added', value: item.addedAt ? new Date(item.addedAt).toLocaleString() : undefined },
                { label: 'Date Made', value: item.dateMade ? new Date(item.dateMade).toLocaleString() : undefined }
            ].filter(f => f.value !== undefined)
        },
        {
            id: 'video',
            title: 'Video Properties',
            icon: Monitor,
            fields: [
                // Technical metadata - stats structure may vary
                { label: 'Resolution', value: (item.stats as any)?.resolution },
                { label: 'Frame Rate', value: (item.stats as any)?.frameRate ? `${(item.stats as any).frameRate} fps` : undefined },
                { label: 'Video Codec', value: (item.stats as any)?.videoCodec },
                { label: 'Video Bitrate', value: (item.stats as any)?.videoBitrate ? `${Math.round((item.stats as any).videoBitrate / 1000)} kbps` : undefined },
                { label: 'Aspect Ratio', value: (item.stats as any)?.aspectRatio },
                { label: 'Scan Type', value: (item.stats as any)?.scanType },
                { label: 'Color Space', value: (item.stats as any)?.colorSpace },
                { label: 'Bit Depth', value: (item.stats as any)?.bitDepth ? `${(item.stats as any).bitDepth}-bit` : undefined }
            ].filter(f => f.value !== undefined)
        },
        {
            id: 'audio',
            title: 'Audio Properties',
            icon: Volume2,
            fields: [
                { label: 'Audio Codec', value: (item.stats as any)?.audioCodec },
                { label: 'Audio Channels', value: (item.stats as any)?.audioChannels ? `${(item.stats as any).audioChannels} channels` : undefined },
                { label: 'Sample Rate', value: (item.stats as any)?.sampleRate ? `${(item.stats as any).sampleRate} Hz` : undefined },
                { label: 'Audio Bitrate', value: (item.stats as any)?.audioBitrate ? `${Math.round((item.stats as any).audioBitrate / 1000)} kbps` : undefined },
                { label: 'Audio Language', value: (item.stats as any)?.audioLanguage }
            ].filter(f => f.value !== undefined)
        },
        {
            id: 'broadcast',
            title: 'Broadcast Info',
            icon: Activity,
            fields: [
                { label: 'Loudness (LUFS)', value: (item.stats as any)?.loudnessLUFS ? `${(item.stats as any).loudnessLUFS.toFixed(1)} LUFS` : undefined },
                { label: 'True Peak', value: (item.stats as any)?.truePeak ? `${(item.stats as any).truePeak.toFixed(1)} dBTP` : undefined },
                { label: 'Mark In', value: item.inPoint !== undefined ? formatDuration(item.inPoint) : undefined },
                { label: 'Mark Out', value: item.outPoint !== undefined ? formatDuration(item.outPoint) : undefined },
                { label: 'Effective Duration', value: item.inPoint !== undefined && item.outPoint !== undefined
                    ? formatDuration(item.outPoint - item.inPoint) : undefined },
                { label: 'Has Segments', value: item.segments?.length ? `${item.segments.length} segment(s)` : 'No' }
            ].filter(f => f.value !== undefined)
        },
        {
            id: 'technical',
            title: 'Technical Details',
            icon: Cpu,
            fields: [
                { label: 'Container Format', value: (item.stats as any)?.containerFormat },
                { label: 'Stream Count', value: (item.stats as any)?.streamCount?.toString() },
                { label: 'Encoder', value: (item.stats as any)?.encoder },
                { label: 'Writing App', value: (item.stats as any)?.writingApp },
                { label: 'File Hash', value: (item.stats as any)?.fileHash, copyable: true }
            ].filter(f => f.value !== undefined)
        }
    ].filter(section => section.fields.length > 0);

    return (
        <div className={styles.overlay} onClick={onClose}>
            <div className={styles.panel} onClick={e => e.stopPropagation()}>
                {/* Header */}
                <div className={styles.header}>
                    <div className={styles.headerTitle}>
                        {getTypeIcon()}
                        <span>Media Properties</span>
                    </div>
                    <div className={styles.headerActions}>
                        {onToggleFavorite && (
                            <button 
                                className={`${styles.iconBtn} ${isFavorite ? styles.favorite : ''}`}
                                onClick={() => onToggleFavorite(item.id)}
                                title={isFavorite ? 'Remove from favorites' : 'Add to favorites'}
                            >
                                <Star size={18} fill={isFavorite ? 'currentColor' : 'none'} />
                            </button>
                        )}
                        {onUpdate && (
                            <button 
                                className={styles.iconBtn}
                                onClick={() => isEditing ? handleSave() : setIsEditing(true)}
                                title={isEditing ? 'Save changes' : 'Edit metadata'}
                            >
                                {isEditing ? <Save size={18} /> : <Edit2 size={18} />}
                            </button>
                        )}
                        <button className={styles.closeBtn} onClick={onClose}>
                            <X size={20} />
                        </button>
                    </div>
                </div>

                {/* Thumbnail Preview */}
                <div className={styles.preview}>
                    {item.thumbnail ? (
                        <img src={item.thumbnail} alt={item.title} />
                    ) : (
                        <div className={styles.placeholderPreview}>
                            {getTypeIcon()}
                        </div>
                    )}
                    <div className={styles.previewOverlay}>
                        <span className={styles.mediaName}>{item.title}</span>
                        {item.duration && (
                            <span className={styles.mediaDuration}>
                                <Clock size={12} />
                                {formatDuration(item.duration)}
                            </span>
                        )}
                    </div>
                </div>

                {/* Tags Section */}
                <div className={styles.tagsSection}>
                    <div className={styles.tagsHeader}>
                        <Tag size={14} />
                        <span>Tags</span>
                    </div>
                    <div className={styles.tags}>
                        {customTags.map(tag => (
                            <span key={tag} className={styles.tag}>
                                {tag}
                                {isEditing && (
                                    <button onClick={() => removeTag(tag)}>×</button>
                                )}
                            </span>
                        ))}
                        {customTags.length === 0 && !isEditing && (
                            <span className={styles.noTags}>No tags</span>
                        )}
                    </div>
                    {isEditing && (
                        <div className={styles.addTag}>
                            <input
                                type="text"
                                placeholder="Add tag..."
                                value={newTag}
                                onChange={e => setNewTag(e.target.value)}
                                onKeyDown={e => {
                                    if (e.key === 'Enter') addTag();
                                }}
                            />
                            <button onClick={addTag} disabled={!newTag.trim()}>Add</button>
                        </div>
                    )}
                </div>

                {/* Description (editable) */}
                <div className={styles.descriptionSection}>
                    <label>Description</label>
                    {isEditing ? (
                        <textarea
                            value={editedItem.notes || ''}
                            onChange={e => setEditedItem(prev => ({ ...prev, description: e.target.value }))}
                            placeholder="Add a description..."
                            rows={3}
                        />
                    ) : (
                        <p>{item.notes || 'No notes'}</p>
                    )}
                </div>

                {/* Metadata Sections */}
                <div className={styles.sections}>
                    {sections.map(section => (
                        <div key={section.id} className={styles.section}>
                            <button 
                                className={styles.sectionHeader}
                                onClick={() => toggleSection(section.id)}
                            >
                                <section.icon size={14} />
                                <span>{section.title}</span>
                                <span className={styles.fieldCount}>{section.fields.length}</span>
                                {expandedSections.has(section.id) ? (
                                    <ChevronUp size={16} />
                                ) : (
                                    <ChevronDown size={16} />
                                )}
                            </button>
                            {expandedSections.has(section.id) && (
                                <div className={styles.sectionContent}>
                                    {section.fields.map((field, idx) => (
                                        <div key={idx} className={styles.field}>
                                            <span className={styles.fieldLabel}>{field.label}</span>
                                            <span className={styles.fieldValue}>
                                                {String(field.value)}
                                                {field.copyable && (
                                                    <button 
                                                        className={styles.copyBtn}
                                                        onClick={() => copyToClipboard(String(field.value), `${section.id}-${idx}`)}
                                                        title="Copy to clipboard"
                                                    >
                                                        {copiedField === `${section.id}-${idx}` ? (
                                                            <Check size={12} />
                                                        ) : (
                                                            <Copy size={12} />
                                                        )}
                                                    </button>
                                                )}
                                            </span>
                                        </div>
                                    ))}
                                </div>
                            )}
                        </div>
                    ))}
                </div>

                {/* Footer */}
                {isEditing && (
                    <div className={styles.footer}>
                        <button className={styles.cancelBtn} onClick={() => {
                            setIsEditing(false);
                            setEditedItem(item);
                            setCustomTags([]); // Tags not supported in MediaItem
                        }}>
                            Cancel
                        </button>
                        <button className={styles.saveBtn} onClick={handleSave}>
                            <Save size={14} />
                            Save Changes
                        </button>
                    </div>
                )}
            </div>
        </div>
    );
};

export default EnhancedMetadataPanel;

