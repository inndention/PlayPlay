// ============================================================
// ANIMATION DEFINITIONS
// ============================================================

export const ANIMATION_TYPES = {
    none: { label: 'None', css: '' },
    fade: { label: 'Fade', css: 'cg-anim-fade' },
    'slide-left': { label: 'Slide Left', css: 'cg-anim-slide-left' },
    'slide-right': { label: 'Slide Right', css: 'cg-anim-slide-right' },
    'slide-up': { label: 'Slide Up', css: 'cg-anim-slide-up' },
    'slide-down': { label: 'Slide Down', css: 'cg-anim-slide-down' },
    'wipe-left': { label: 'Wipe Left', css: 'cg-anim-wipe-left' },
    'wipe-right': { label: 'Wipe Right', css: 'cg-anim-wipe-right' },
    bounce: { label: 'Bounce', css: 'cg-anim-bounce' },
    zoom: { label: 'Zoom', css: 'cg-anim-zoom' },
    flip: { label: 'Flip', css: 'cg-anim-flip' },
    typewriter: { label: 'Typewriter', css: 'cg-anim-typewriter' }
};

export const ANIMATION_PRESETS = [
    { id: 'broadcast', name: 'Broadcast', icon: '📺', in: 'slide-left', out: 'slide-right', duration: 600, easing: 'ease-out' },
    { id: 'news', name: 'News', icon: '📰', in: 'wipe-left', out: 'wipe-right', duration: 500, easing: 'ease-in-out' },
    { id: 'smooth', name: 'Smooth', icon: '✨', in: 'fade', out: 'fade', duration: 800, easing: 'ease-in-out' },
    { id: 'dynamic', name: 'Dynamic', icon: '⚡', in: 'bounce', out: 'slide-down', duration: 700, easing: 'cubic-bezier(0.34,1.56,0.64,1)' },
    { id: 'corporate', name: 'Corporate', icon: '🏢', in: 'slide-up', out: 'slide-down', duration: 500, easing: 'ease-out' },
    { id: 'sports', name: 'Sports', icon: '⚽', in: 'zoom', out: 'zoom', duration: 400, easing: 'ease-out' },
    { id: 'elegant', name: 'Elegant', icon: '💎', in: 'fade', out: 'fade', duration: 1200, easing: 'ease-in-out' },
    { id: 'snap', name: 'Snap', icon: '📸', in: 'slide-right', out: 'slide-left', duration: 300, easing: 'ease-out' },
    { id: 'typewriter', name: 'Typewriter', icon: '⌨', in: 'typewriter', out: 'fade', duration: 1000, easing: 'linear' },
    { id: 'flip', name: 'Flip', icon: '↕', in: 'flip', out: 'flip', duration: 600, easing: 'ease-in-out' }
];

// ============================================================
// TEMPLATE DEFINITIONS
// ============================================================

export interface TemplateParam {
    name: string;
    label: string;
    type: 'text' | 'select' | 'color' | 'number' | 'textarea' | 'checkbox' | 'datetime';
    default: any;
    required?: boolean;
    options?: string[];
}

export interface TemplateDef {
    name: string;
    category: string;
    icon: string;
    description: string;
    params: TemplateParam[];
}

