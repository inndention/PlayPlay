import React, { useState, useEffect, useRef } from 'react';
import { HelpTopic, HELP_CATEGORIES, searchHelp, getTopicsByCategory, getHelpTopic } from '../lib/helpIndex';
import { Search, X, ChevronRight, BookOpen, ExternalLink } from 'lucide-react';
import styles from './HelpPanel.module.css';

interface HelpPanelProps {
    isOpen: boolean;
    onClose: () => void;
    initialTopic?: string;
}

export function HelpPanel({ isOpen, onClose, initialTopic }: HelpPanelProps) {
    const [searchQuery, setSearchQuery] = useState('');
    const [selectedTopic, setSelectedTopic] = useState<HelpTopic | null>(null);
    const [selectedCategory, setSelectedCategory] = useState<string | null>(null);
    const searchInputRef = useRef<HTMLInputElement>(null);

    // Handle initial topic
    useEffect(() => {
        if (initialTopic) {
            const topic = getHelpTopic(initialTopic);
            if (topic) {
                setSelectedTopic(topic);
                setSelectedCategory(topic.category);
            }
        }
    }, [initialTopic]);

    // Focus search on open
    useEffect(() => {
        if (isOpen && searchInputRef.current) {
            setTimeout(() => searchInputRef.current?.focus(), 100);
        }
    }, [isOpen]);

    // Handle escape key
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (e.key === 'Escape') {
                if (selectedTopic) {
                    setSelectedTopic(null);
                } else {
                    onClose();
                }
            }
        };
        if (isOpen) {
            window.addEventListener('keydown', handleKeyDown);
            return () => window.removeEventListener('keydown', handleKeyDown);
        }
    }, [isOpen, selectedTopic, onClose]);

    if (!isOpen) return null;

    const searchResults = searchQuery ? searchHelp(searchQuery) : null;
    const categoryTopics = selectedCategory ? getTopicsByCategory(selectedCategory) : null;

    const handleTopicClick = (topic: HelpTopic) => {
        setSelectedTopic(topic);
        setSelectedCategory(topic.category);
        setSearchQuery('');
    };

    const handleBack = () => {
        if (selectedTopic) {
            setSelectedTopic(null);
        } else if (selectedCategory) {
            setSelectedCategory(null);
        }
    };

    const renderMarkdown = (content: string) => {
        // Simple markdown rendering
        return content
            .split('\n')
            .map((line, i) => {
                // Headers
                if (line.startsWith('**') && line.endsWith('**')) {
                    return <h4 key={i} className={styles.mdHeader}>{line.replace(/\*\*/g, '')}</h4>;
                }
                // Bold
                line = line.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
                // Lists
                if (line.startsWith('- ')) {
                    return <li key={i} className={styles.mdList} dangerouslySetInnerHTML={{ __html: line.substring(2) }} />;
                }
                if (line.match(/^\d+\. /)) {
                    return <li key={i} className={styles.mdList} dangerouslySetInnerHTML={{ __html: line.replace(/^\d+\. /, '') }} />;
                }
                // Tables (simple)
                if (line.startsWith('|')) {
                    return null; // Skip table formatting for now
                }
                // Regular paragraph
                if (line.trim()) {
                    return <p key={i} className={styles.mdPara} dangerouslySetInnerHTML={{ __html: line }} />;
                }
                return null;
            });
    };

    return (
        <div className={styles.overlay} onClick={onClose}>
            <div className={styles.panel} onClick={e => e.stopPropagation()}>
                {/* Header */}
                <div className={styles.header}>
                    <div className={styles.headerLeft}>
                        {(selectedTopic || selectedCategory) && (
                            <button className={styles.backBtn} onClick={handleBack}>
                                ←
                            </button>
                        )}
                        <BookOpen size={18} />
                        <span>Help & Documentation</span>
                    </div>
                    <button className={styles.closeBtn} onClick={onClose}>
                        <X size={18} />
                    </button>
                </div>

                {/* Search */}
                <div className={styles.searchBox}>
                    <Search size={16} className={styles.searchIcon} />
                    <input
                        ref={searchInputRef}
                        type="text"
                        placeholder="Search help topics..."
                        value={searchQuery}
                        onChange={e => setSearchQuery(e.target.value)}
                        className={styles.searchInput}
                    />
                    {searchQuery && (
                        <button className={styles.clearBtn} onClick={() => setSearchQuery('')}>
                            <X size={14} />
                        </button>
                    )}
                </div>

                {/* Content */}
                <div className={styles.content}>
                    {/* Search Results */}
                    {searchResults && (
                        <div className={styles.section}>
                            <div className={styles.sectionTitle}>
                                Search Results ({searchResults.length})
                            </div>
                            {searchResults.length === 0 ? (
                                <div className={styles.empty}>No topics found for "{searchQuery}"</div>
                            ) : (
                                <div className={styles.topicList}>
                                    {searchResults.map(topic => (
                                        <button
                                            key={topic.id}
                                            className={styles.topicItem}
                                            onClick={() => handleTopicClick(topic)}
                                        >
                                            <span className={styles.topicTitle}>{topic.title}</span>
                                            <span className={styles.topicCategory}>{topic.category}</span>
                                            <ChevronRight size={14} />
                                        </button>
                                    ))}
                                </div>
                            )}
                        </div>
                    )}

                    {/* Topic View */}
                    {selectedTopic && !searchResults && (
                        <div className={styles.topicView}>
                            <h2 className={styles.topicTitle}>{selectedTopic.title}</h2>
                            <div className={styles.topicContent}>
                                {renderMarkdown(selectedTopic.content)}
                            </div>
                            {selectedTopic.relatedTopics && selectedTopic.relatedTopics.length > 0 && (
                                <div className={styles.relatedSection}>
                                    <div className={styles.sectionTitle}>Related Topics</div>
                                    <div className={styles.relatedLinks}>
                                        {selectedTopic.relatedTopics.map(id => {
                                            const related = getHelpTopic(id);
                                            return related ? (
                                                <button
                                                    key={id}
                                                    className={styles.relatedLink}
                                                    onClick={() => handleTopicClick(related)}
                                                >
                                                    <ExternalLink size={12} />
                                                    {related.title}
                                                </button>
                                            ) : null;
                                        })}
                                    </div>
                                </div>
                            )}
                        </div>
                    )}

                    {/* Category Topics */}
                    {categoryTopics && !selectedTopic && !searchResults && (
                        <div className={styles.section}>
                            <div className={styles.sectionTitle}>
                                {HELP_CATEGORIES.find(c => c.id === selectedCategory)?.name}
                            </div>
                            <div className={styles.topicList}>
                                {categoryTopics.map(topic => (
                                    <button
                                        key={topic.id}
                                        className={styles.topicItem}
                                        onClick={() => handleTopicClick(topic)}
                                    >
                                        <span className={styles.topicTitle}>{topic.title}</span>
                                        <ChevronRight size={14} />
                                    </button>
                                ))}
                            </div>
                        </div>
                    )}

                    {/* Category List (Home) */}
                    {!selectedCategory && !searchResults && (
                        <div className={styles.categoryGrid}>
                            {HELP_CATEGORIES.map(category => (
                                <button
                                    key={category.id}
                                    className={styles.categoryCard}
                                    onClick={() => setSelectedCategory(category.id)}
                                >
                                    <span className={styles.categoryIcon}>{category.icon}</span>
                                    <span className={styles.categoryName}>{category.name}</span>
                                    <span className={styles.categoryCount}>
                                        {getTopicsByCategory(category.id).length} topics
                                    </span>
                                </button>
                            ))}
                        </div>
                    )}
                </div>

                {/* Footer */}
                <div className={styles.footer}>
                    <span>Press <kbd>F1</kbd> anytime for help</span>
                    <span>Press <kbd>Esc</kbd> to close</span>
                </div>
            </div>
        </div>
    );
}
