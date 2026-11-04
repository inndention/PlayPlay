import React from 'react';
import { Palette, Sparkles, Zap, Snowflake, Sun, Leaf, Ghost, Gift } from 'lucide-react';
import { THEME_PACKS } from '../lib/cgTemplates';
import { useCGState } from './CGLivePanel';
import styles from './CGThemeManager.module.css';

const ICON_MAP: Record<string, React.ReactNode> = {
    halloween: <Ghost size={16} />,
    christmas: <Gift size={16} />,
    summer: <Sun size={16} />,
    spring: <Leaf size={16} />,
    winter: <Snowflake size={16} />,
    auto: <Zap size={16} />,
    tech: <Zap size={16} />,
    agriculture: <Zap size={16} />,
    racing: <Zap size={16} />,
};

export function CGThemeManager() {
    const { updateDraftAdvanced, updateDraftCore } = useCGState();

    const applyTheme = (theme: any) => {
        // Apply theme-specific parameters
        if (theme.params) {
            Object.entries(theme.params).forEach(([key, value]) => {
                void updateDraftAdvanced(key, String(value));
            });
        }

        // Seasonal color mappings (simplified for now)
        if (theme.id === 'halloween') {
            void updateDraftCore('style1', '#ff7b00'); // Orange
            void updateDraftCore('style2', '#000000'); // Black
        } else if (theme.id === 'christmas') {
            void updateDraftCore('style1', '#ff0000'); // Red
            void updateDraftCore('style2', '#008000'); // Green
        } else if (theme.id === 'winter') {
            void updateDraftCore('style1', '#00d9ff'); // Cyan
            void updateDraftCore('style2', '#ffffff'); // White
        }
    };

    return (
        <div className={styles.manager}>
            <div className={styles.header}>
                <Palette size={18} />
                <h3>Theme Engine</h3>
            </div>

            {Object.entries(THEME_PACKS).map(([key, pack]) => (
                <div key={key} className={styles.packSection}>
                    <div className={styles.packTitle}>{pack.name}</div>
                    <div className={styles.themeGrid}>
                        {pack.themes.map((theme) => (
                            <button
                                key={theme.id}
                                type="button"
                                className={styles.themeButton}
                                onClick={() => applyTheme(theme)}
                                title={`Apply ${theme.name} theme`}
                            >
                                <div className={styles.themeIcon}>
                                    {ICON_MAP[theme.id] || <Sparkles size={16} />}
                                </div>
                                <span>{theme.name}</span>
                            </button>
                        ))}
                    </div>
                </div>
            ))}

            <div className={styles.footer}>
                <p>Themes apply global color schemes and seasonal assets to the selected CG template.</p>
            </div>
        </div>
    );
}