export const CG_TEMPLATES: Record<string, TemplateDef> = {
    // Lower Thirds
    'lower_third_modern': {
        name: 'Modern Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Gradient accent with clip-path design',
        params: [
            { name: 'name', label: 'Name', type: 'text', default: 'John Doe', required: true },
            { name: 'title', label: 'Title', type: 'text', default: 'Senior Correspondent', required: true },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' },
            { name: 'logo', label: 'Logo URL', type: 'text', default: '' },
            { name: 'color1', label: 'Primary Color', type: 'color', default: '00d9ff' },
            { name: 'color2', label: 'Secondary Color', type: 'color', default: 'a855f7' }
        ]
    },
    'lower_third_classic': {
        name: 'Classic Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Traditional broadcast lower third',
        params: [
            { name: 'name', label: 'Name', type: 'text', default: 'Jane Smith', required: true },
            { name: 'title', label: 'Title', type: 'text', default: 'News Anchor', required: true },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'lower_third_corporate': {
        name: 'Corporate Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Professional corporate style',
        params: [
            { name: 'name', label: 'Name', type: 'text', default: 'Michael Johnson', required: true },
            { name: 'title', label: 'Title', type: 'text', default: 'CEO, Tech Corp', required: true },
            { name: 'company', label: 'Company', type: 'text', default: 'Tech Corp Inc.' },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'lower_third_minimal': {
        name: 'Minimal Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Clean minimal design',
        params: [
            { name: 'name', label: 'Name', type: 'text', default: 'Sarah Connor', required: true },
            { name: 'title', label: 'Title', type: 'text', default: 'Field Reporter', required: true },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'lower_third_sport': {
        name: 'Sports Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Sports broadcast with stat highlight',
        params: [
            { name: 'name', label: 'Player Name', type: 'text', default: 'LeBron James', required: true },
            { name: 'title', label: 'Position/Team', type: 'text', default: 'SF, Lakers #23', required: true },
            { name: 'stat', label: 'Stat Highlight', type: 'text', default: '32 Points' },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'lower_third_sports': {
        name: 'Sports Lower Third v2',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Alternative sports lower third',
        params: [
            { name: 'name', label: 'Athlete Name', type: 'text', default: 'Tom Brady', required: true },
            { name: 'title', label: 'Team/Role', type: 'text', default: 'QB, Tampa Bay', required: true },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'lower_third': {
        name: 'Standard Lower Third',
        category: 'lower-thirds',
        icon: '▭',
        description: 'Standard broadcast lower third',
        params: [
            { name: 'name', label: 'Name', type: 'text', default: 'News Presenter', required: true },
            { name: 'title', label: 'Title', type: 'text', default: 'Chief Anchor', required: true },
            { name: 'position', label: 'Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },

    // Banners
    'banner_breaking_news': {
        name: 'Breaking News Banner',
        category: 'banners',
        icon: '▬',
        description: 'Urgent breaking news banner',
        params: [
            { name: 'text', label: 'Breaking News Text', type: 'text', default: 'Major story developing', required: true }
        ]
    },
    'banner_special_report': {
        name: 'Special Report Banner',
        category: 'banners',
        icon: '▬',
        description: 'Special report announcement',
        params: [
            { name: 'title', label: 'Report Title', type: 'text', default: 'SPECIAL REPORT', required: true },
            { name: 'subtitle', label: 'Subtitle', type: 'text', default: 'Live Coverage' }
        ]
    },
    'fullscreen_breaking_news': {
        name: 'Fullscreen Breaking News',
        category: 'banners',
        icon: '▬',
        description: 'Full-screen breaking news overlay',
        params: [
            { name: 'headline', label: 'Headline', type: 'text', default: 'BREAKING NEWS', required: true },
            { name: 'text', label: 'Subheadline', type: 'text', default: 'Developing story details here' }
        ]
    },

    // Fullscreen
    'fullscreen_coming_up': {
        name: 'Coming Up',
        category: 'fullscreen',
        icon: '⬜',
        description: 'Coming up next segment card',
        params: [
            { name: 'title', label: 'Title', type: 'text', default: 'Coming Up Next', required: true },
            { name: 'items', label: 'Items (comma separated)', type: 'textarea', default: 'Weather Update, Sports Highlights, Traffic Report' }
        ]
    },
    'fullscreen_end_credits': {
        name: 'End Credits',
        category: 'fullscreen',
        icon: '⬜',
        description: 'Scrolling end credits',
        params: [
            { name: 'title', label: 'Show Title', type: 'text', default: 'News Broadcast', required: true },
            { name: 'credits', label: 'Credits', type: 'textarea', default: 'Executive Producer: John Smith\nDirector: Jane Doe\nTechnical Director: Mike Johnson' }
        ]
    },
    'fullscreen_gfx': {
        name: 'Fullscreen Graphics',
        category: 'fullscreen',
        icon: '⬜',
        description: 'Full-screen graphic with title',
        params: [
            { name: 'title', label: 'Title', type: 'text', default: 'Special Report', required: true },
            { name: 'subtitle', label: 'Subtitle', type: 'text', default: 'In-depth analysis' },
            { name: 'background', label: 'Background Image URL', type: 'text', default: '' }
        ]
    },
    'fullscreen_title_card': {
        name: 'Title Card',
        category: 'fullscreen',
        icon: '⬜',
        description: 'Segment title card',
        params: [
            { name: 'title', label: 'Title', type: 'text', default: 'Segment Title', required: true },
            { name: 'subtitle', label: 'Subtitle', type: 'text', default: '' }
        ]
    },

    // Tickers
    'ticker_news': {
        name: 'News Ticker',
        category: 'tickers',
        icon: '↔',
        description: 'Scrolling news ticker',
        params: [
            { name: 'text', label: 'Ticker Text', type: 'text', default: 'Breaking news story • Another update • More information coming soon', required: true },
            { name: 'label', label: 'Label', type: 'text', default: 'NEWS' },
            { name: 'speed', label: 'Speed (pixels/sec)', type: 'number', default: 100 }
        ]
    },
    'ticker_sports_scores': {
        name: 'Sports Scores Ticker',
        category: 'tickers',
        icon: '↔',
        description: 'Live sports scores ticker',
        params: [
            { name: 'text', label: 'Scores', type: 'text', default: 'Lakers 112 - Celtics 108 • Warriors 105 - Bulls 98 • Yankees 5 - Red Sox 3', required: true },
            { name: 'label', label: 'Label', type: 'text', default: 'SCORES' },
            { name: 'speed', label: 'Speed', type: 'number', default: 80 }
        ]
    },
    'ticker_stock_market': {
        name: 'Stock Market Ticker',
        category: 'tickers',
        icon: '↔',
        description: 'Financial market data ticker',
        params: [
            { name: 'text', label: 'Stock Data', type: 'text', default: 'AAPL +2.45% • GOOGL -1.23% • MSFT +0.89% • AMZN +1.56%', required: true },
            { name: 'label', label: 'Label', type: 'text', default: 'MARKETS' },
            { name: 'speed', label: 'Speed', type: 'number', default: 60 }
        ]
    },
    'ticker': {
        name: 'Generic Ticker',
        category: 'tickers',
        icon: '↔',
        description: 'Generic scrolling ticker',
        params: [
            { name: 'text', label: 'Ticker Text', type: 'text', default: 'Ticker content goes here', required: true },
            { name: 'label', label: 'Label', type: 'text', default: 'UPDATE' },
            { name: 'speed', label: 'Speed', type: 'number', default: 100 }
        ]
    },

    // Score Bugs
    'score_bug_sports': {
        name: 'Sports Score Bug',
        category: 'score-bugs',
        icon: '⚽',
        description: 'Live sports score overlay',
        params: [
            { name: 'team1', label: 'Home Team', type: 'text', default: 'HOME', required: true },
            { name: 'score1', label: 'Home Score', type: 'number', default: 0 },
            { name: 'team2', label: 'Away Team', type: 'text', default: 'AWAY', required: true },
            { name: 'score2', label: 'Away Score', type: 'number', default: 0 },
            { name: 'period', label: 'Period/Quarter', type: 'text', default: 'Q1' },
            { name: 'time', label: 'Game Time', type: 'text', default: '12:00' }
        ]
    },
    'score_bug_minimal': {
        name: 'Minimal Score Bug',
        category: 'score-bugs',
        icon: '⚽',
        description: 'Clean minimal score display',
        params: [
            { name: 'team1', label: 'Team 1', type: 'text', default: 'TEAM A', required: true },
            { name: 'score1', label: 'Score 1', type: 'number', default: 0 },
            { name: 'team2', label: 'Team 2', type: 'text', default: 'TEAM B', required: true },
            { name: 'score2', label: 'Score 2', type: 'number', default: 0 }
        ]
    },

    // Weather
    'weather_forecast_ai': {
        name: 'AI Weather Forecast',
        category: 'weather',
        icon: '☁',
        description: 'AI-powered weather forecast',
        params: [
            { name: 'location', label: 'Location', type: 'text', default: 'martinsville', required: true }
        ]
    },
    'weather_forecast_voice': {
        name: 'Voice Weather Forecast',
        category: 'weather',
        icon: '☁',
        description: 'Voice-enabled weather forecast',
        params: [
            { name: 'location', label: 'Location', type: 'text', default: 'New York', required: true }
        ]
    },
    'weather_bug_rotating': {
        name: 'Rotating Weather Bug',
        category: 'weather',
        icon: '☁',
        description: 'Rotating weather conditions bug',
        params: [
            { name: 'location', label: 'Location', type: 'text', default: 'New York', required: true },
            { name: 'temp', label: 'Temperature', type: 'text', default: '72°F' }
        ]
    },

    // Bugs & Overlays
    'bug_live_indicator': {
        name: 'LIVE Indicator',
        category: 'bugs',
        icon: '🔴',
        description: 'Pulsing LIVE broadcast indicator',
        params: [
            { name: 'text', label: 'Label', type: 'text', default: 'LIVE' }
        ]
    },
    'bug_logo_corner': {
        name: 'Corner Logo',
        category: 'bugs',
        icon: '🔵',
        description: 'Channel logo corner bug',
        params: [
            { name: 'logo', label: 'Logo URL', type: 'text', default: '' },
            { name: 'position', label: 'Position', type: 'select', options: ['top-left', 'top-right', 'bottom-left', 'bottom-right'], default: 'top-right' }
        ]
    },
    'bug_recording': {
        name: 'Recording Indicator',
        category: 'bugs',
        icon: '🔴',
        description: 'REC indicator overlay',
        params: [
            { name: 'text', label: 'Label', type: 'text', default: 'REC' }
        ]
    },
    'master_overlay': {
        name: 'Master Overlay',
        category: 'bugs',
        icon: '🔵',
        description: 'Clock and date master overlay',
        params: [
            { name: 'showClock', label: 'Show Clock', type: 'checkbox', default: true },
            { name: 'showDate', label: 'Show Date', type: 'checkbox', default: true }
        ]
    },

    // Social
    'social_twitter': {
        name: 'Twitter Feed',
        category: 'social',
        icon: '🐦',
        description: 'Twitter/X post overlay',
        params: [
            { name: 'handle', label: 'Twitter Handle', type: 'text', default: '@username' },
            { name: 'text', label: 'Tweet Text', type: 'textarea', default: 'Tweet content here...' },
            { name: 'name', label: 'Display Name', type: 'text', default: 'User Name' }
        ]
    },
    'social_youtube_subscribe': {
        name: 'YouTube Subscribe',
        category: 'social',
        icon: '▶',
        description: 'YouTube subscribe call-to-action',
        params: [
            { name: 'channel', label: 'Channel Name', type: 'text', default: 'Channel Name', required: true },
            { name: 'subscribers', label: 'Subscriber Count', type: 'text', default: '1M' }
        ]
    },

    // Special
    'countdown_timer': {
        name: 'Countdown Timer',
        category: 'special',
        icon: '⏱',
        description: 'Event countdown timer',
        params: [
            { name: 'target', label: 'Target Date/Time', type: 'datetime', default: '' },
            { name: 'label', label: 'Label', type: 'text', default: 'Time Remaining' }
        ]
    },
    'emergency': {
        name: 'Emergency Alert',
        category: 'special',
        icon: '⚠',
        description: 'Emergency broadcast alert',
        params: [
            { name: 'message', label: 'Alert Message', type: 'text', default: 'This is a test of the emergency broadcast system', required: true }
        ]
    }
};

export const THEME_PACKS = {
    seasonal: {
        name: 'Seasonal \u0026 Holiday',
        themes: [
            { id: 'halloween', name: 'Halloween', params: { holiday: 'halloween' } },
            { id: 'christmas', name: 'Christmas', params: { holiday: 'christmas' } },
            { id: 'summer', name: 'Summer', params: { holiday: 'summer' } },
            { id: 'spring', name: 'Spring', params: { holiday: 'spring' } },
            { id: 'winter', name: 'Winter', params: { holiday: 'winter' } }
        ]
    },
    industry: {
        name: 'Industry Themed',
        themes: [
            { id: 'auto', name: 'Automotive', params: { industry: 'auto' } },
            { id: 'tech', name: 'Technology', params: { industry: 'tech' } },
            { id: 'agriculture', name: 'Agriculture', params: { industry: 'agriculture' } },
            { id: 'racing', name: 'Racing', params: { industry: 'racing' } }
        ]
    }
};
