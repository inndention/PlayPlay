import React, { useState } from 'react';
import { Layers, Search, LayoutGrid, List } from 'lucide-react';
import { useCGState } from './CGLivePanel';
import { CG_TEMPLATES } from '../lib/cgTemplates';
import styles from './CGTemplateShelf.module.css';

export function CGTemplateShelf() {
    const { selectTemplate, selectedTemplate } = useCGState();
    const [search, setSearch] = useState('');
    const [viewMode, setViewMode] = useState<'grid' | 'list'>('grid');
    const [activeCategory, setActiveCategory] = useState<string>('all');

    const categories = ['all', ...new Set(Object.values(CG_TEMPLATES).map(t => t.category))];

    const filteredTemplates = Object.entries(CG_TEMPLATES).filter(([id, t]) => {
        const matchesSearch = t.name.toLowerCase().includes(search.toLowerCase()) || 
                             t.description.toLowerCase().includes(search.toLowerCase());
        const matchesCategory = activeCategory === 'all' || t.category === activeCategory;
        return matchesSearch && matchesCategory;
    });

    return (
        <div className={styles.shelf}>
            <div className={styles.header}>
                <div className={styles.headerTitle}>
                    <Layers size={16} />
                    <span>Template Library</span>
                </div>
                <div className={styles.viewToggle}>
                    <button 
                        onClick={() => setViewMode('grid')} 
                        className={viewMode === 'grid' ? styles.activeToggle : ''}
                    >
                        <LayoutGrid size={14} />
                    </button>
                    <button 
                        onClick={() => setViewMode('list')} 
                        className={viewMode === 'list' ? styles.activeToggle : ''}
                    >
                        <List size={14} />
                    </button>
                </div>
            </div>

            <div className={styles.controls}>
                <div className={styles.searchBox}>
                    <Search size={14} />
                    <input 
                        type="text" 
                        placeholder="Search templates..." 
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                    />
                </div>
                <div className={styles.categories}>
                    {categories.map(cat => (
                        <button
                            key={cat}
                            className={`${styles.categoryTab} ${activeCategory === cat ? styles.activeCategory : ''}`}
                            onClick={() => setActiveCategory(cat)}
                        >
                            {cat.replace('-', ' ')}
                        </button>
                    ))}
                </div>
            </div>

            <div className={`${styles.templateList} ${styles[viewMode]}`}>
                {filteredTemplates.map(([id, t]) => (
                    <button
                        key={id}
                        className={`${styles.templateItem} ${selectedTemplate?.templateId === id ? styles.selectedItem : ''}`}
                         onClick={() => selectTemplate({
                            id,
                            name: t.name,
                            category: t.category as 'lower_third' | 'ticker' | 'bug' | 'fullscreen' | 'social' | 'weather' | 'banner',
                            filePath: '',
                            parameters: t.params.map(p => ({
                                name: p.name,
                                label: p.label,
                                type: p.type === 'textarea' ? 'text' : p.type === 'checkbox' ? 'text' : p.type === 'datetime' ? 'text' : p.type,
                                defaultValue: String(p.default || ''),
                                options: p.options,
                            })),
                            thumbnail: undefined,
                         })}
                    >
                        <div className={styles.templateIcon}>{t.icon}</div>
                        <div className={styles.templateInfo}>
                            <div className={styles.templateName}>{t.name}</div>
                            <div className={styles.templateDesc}>{t.description}</div>
                        </div>
                        {selectedTemplate?.templateId === id && (
                            <div className={styles.activeBadge}>Active</div>
                        )}
                    </button>
                ))}
            </div>
        </div>
    );
}
