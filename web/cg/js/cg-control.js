/**
 * Broadcast CG Control Center
 * Main application logic for managing and deploying HTML CG templates
 * Features: Animation types, Remote text, Output monitor, Deployment history
 */

// ============================================================
// ANIMATION DEFINITIONS
// ============================================================
const ANIMATION_TYPES = {
    none: { label: 'None', css: '' },
    fade: { label: 'Fade', css: 'cg-anim-fade' },
    phase: { label: 'Phase', css: 'cg-anim-phase' },
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

const ANIMATION_PRESETS = [
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
const TEMPLATES = {
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
    },
    'directors_scene': {
        name: "Director's Scene",
        category: 'special',
        icon: '🎬',
        description: 'Director scene marker',
        params: [
            { name: 'scene', label: 'Scene Name', type: 'text', default: 'Scene 1', required: true },
            { name: 'notes', label: 'Director Notes', type: 'textarea', default: '' }
        ]
    },

    // Election
    'election_results': {
        name: 'Election Results',
        category: 'election',
        icon: '🗳',
        description: 'Live vote count with candidate comparison bars',
        params: [
            { name: 'candidate1', label: 'Candidate 1 Name', type: 'text', default: 'Candidate A', required: true },
            { name: 'votes1', label: 'Candidate 1 Votes %', type: 'number', default: '52' },
            { name: 'candidate2', label: 'Candidate 2 Name', type: 'text', default: 'Candidate B', required: true },
            { name: 'votes2', label: 'Candidate 2 Votes %', type: 'number', default: '48' },
            { name: 'party1', label: 'Party 1 Label', type: 'text', default: 'DEM' },
            { name: 'color1', label: 'Party 1 Color', type: 'color', default: '2563eb' },
            { name: 'party2', label: 'Party 2 Label', type: 'text', default: 'REP' },
            { name: 'color2', label: 'Party 2 Color', type: 'color', default: 'dc2626' },
            { name: 'label', label: 'Race Label', type: 'text', default: 'Election Results' },
            { name: 'reporting', label: 'Reporting %', type: 'text', default: '100% Reporting' },
            { name: 'winner', label: 'Projected Winner', type: 'select', options: ['none', '1', '2'], default: 'none' }
        ]
    },
    'candidate_lower': {
        name: 'Candidate Lower Third',
        category: 'election',
        icon: '🗳',
        description: 'Candidate name with party affiliation',
        params: [
            { name: 'name', label: 'Candidate Name', type: 'text', default: 'Sen. Jane Smith', required: true },
            { name: 'title', label: 'Position/Title', type: 'text', default: 'U.S. Senate Candidate' },
            { name: 'party', label: 'Party Label', type: 'text', default: 'DEM' },
            { name: 'state', label: 'State/District', type: 'text', default: 'California' },
            { name: 'color', label: 'Party Color', type: 'color', default: '2563eb' },
            { name: 'position', label: 'Screen Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },

    // Podcast / Streaming
    'podcast_host': {
        name: 'Podcast Host Card',
        category: 'streaming',
        icon: '🎙',
        description: 'Host name card with show info',
        params: [
            { name: 'name', label: 'Host Name', type: 'text', default: 'Alex Johnson', required: true },
            { name: 'show', label: 'Show Title', type: 'text', default: 'The Daily Podcast' },
            { name: 'role', label: 'Role', type: 'text', default: 'Host & Producer' },
            { name: 'episode', label: 'Episode', type: 'text', default: 'Episode 42' },
            { name: 'color1', label: 'Accent Color 1', type: 'color', default: 'a78bfa' },
            { name: 'color2', label: 'Accent Color 2', type: 'color', default: '7c3aed' },
            { name: 'position', label: 'Screen Position', type: 'select', options: ['left', 'center', 'right'], default: 'left' }
        ]
    },
    'stream_subscriber': {
        name: 'Subscriber Milestone',
        category: 'streaming',
        icon: '🎉',
        description: 'Subscriber or follower milestone alert',
        params: [
            { name: 'milestone', label: 'Milestone Number', type: 'text', default: '10,000', required: true },
            { name: 'unit', label: 'Unit Label', type: 'text', default: 'Subscribers' },
            { name: 'username', label: 'Display Name', type: 'text', default: 'Thank You!' },
            { name: 'message', label: 'Message', type: 'text', default: "You're our newest subscriber" },
            { name: 'label', label: 'Alert Label', type: 'text', default: 'SUBSCRIBER MILESTONE' },
            { name: 'icon', label: 'Icon (emoji)', type: 'text', default: '🎉' }
        ]
    },
    'stream_donation': {
        name: 'Donation Alert',
        category: 'streaming',
        icon: '💚',
        description: 'Live stream donation alert overlay',
        params: [
            { name: 'donor', label: 'Donor Name', type: 'text', default: 'Anonymous', required: true },
            { name: 'amount', label: 'Amount', type: 'text', default: '25.00', required: true },
            { name: 'currency', label: 'Currency Symbol', type: 'text', default: '$' },
            { name: 'message', label: 'Message', type: 'text', default: 'Thank you for the great content!' },
            { name: 'label', label: 'Alert Label', type: 'text', default: 'New Donation' }
        ]
    },

    // Advertising
    'sponsor_bump': {
        name: 'Sponsor Bump',
        category: 'advertising',
        icon: '📢',
        description: '"Brought to you by" sponsor bar',
        params: [
            { name: 'sponsor', label: 'Sponsor Name', type: 'text', default: 'Sponsor Name', required: true },
            { name: 'tagline', label: 'Tagline', type: 'text', default: 'Your trusted partner' },
            { name: 'logo', label: 'Logo URL', type: 'text', default: '' },
            { name: 'prefix', label: 'Prefix Text', type: 'text', default: 'Brought to you by' },
            { name: 'color', label: 'Accent Color', type: 'color', default: '00d9ff' }
        ]
    },
    'ad_slate': {
        name: 'Ad Break Slate',
        category: 'advertising',
        icon: '📺',
        description: 'Full-screen ad break countdown',
        params: [
            { name: 'duration', label: 'Duration (seconds)', type: 'number', default: '60', required: true },
            { name: 'label', label: 'Label', type: 'text', default: 'Commercial Break' },
            { name: 'return', label: 'Return Text', type: 'text', default: "We'll be right back" },
            { name: 'show', label: 'Show Name', type: 'text', default: '' }
        ]
    },

    // Sports Extended
    'player_stat': {
        name: 'Player Stat Card',
        category: 'sports-extended',
        icon: '🏅',
        description: 'Player profile with key statistics',
        params: [
            { name: 'name', label: 'Player Name', type: 'text', default: 'LeBron James', required: true },
            { name: 'team', label: 'Team Name', type: 'text', default: 'Los Angeles Lakers' },
            { name: 'position', label: 'Position', type: 'text', default: 'Small Forward' },
            { name: 'number', label: 'Jersey Number', type: 'text', default: '23' },
            { name: 'color', label: 'Team Color', type: 'color', default: '552583' },
            { name: 'stat1', label: 'Stat 1 Value', type: 'text', default: '28.5' },
            { name: 'stat1label', label: 'Stat 1 Label', type: 'text', default: 'PTS' },
            { name: 'stat2', label: 'Stat 2 Value', type: 'text', default: '8.3' },
            { name: 'stat2label', label: 'Stat 2 Label', type: 'text', default: 'REB' },
            { name: 'stat3', label: 'Stat 3 Value', type: 'text', default: '6.8' },
            { name: 'stat3label', label: 'Stat 3 Label', type: 'text', default: 'AST' }
        ]
    },
    'standings_bug': {
        name: 'Standings Bug',
        category: 'sports-extended',
        icon: '🏆',
        description: 'Compact league standings overlay',
        params: [
            { name: 'league', label: 'League Name', type: 'text', default: 'NBA Standings' },
            { name: 'icon', label: 'League Icon', type: 'text', default: '🏀' },
            { name: 'color', label: 'Header Color', type: 'color', default: '1e40af' },
            { name: 'teams', label: 'Teams (ABBR:W:L, csv)', type: 'text', default: 'LAL:42:28,BOS:44:26,MIA:38:32,MIL:37:33,PHI:36:34' },
            { name: 'highlight', label: 'Highlight Rank #', type: 'number', default: '1' }
        ]
    }
};

// ============================================================
// APPLICATION STATE
// ============================================================
const state = {
    selectedTemplate: null,
    currentCategory: 'all',
    searchQuery: '',
    templateParams: {},
    viewMode: 'grid', // 'grid' | 'list'
    animation: {
        in: 'none',
        out: 'none',
        inDuration: 600,
        outDuration: 400,
        delay: 0,
        easing: 'ease-out',
        autoHide: 0,
        loop: false
    },
    remote: {
        enabled: false,
        sourceType: 'url',
        url: '',
        wsUrl: '',
        targetField: '',
        jsonPath: '',
        refreshInterval: 0,
        transform: '',
        intervalId: null,
        wsConnection: null
    },
    dataSources: JSON.parse(localStorage.getItem('cg_data_sources') || '[]'),
    deploymentHistory: JSON.parse(localStorage.getItem('cg_history') || '[]'),
    currentOutput: null,
    serverConnected: false,

    // Multi-channel (preview = auto-updated as you type, program = TAKE only)
    outputChannel: 'main',
    outputChannels: { main: null, secondary: null, web: null, preview: null, program: null },

    // Rundown / Playlist
    playlists: [],
    activePlaylistId: null,
    activePlaylistIndex: -1,

    // Template Builder
    builderDraft: { name: '', category: 'custom', icon: '⬜', description: '', layout: 'lower-third', params: [], style: { accent: '00d9ff', bg: '0a0f1e', text: 'ffffff', font: 'Inter' } },
    builderStep: 1,
    builderEditId: null,
    customTemplatesMap: {},

    // NDI Source Switcher - PROGRAM/PREVIEW
    ndi: {
        programSource: null,
        previewSource: null,
        sources: [],
        transition: { type: 'cut', duration: 500 },
        playlist: [],
        playlistIndex: 0,
        isPlaying: false,
        schedule: { enabled: false, time: '00:00' },
        cgDuringSwitch: { enabled: false, template: null }
    }
};

// ============================================================
// DOM ELEMENTS
// ============================================================
const elements = {
    templateGrid: document.getElementById('templateGrid'),
    configPanel: document.getElementById('configPanel'),
    configTitle: document.getElementById('configTitle'),
    formFields: document.getElementById('formFields'),
    previewFrame: document.getElementById('previewFrame'),
    outputUrl: document.getElementById('outputUrl'),
    searchInput: document.getElementById('searchInput'),
    templateCount: document.getElementById('templateCount'),
    toastContainer: document.getElementById('toastContainer'),
    apiModal: document.getElementById('apiModal'),
    activeOutputBar: document.getElementById('activeOutputBar'),
    activeTemplateName: document.getElementById('activeTemplateName'),
    statusDot: document.getElementById('statusDot'),
    statusText: document.getElementById('statusText'),
    wsStatus: document.getElementById('wsStatus')
};

// ============================================================
// INITIALIZE
// ============================================================
function init() {
    renderTemplates();
    setupEventListeners();
    setupTabNavigation();
    setupAnimationControls();
    setupRemoteTextControls();
    updateTemplateCount();
    checkServerConnection();
    renderPresetGrid();
    renderDataSourceList();
    renderHistoryList();
    loadCustomTemplates();
    loadPlaylists();
    setupKeyboardShortcuts();
    renderPresetSelect();
    setupNdiModal();
    setupMediaLibrary();
    setupSourceContextMenus();
    setupCGSlotModeListeners();
    loadCGMarriageState();
    setupAsRunModal();
    setupPGMMonitor();
    connectGlobalWs();
}

// ============================================================
// RENDER TEMPLATES
// ============================================================
function renderTemplates() {
    const filteredTemplates = Object.entries(TEMPLATES).filter(([key, template]) => {
        const matchesCategory = state.currentCategory === 'all' || template.category === state.currentCategory;
        const matchesSearch = !state.searchQuery ||
            template.name.toLowerCase().includes(state.searchQuery.toLowerCase()) ||
            key.toLowerCase().includes(state.searchQuery.toLowerCase()) ||
            (template.description || '').toLowerCase().includes(state.searchQuery.toLowerCase());
        return matchesCategory && matchesSearch;
    });

    if (state.viewMode === 'list') {
        elements.templateGrid.innerHTML = filteredTemplates.map(([key, template]) => `
            <div class="template-card" data-template="${key}">
                <span class="template-icon">${template.icon}</span>
                <div class="template-card-info">
                    <div class="template-name">${template.name}</div>
                    <div class="template-category">${formatCategory(template.category)}</div>
                </div>
                <div class="template-card-actions">
                    <button class="template-quick-btn" data-action="configure" data-template="${key}">Configure</button>
                    <button class="template-quick-btn deploy" data-action="quickdeploy" data-template="${key}">Deploy</button>
                </div>
            </div>
        `).join('');
    } else {
        elements.templateGrid.innerHTML = filteredTemplates.map(([key, template]) => `
            <div class="template-card" data-template="${key}">
                <span class="template-icon">${template.icon}</span>
                <div class="template-name">${template.name}</div>
                <div class="template-category">${formatCategory(template.category)}</div>
                ${template.description ? `<div class="template-desc">${template.description}</div>` : ''}
                <div class="template-card-actions">
                    <button class="template-quick-btn" data-action="configure" data-template="${key}">Configure</button>
                    <button class="template-quick-btn deploy" data-action="quickdeploy" data-template="${key}">Deploy</button>
                </div>
            </div>
        `).join('');
    }

    updateTemplateCount(filteredTemplates.length);
}

function formatCategory(category) {
    return category.split('-').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
}

function updateTemplateCount(count) {
    const total = count !== undefined ? count : Object.keys(TEMPLATES).length;
    elements.templateCount.textContent = `${total} template${total !== 1 ? 's' : ''}`;
}

// ============================================================
// EVENT LISTENERS
// ============================================================
function setupEventListeners() {
    // Category Navigation
    document.querySelectorAll('.nav-item[data-category]').forEach(item => {
        item.addEventListener('click', () => {
            document.querySelectorAll('.nav-item[data-category]').forEach(i => i.classList.remove('active'));
            item.classList.add('active');
            state.currentCategory = item.dataset.category;
            renderTemplates();
        });
    });

    // Search
    elements.searchInput.addEventListener('input', (e) => {
        state.searchQuery = e.target.value;
        renderTemplates();
    });

    // Template Grid Clicks
    elements.templateGrid.addEventListener('click', (e) => {
        const actionBtn = e.target.closest('[data-action]');
        if (actionBtn) {
            e.stopPropagation();
            const action = actionBtn.dataset.action;
            const key = actionBtn.dataset.template;
            if (action === 'configure') selectTemplate(key);
            if (action === 'quickdeploy') quickDeploy(key);
            return;
        }
        const card = e.target.closest('.template-card');
        if (card) selectTemplate(card.dataset.template);
    });

    // Close Config Panel
    document.getElementById('btnCloseConfig').addEventListener('click', closeConfigPanel);

    // Update Preview
    document.getElementById('btnUpdatePreview').addEventListener('click', updatePreview);

    // Copy URL
    document.getElementById('btnCopyUrl').addEventListener('click', copyUrl);
    document.getElementById('btnCopyFullUrl').addEventListener('click', copyUrl);

    // Deploy
    document.getElementById('btnDeploy').addEventListener('click', deployTemplate);

    // Fullscreen Preview
    document.getElementById('btnFullscreenPreview').addEventListener('click', fullscreenPreview);

    // Replay Preview
    document.getElementById('btnReplayPreview').addEventListener('click', replayPreview);

    // Refresh
    document.getElementById('btnRefresh').addEventListener('click', () => {
        renderTemplates();
        showToast('Templates refreshed', 'success');
    });

    // View Toggle
    document.getElementById('btnGridView').addEventListener('click', () => {
        state.viewMode = 'grid';
        elements.templateGrid.classList.remove('list-view');
        document.getElementById('btnGridView').classList.add('active');
        document.getElementById('btnListView').classList.remove('active');
        renderTemplates();
    });

    document.getElementById('btnListView').addEventListener('click', () => {
        state.viewMode = 'list';
        elements.templateGrid.classList.add('list-view');
        document.getElementById('btnListView').classList.add('active');
        document.getElementById('btnGridView').classList.remove('active');
        renderTemplates();
    });

    // Active Output Bar
    document.getElementById('btnClearOutput').addEventListener('click', clearOutput);
    document.getElementById('btnReplayOutput').addEventListener('click', () => {
        if (state.currentOutput) {
            deployTemplate();
        }
    });

    // Tool Nav Buttons
    document.getElementById('btnRemoteTextManager').addEventListener('click', () => {
        document.getElementById('remoteTextModal').classList.add('open');
    });
    document.getElementById('btnAnimationPresets').addEventListener('click', () => {
        document.getElementById('animPresetsModal').classList.add('open');
    });
    document.getElementById('btnOutputMonitor').addEventListener('click', openOutputMonitor);
    document.getElementById('btnApiDocs').addEventListener('click', () => {
        elements.apiModal.classList.add('open');
    });
    document.getElementById('btnRundown').addEventListener('click', openRundown);
    document.getElementById('btnTemplateBuilder').addEventListener('click', () => openTemplateBuilder(null));
    document.getElementById('btnNdiSwitcher').addEventListener('click', () => {
        document.getElementById('ndiSwitcherModal').classList.add('open');
    });

    // Channel selector
    document.querySelectorAll('.channel-tab').forEach(btn => {
        btn.addEventListener('click', () => selectChannel(btn.dataset.channel));
    });

    // Rundown modal controls
    document.getElementById('rundownPlaylistSelect').addEventListener('change', (e) => {
        state.activePlaylistId = e.target.value;
        state.activePlaylistIndex = -1;
        renderRundownItems();
    });
    document.getElementById('btnAddToRundown').addEventListener('click', addToRundown);
    document.getElementById('btnRundownNew').addEventListener('click', createPlaylist);
    document.getElementById('btnRundownDelete').addEventListener('click', deleteActivePlaylist);
    document.getElementById('btnRundownAddCurrent').addEventListener('click', addToRundown);
    document.getElementById('btnRundownPlay').addEventListener('click', playRundown);
    document.getElementById('btnRundownNext').addEventListener('click', nextRundownItem);
    document.getElementById('btnRundownPrev').addEventListener('click', prevRundownItem);
    document.getElementById('btnRundownStop').addEventListener('click', stopRundown);

    // Builder modal controls
    document.getElementById('builderNextBtn').addEventListener('click', builderNext);
    document.getElementById('builderBackBtn').addEventListener('click', builderBack);

    // Modal Close Buttons
    ['btnCloseModal', 'btnCloseRemoteModal', 'btnCloseAnimModal', 'btnCloseOutputModal',
        'btnCloseRundownModal', 'btnCloseBuilderModal', 'btnCloseNdiModal', 'btnCloseMediaModal',
        'btnClosePGMModal', 'btnCloseAsRunModal'].forEach(id => {
            const btn = document.getElementById(id);
            if (btn) btn.addEventListener('click', () => {
                document.querySelectorAll('.modal').forEach(m => m.classList.remove('open'));
            });
        });

    // Close modals on backdrop click
    document.querySelectorAll('.modal').forEach(modal => {
        modal.addEventListener('click', (e) => {
            if (e.target === modal) modal.classList.remove('open');
        });
    });

    // Output Monitor
    document.getElementById('btnMonitorClear').addEventListener('click', clearOutput);
    document.getElementById('btnMonitorRefresh').addEventListener('click', () => {
        const frame = document.getElementById('monitorFrame');
        if (frame.src) frame.src = frame.src;
    });

    // Data Source Manager
    document.getElementById('btnSaveDataSource').addEventListener('click', saveDataSource);

    // Preview Animation
    document.getElementById('btnPreviewAnim').addEventListener('click', replayPreview);

    // Auto-hide custom duration
    document.getElementById('autoHide').addEventListener('change', (e) => {
        const customGroup = document.getElementById('customDurationGroup');
        customGroup.style.display = e.target.value === 'custom' ? 'block' : 'none';
        if (e.target.value !== 'custom') {
            state.animation.autoHide = parseInt(e.target.value) || 0;
        }
    });

    document.getElementById('customDuration').addEventListener('input', (e) => {
        state.animation.autoHide = parseInt(e.target.value) || 0;
    });

    // TAKE
    document.getElementById('btnTake').addEventListener('click', take);

    // Presets
    document.getElementById('btnSavePreset').addEventListener('click', saveCurrentAsPreset);
    document.getElementById('presetSelect').addEventListener('change', loadSelectedPreset);
    document.getElementById('btnDeletePreset').addEventListener('click', deleteSelectedPreset);

    // OBS URL copy buttons
    document.getElementById('btnCopyObsPreview').addEventListener('click', () => {
        navigator.clipboard.writeText('http://localhost:3060/obs-preview.html').then(() => {
            showToast('Preview URL copied', 'success');
        });
    });
    document.getElementById('btnCopyObsProgram').addEventListener('click', () => {
        navigator.clipboard.writeText('http://localhost:3060/obs-program.html').then(() => {
            showToast('Program URL copied', 'success');
        });
    });

    // Remote text test
    document.getElementById('btnTestRemote').addEventListener('click', testRemoteConnection);
    document.getElementById('btnApplyRemote').addEventListener('click', applyRemoteAndDeploy);

    // Remote source type change
    document.getElementById('remoteSourceType').addEventListener('change', (e) => {
        state.remote.sourceType = e.target.value;
        document.getElementById('remoteUrlGroup').style.display = e.target.value === 'websocket' ? 'none' : 'block';
        document.getElementById('remoteWsGroup').style.display = e.target.value === 'websocket' ? 'block' : 'none';
    });

    // Remote text enabled toggle
    document.getElementById('remoteTextEnabled').addEventListener('change', (e) => {
        state.remote.enabled = e.target.checked;
        document.getElementById('remoteTextConfig').style.opacity = e.target.checked ? '1' : '0.5';
        document.getElementById('remoteTextConfig').style.pointerEvents = e.target.checked ? 'auto' : 'none';
        if (!e.target.checked) stopRemotePolling();
    });
}

// ============================================================
// TAB NAVIGATION
// ============================================================
function setupTabNavigation() {
    document.querySelectorAll('.config-tab').forEach(tab => {
        tab.addEventListener('click', () => {
            const tabId = tab.dataset.tab;
            document.querySelectorAll('.config-tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.config-tab-content').forEach(c => c.classList.remove('active'));
            tab.classList.add('active');
            document.getElementById(`tab-${tabId}`).classList.add('active');

            // Populate remote target fields when switching to remote tab
            if (tabId === 'remote') populateRemoteTargetFields();
        });
    });
}

// ============================================================
// ANIMATION CONTROLS
// ============================================================
function setupAnimationControls() {
    // In animation buttons
    document.querySelectorAll('#animInGrid .anim-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('#animInGrid .anim-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            state.animation.in = btn.dataset.anim;
            updatePreview();
        });
    });

    // Out animation buttons
    document.querySelectorAll('#animOutGrid .anim-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('#animOutGrid .anim-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            state.animation.out = btn.dataset.anim;
        });
    });

    // Duration inputs
    document.getElementById('animInDuration').addEventListener('input', (e) => {
        state.animation.inDuration = parseInt(e.target.value) || 600;
        updatePreview();
    });
    document.getElementById('animOutDuration').addEventListener('input', (e) => {
        state.animation.outDuration = parseInt(e.target.value) || 400;
    });
    document.getElementById('animDelay').addEventListener('input', (e) => {
        state.animation.delay = parseInt(e.target.value) || 0;
        updatePreview();
    });
    document.getElementById('animEasing').addEventListener('change', (e) => {
        state.animation.easing = e.target.value;
        updatePreview();
    });
    document.getElementById('animLoop').addEventListener('change', (e) => {
        state.animation.loop = e.target.checked;
    });
}

// ============================================================
// REMOTE TEXT CONTROLS
// ============================================================
function setupRemoteTextControls() {
    document.getElementById('remoteUrl').addEventListener('input', (e) => {
        state.remote.url = e.target.value;
    });
    document.getElementById('remoteWsUrl').addEventListener('input', (e) => {
        state.remote.wsUrl = e.target.value;
    });
    document.getElementById('remoteJsonPath').addEventListener('input', (e) => {
        state.remote.jsonPath = e.target.value;
    });
    document.getElementById('remoteRefreshInterval').addEventListener('change', (e) => {
        state.remote.refreshInterval = parseInt(e.target.value) || 0;
    });
    document.getElementById('remoteTransform').addEventListener('input', (e) => {
        state.remote.transform = e.target.value;
    });
}

function populateRemoteTargetFields() {
    if (!state.selectedTemplate) return;
    const template = TEMPLATES[state.selectedTemplate];
    const select = document.getElementById('remoteTargetField');
    select.innerHTML = template.params
        .filter(p => ['text', 'textarea'].includes(p.type))
        .map(p => `<option value="${p.name}">${p.label}</option>`)
        .join('');
    state.remote.targetField = select.value;
    select.addEventListener('change', (e) => {
        state.remote.targetField = e.target.value;
    });
}

async function testRemoteConnection() {
    const dot = document.getElementById('remoteStatusDot');
    const text = document.getElementById('remoteStatusText');
    dot.className = 'remote-status-dot loading';
    text.textContent = 'Testing connection...';

    try {
        if (state.remote.sourceType === 'websocket') {
            const ws = new WebSocket(state.remote.wsUrl || 'ws://localhost:8060');
            ws.onopen = () => {
                dot.className = 'remote-status-dot connected';
                text.textContent = 'WebSocket connected';
                ws.close();
            };
            ws.onerror = () => {
                dot.className = 'remote-status-dot error';
                text.textContent = 'WebSocket connection failed';
            };
        } else {
            const url = state.remote.url;
            if (!url) throw new Error('No URL specified');
            const response = await fetch(url);
            if (!response.ok) throw new Error(`HTTP ${response.status}`);
            const data = await response.text();
            const value = extractValue(data, state.remote.jsonPath);
            dot.className = 'remote-status-dot connected';
            text.textContent = `Connected — Value: "${String(value).substring(0, 50)}${String(value).length > 50 ? '...' : ''}"`;
            showToast('Remote connection successful', 'success');
        }
    } catch (err) {
        dot.className = 'remote-status-dot error';
        text.textContent = `Error: ${err.message}`;
        showToast(`Remote test failed: ${err.message}`, 'error');
    }
}

function extractValue(data, jsonPath) {
    try {
        const parsed = JSON.parse(data);
        if (!jsonPath) return JSON.stringify(parsed);
        return jsonPath.split('.').reduce((obj, key) => {
            const arrMatch = key.match(/^(\w+)\[(\d+)\]$/);
            if (arrMatch) return obj[arrMatch[1]][parseInt(arrMatch[2])];
            return obj[key];
        }, parsed);
    } catch {
        return data; // Return raw text if not JSON
    }
}

function applyTransform(value, transformCode) {
    if (!transformCode.trim()) return value;
    try {
        // eslint-disable-next-line no-new-func
        const fn = new Function('value', transformCode);
        return fn(value);
    } catch (e) {
        console.warn('Transform error:', e);
        return value;
    }
}

async function fetchRemoteValue() {
    try {
        const response = await fetch(state.remote.url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.text();
        let value = extractValue(data, state.remote.jsonPath);
        if (state.remote.transform) value = applyTransform(value, state.remote.transform);
        return String(value);
    } catch (err) {
        console.warn('Remote fetch error:', err);
        return null;
    }
}

function startRemotePolling() {
    stopRemotePolling();
    if (!state.remote.enabled || !state.remote.url || !state.remote.refreshInterval) return;

    state.remote.intervalId = setInterval(async () => {
        const value = await fetchRemoteValue();
        if (value !== null && state.remote.targetField) {
            state.templateParams[state.remote.targetField] = value;
            // Update form field
            const field = elements.formFields.querySelector(`[name="${state.remote.targetField}"]`);
            if (field) field.value = value;
            updatePreview();
        }
    }, state.remote.refreshInterval);
}

function stopRemotePolling() {
    if (state.remote.intervalId) {
        clearInterval(state.remote.intervalId);
        state.remote.intervalId = null;
    }
    if (state.remote.wsConnection) {
        state.remote.wsConnection.close();
        state.remote.wsConnection = null;
    }
}

async function applyRemoteAndDeploy() {
    if (!state.remote.enabled) {
        showToast('Enable remote text first', 'warning');
        return;
    }
    const value = await fetchRemoteValue();
    if (value !== null && state.remote.targetField) {
        state.templateParams[state.remote.targetField] = value;
        const field = elements.formFields.querySelector(`[name="${state.remote.targetField}"]`);
        if (field) field.value = value;
        updatePreview();
        startRemotePolling();
        deployTemplate();
    } else {
        showToast('Could not fetch remote value', 'error');
    }
}

// ============================================================
// SELECT TEMPLATE
// ============================================================
function selectTemplate(templateKey) {
    const template = TEMPLATES[templateKey];
    if (!template) return;

    state.selectedTemplate = templateKey;

    document.querySelectorAll('.template-card').forEach(card => {
        card.classList.toggle('selected', card.dataset.template === templateKey);
    });

    elements.configPanel.classList.add('open');
    elements.configTitle.textContent = template.name;
    setTimeout(fixConfigBodyHeight, 50);

    // Initialize params with defaults
    state.templateParams = {};
    template.params.forEach(param => {
        state.templateParams[param.name] = param.default;
    });

    renderFormFields(template);
    updatePreview();
    renderPresetSelect();

    // Switch to params tab
    document.querySelectorAll('.config-tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.config-tab-content').forEach(c => c.classList.remove('active'));
    document.querySelector('.config-tab[data-tab="params"]').classList.add('active');
    document.getElementById('tab-params').classList.add('active');
}

// ============================================================
// RENDER FORM FIELDS
// ============================================================
function renderFormFields(template) {
    elements.formFields.innerHTML = template.params.map(param => {
        let inputHtml = '';

        switch (param.type) {
            case 'text':
                inputHtml = `<input type="text" name="${param.name}" class="form-input"
                    value="${escapeHtml(String(param.default || ''))}" ${param.required ? 'required' : ''}>`;
                break;
            case 'number':
                inputHtml = `<input type="number" name="${param.name}" class="form-input"
                    value="${param.default || 0}" title="${param.label}">`;
                break;
            case 'datetime':
                inputHtml = `<input type="datetime-local" name="${param.name}" class="form-input"
                    value="${param.default || ''}" title="${param.label}">`;
                break;
            case 'textarea':
                inputHtml = `<textarea name="${param.name}" class="form-textarea"
                    ${param.required ? 'required' : ''}>${escapeHtml(String(param.default || ''))}</textarea>`;
                break;
            case 'select':
                inputHtml = `<select name="${param.name}" class="form-select" title="${param.label}">
                    ${param.options.map(opt => `<option value="${opt}" ${opt === param.default ? 'selected' : ''}>${opt}</option>`).join('')}
                </select>`;
                break;
            case 'color':
                inputHtml = `
                    <div class="form-color">
                        <input type="color" name="${param.name}" value="#${param.default || 'ffffff'}" title="${param.label}">
                        <input type="text" name="${param.name}_hex" class="form-input" value="#${param.default || 'ffffff'}">
                    </div>`;
                break;
            case 'checkbox':
                inputHtml = `
                    <label class="toggle-switch" title="${param.label}">
                        <input type="checkbox" name="${param.name}" ${param.default ? 'checked' : ''}>
                        <span class="toggle-slider"></span>
                    </label>`;
                break;
        }

        return `
            <div class="form-group">
                <label class="form-label">${param.label}${param.required ? ' *' : ''}</label>
                ${inputHtml}
            </div>
        `;
    }).join('');

    // Add event listeners for real-time preview
    elements.formFields.querySelectorAll('input, select, textarea').forEach(field => {
        field.addEventListener('input', handleParamChange);
        field.addEventListener('change', handleParamChange);
    });
}

function escapeHtml(str) {
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// ============================================================
// HANDLE PARAM CHANGE
// ============================================================
function handleParamChange(e) {
    const name = e.target.name;
    let value = e.target.type === 'checkbox' ? e.target.checked : e.target.value;

    if (name.endsWith('_hex')) {
        const colorName = name.replace('_hex', '');
        if (value.startsWith('#')) {
            const colorInput = elements.formFields.querySelector(`input[type="color"][name="${colorName}"]`);
            if (colorInput) colorInput.value = value;
        }
        state.templateParams[colorName] = value.replace('#', '');
    } else {
        state.templateParams[name] = value;
    }

    // Debounced preview update
    clearTimeout(handleParamChange._timer);
    handleParamChange._timer = setTimeout(updatePreview, 300);
}

// ============================================================
// GENERATE TEMPLATE URL
// ============================================================
function generateTemplateUrl(overrideParams) {
    if (!state.selectedTemplate) return '';

    const params = new URLSearchParams();
    const templateParams = overrideParams || state.templateParams;

    Object.entries(templateParams).forEach(([key, value]) => {
        if (value !== '' && value !== undefined && value !== null) {
            if (key.endsWith('_hex')) return;
            params.append(key, value);
        }
    });

    // Add animation params
    if (state.animation.in !== 'none') {
        params.append('animIn', state.animation.in);
        params.append('animInDuration', state.animation.inDuration);
        params.append('animEasing', state.animation.easing);
    }
    if (state.animation.out !== 'none') {
        params.append('animOut', state.animation.out);
        params.append('animOutDuration', state.animation.outDuration);
    }
    if (state.animation.delay > 0) params.append('animDelay', state.animation.delay);
    if (state.animation.autoHide > 0) params.append('autoHide', state.animation.autoHide);
    if (state.animation.loop) params.append('loop', '1');

    const baseUrl = window.location.origin + window.location.pathname.replace('index.html', '');
    return `${baseUrl}${state.selectedTemplate}.html?${params.toString()}`;
}

// ============================================================
// UPDATE PREVIEW
// ============================================================
function updatePreview() {
    if (!state.selectedTemplate) return;
    const url = generateTemplateUrl();
    elements.previewFrame.src = url;
    elements.outputUrl.textContent = url;

    // Push to server preview channel so obs-preview.html updates live
    if (state.serverConnected) {
        fetch('/api/deploy', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                template: state.selectedTemplate,
                params: state.templateParams,
                animation: state.animation,
                channel: 'preview'
            })
        }).catch(() => { });
    }
}

function replayPreview() {
    if (!state.selectedTemplate) return;
    const url = generateTemplateUrl();
    elements.previewFrame.src = '';
    setTimeout(() => { elements.previewFrame.src = url; }, 50);
}

// ============================================================
// TAKE — send current preview config to program output
// ============================================================
function take() {
    if (!state.selectedTemplate) {
        showToast('Select a template first', 'warning');
        return;
    }
    if (!state.serverConnected) {
        showToast('Server not connected — start the server to use TAKE', 'error');
        return;
    }

    const template = TEMPLATES[state.selectedTemplate] || state.customTemplatesMap[state.selectedTemplate];
    fetch('/api/deploy', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
            template: state.selectedTemplate,
            params: state.templateParams,
            animation: state.animation,
            channel: 'program'
        })
    }).then(r => r.json()).then(data => {
        if (data.success) {
            state.outputChannels.program = data.deployment;
            updateChannelStatus();
            showToast(`TAKEN → ${template ? template.name : state.selectedTemplate}`, 'success');
            // Brief flash on the button
            const btn = document.getElementById('btnTake');
            if (btn) {
                btn.classList.add('take-flash');
                setTimeout(() => btn.classList.remove('take-flash'), 400);
            }
        }
    }).catch(() => showToast('TAKE failed — server not responding', 'error'));
}

// ============================================================
// COPY URL
// ============================================================
function copyUrl() {
    const url = elements.outputUrl.textContent;
    if (!url || url === 'No template selected') return;
    navigator.clipboard.writeText(url).then(() => {
        showToast('URL copied to clipboard', 'success');
    }).catch(() => {
        // Fallback
        const ta = document.createElement('textarea');
        ta.value = url;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        showToast('URL copied to clipboard', 'success');
    });
}

// ============================================================
// DEPLOY TEMPLATE
// ============================================================
function deployTemplate() {
    if (!state.selectedTemplate) return;

    const url = generateTemplateUrl();
    const template = TEMPLATES[state.selectedTemplate] || state.customTemplatesMap[state.selectedTemplate];
    const channel = state.outputChannel || 'main';

    // Open output window named by channel
    window.open(url, `cg_output_${channel}`, 'width=1920,height=1080');

    // Update channel state
    const deployment = {
        template: state.selectedTemplate,
        name: template ? template.name : state.selectedTemplate,
        params: { ...state.templateParams },
        animation: { ...state.animation },
        url,
        channel,
        timestamp: new Date().toISOString()
    };
    state.outputChannels[channel] = deployment;
    state.currentOutput = deployment;

    // Add to history
    state.deploymentHistory.unshift(deployment);
    if (state.deploymentHistory.length > 50) state.deploymentHistory.pop();
    localStorage.setItem('cg_history', JSON.stringify(state.deploymentHistory));

    // Update active output bar
    elements.activeOutputBar.style.display = 'flex';
    elements.activeTemplateName.textContent = `${deployment.name} [${channel}]`;

    updateChannelStatus();

    // Start remote polling if enabled
    if (state.remote.enabled) startRemotePolling();

    showToast(`Deployed: ${deployment.name} → ${channel}`, 'success');
    renderHistoryList();
}

function quickDeploy(templateKey) {
    const template = TEMPLATES[templateKey];
    if (!template) return;

    state.selectedTemplate = templateKey;
    state.templateParams = {};
    template.params.forEach(p => { state.templateParams[p.name] = p.default; });

    deployTemplate();
    showToast(`Quick deployed: ${template.name}`, 'success');
}

function clearOutput() {
    stopRemotePolling();
    const channel = state.outputChannel || 'main';
    state.outputChannels[channel] = null;

    // If main channel, update the top bar
    const anyActive = Object.values(state.outputChannels).some(v => v !== null);
    if (!anyActive) {
        state.currentOutput = null;
        elements.activeOutputBar.style.display = 'none';
        elements.activeTemplateName.textContent = '—';
    } else {
        // Show whichever other channel is still active
        const next = Object.values(state.outputChannels).find(v => v !== null);
        state.currentOutput = next;
    }

    updateChannelStatus();
    showToast(`Channel ${channel} cleared`, 'success');
}

// ============================================================
// MULTI-OUTPUT CHANNELS
// ============================================================
function selectChannel(channelId) {
    state.outputChannel = channelId;
    document.querySelectorAll('.channel-tab').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.channel === channelId);
    });
    updateChannelStatus();
}

function updateChannelStatus() {
    const activeCount = Object.values(state.outputChannels).filter(v => v !== null).length;
    const statusEl = document.getElementById('channelStatusBadge');
    if (statusEl) {
        statusEl.textContent = activeCount > 0 ? `${activeCount} active` : '';
        statusEl.style.display = activeCount > 0 ? 'inline-block' : 'none';
    }
    // Update monitor channel tabs if monitor is open
    renderMonitorChannelTabs();
}

function renderMonitorChannelTabs() {
    const tabsEl = document.getElementById('monitorChannelTabs');
    if (!tabsEl) return;
    const channels = ['main', 'secondary', 'web', 'preview', 'program'];
    tabsEl.innerHTML = channels.map(ch => {
        const d = state.outputChannels[ch];
        const label = ch.charAt(0).toUpperCase() + ch.slice(1);
        const active = d ? '●' : '○';
        return `<button class="monitor-ch-tab ${d ? 'has-content' : ''}" data-channel="${ch}">${active} ${label}</button>`;
    }).join('');
    tabsEl.querySelectorAll('.monitor-ch-tab').forEach(btn => {
        btn.addEventListener('click', () => showMonitorChannel(btn.dataset.channel));
    });
}

function showMonitorChannel(channel) {
    document.querySelectorAll('.monitor-ch-tab').forEach(b => b.classList.toggle('active', b.dataset.channel === channel));
    const frame = document.getElementById('monitorFrame');
    const info = document.getElementById('monitorInfo');
    const d = state.outputChannels[channel];
    if (d && frame) {
        frame.src = d.url;
        if (info) info.textContent = `Active: ${d.name} — ${new Date(d.timestamp).toLocaleTimeString()}`;
    } else {
        if (frame) frame.src = '';
        if (info) info.textContent = `Channel "${channel}" has no active output`;
    }
}

// ============================================================
// RUNDOWN / PLAYLIST
// ============================================================
async function loadPlaylists() {
    try {
        const res = await fetch('/api/playlists');
        if (!res.ok) return;
        const data = await res.json();
        state.playlists = data.playlists || [];
        if (data.active) {
            state.activePlaylistId = data.active.id;
            state.activePlaylistIndex = data.active.index;
        }
        renderRundownModal();
    } catch { /* standalone mode */ }
}

function openRundown() {
    document.getElementById('rundownModal').classList.add('open');
    loadPlaylists();
}

function renderRundownModal() {
    const select = document.getElementById('rundownPlaylistSelect');
    const list = document.getElementById('rundownItemList');
    if (!select || !list) return;

    select.innerHTML = state.playlists.length === 0
        ? '<option value="">— No playlists —</option>'
        : state.playlists.map(pl => `<option value="${pl.id}">${pl.name} (${pl.items.length} items)</option>`).join('');

    if (state.playlists.length > 0 && !state.activePlaylistId) {
        state.activePlaylistId = state.playlists[0].id;
    }
    if (state.activePlaylistId && select.value !== state.activePlaylistId) {
        select.value = state.activePlaylistId;
    }

    renderRundownItems();
}

function renderRundownItems() {
    const list = document.getElementById('rundownItemList');
    if (!list) return;
    const pl = state.playlists.find(p => p.id === state.activePlaylistId);
    if (!pl || pl.items.length === 0) {
        list.innerHTML = '<div class="rundown-empty">No items. Configure a template and click "Add to Rundown".</div>';
        return;
    }
    list.innerHTML = pl.items.map((item, idx) => {
        const t = TEMPLATES[item.template] || state.customTemplatesMap[item.template] || {};
        const isCurrent = state.activePlaylistIndex === idx;
        return `<div class="rundown-item ${isCurrent ? 'active' : ''}" data-idx="${idx}">
            <span class="rundown-num">${idx + 1}</span>
            <div class="rundown-info">
                <span class="rundown-template">${t.name || item.template}</span>
                <span class="rundown-params">${Object.entries(item.params || {}).filter(([, v]) => v).slice(0, 2).map(([k, v]) => `${k}: ${String(v).substring(0, 20)}`).join(' · ')}</span>
            </div>
            <button class="rundown-remove" data-idx="${idx}" title="Remove">✕</button>
        </div>`;
    }).join('');
    list.querySelectorAll('.rundown-remove').forEach(btn => {
        btn.addEventListener('click', (e) => { e.stopPropagation(); removeRundownItem(parseInt(btn.dataset.idx)); });
    });
    list.querySelectorAll('.rundown-item').forEach(el => {
        el.addEventListener('dblclick', () => jumpToRundownItem(parseInt(el.dataset.idx)));
    });
}

async function createPlaylist() {
    const name = prompt('Playlist name:');
    if (!name) return;
    try {
        const res = await fetch('/api/playlists', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name, items: [] }) });
        const data = await res.json();
        if (data.success) {
            state.playlists.push(data.playlist);
            state.activePlaylistId = data.playlist.id;
            renderRundownModal();
            showToast(`Created: ${name}`, 'success');
        }
    } catch { showToast('Server not running', 'error'); }
}

async function deleteActivePlaylist() {
    if (!state.activePlaylistId) return;
    if (!confirm('Delete this playlist?')) return;
    try {
        await fetch(`/api/playlists/${state.activePlaylistId}`, { method: 'DELETE' });
        state.playlists = state.playlists.filter(p => p.id !== state.activePlaylistId);
        state.activePlaylistId = state.playlists.length > 0 ? state.playlists[0].id : null;
        state.activePlaylistIndex = -1;
        renderRundownModal();
        showToast('Playlist deleted', 'success');
    } catch { showToast('Server not running', 'error'); }
}

async function addToRundown() {
    if (!state.selectedTemplate) { showToast('Select a template first', 'warning'); return; }
    if (!state.activePlaylistId) {
        const name = prompt('Create a playlist first. Playlist name:');
        if (!name) return;
        try {
            const res = await fetch('/api/playlists', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name, items: [] }) });
            const data = await res.json();
            if (data.success) { state.playlists.push(data.playlist); state.activePlaylistId = data.playlist.id; }
        } catch { showToast('Server not running', 'error'); return; }
    }
    const pl = state.playlists.find(p => p.id === state.activePlaylistId);
    if (!pl) return;
    pl.items.push({ template: state.selectedTemplate, params: { ...state.templateParams } });
    try {
        await fetch(`/api/playlists/${state.activePlaylistId}`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ items: pl.items }) });
    } catch { /* offline */ }
    renderRundownItems();
    showToast('Added to rundown', 'success');
}

async function removeRundownItem(idx) {
    const pl = state.playlists.find(p => p.id === state.activePlaylistId);
    if (!pl) return;
    pl.items.splice(idx, 1);
    if (state.activePlaylistIndex >= pl.items.length) state.activePlaylistIndex = pl.items.length - 1;
    try {
        await fetch(`/api/playlists/${state.activePlaylistId}`, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ items: pl.items }) });
    } catch { /* offline */ }
    renderRundownItems();
}

async function playRundown() {
    if (!state.activePlaylistId) { showToast('No playlist selected', 'warning'); return; }
    try {
        const ch = state.outputChannel || 'main';
        const res = await fetch(`/api/playlists/${state.activePlaylistId}/play?channel=${ch}`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.activePlaylistIndex = data.index;
            if (data.deployment) {
                window.open(data.deployment.url, `cg_output_${ch}`, 'width=1920,height=1080');
                state.outputChannels[ch] = data.deployment;
                updateChannelStatus();
            }
            renderRundownItems();
            showToast('Rundown started', 'success');
        }
    } catch { showToast('Server not running', 'error'); }
}

async function nextRundownItem() {
    if (!state.activePlaylistId) return;
    try {
        const res = await fetch(`/api/playlists/${state.activePlaylistId}/next`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.activePlaylistIndex = data.index;
            const ch = state.outputChannel || 'main';
            if (data.deployment) {
                window.open(data.deployment.url, `cg_output_${ch}`, 'width=1920,height=1080');
                state.outputChannels[ch] = data.deployment;
                updateChannelStatus();
            }
            renderRundownItems();
            if (data.atEnd) showToast('End of rundown', 'info');
        }
    } catch { showToast('Server not running', 'error'); }
}

async function prevRundownItem() {
    if (!state.activePlaylistId) return;
    try {
        const res = await fetch(`/api/playlists/${state.activePlaylistId}/prev`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.activePlaylistIndex = data.index;
            const ch = state.outputChannel || 'main';
            if (data.deployment) {
                window.open(data.deployment.url, `cg_output_${ch}`, 'width=1920,height=1080');
                state.outputChannels[ch] = data.deployment;
                updateChannelStatus();
            }
            renderRundownItems();
        }
    } catch { showToast('Server not running', 'error'); }
}

async function stopRundown() {
    if (!state.activePlaylistId) return;
    try {
        await fetch(`/api/playlists/${state.activePlaylistId}/stop`, { method: 'POST' });
        state.activePlaylistIndex = -1;
        renderRundownItems();
        showToast('Rundown stopped', 'success');
    } catch { showToast('Server not running', 'error'); }
}

async function jumpToRundownItem(idx) {
    if (!state.activePlaylistId) return;
    try {
        const ch = state.outputChannel || 'main';
        const res = await fetch(`/api/playlists/${state.activePlaylistId}/play?index=${idx}&channel=${ch}`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.activePlaylistIndex = data.index;
            if (data.deployment) {
                window.open(data.deployment.url, `cg_output_${ch}`, 'width=1920,height=1080');
                state.outputChannels[ch] = data.deployment;
                updateChannelStatus();
            }
            renderRundownItems();
        }
    } catch { showToast('Server not running', 'error'); }
}

function setupKeyboardShortcuts() {
    document.addEventListener('keydown', (e) => {
        // Only fire if not typing in an input
        if (['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement.tagName)) return;
        if (e.key === 'F1') { e.preventDefault(); playRundown(); }
        if (e.key === 'F2') { e.preventDefault(); nextRundownItem(); }
        if (e.key === 'F3') { e.preventDefault(); prevRundownItem(); }
        if (e.key === 'F4') { e.preventDefault(); stopRundown(); }
        if (e.key === 't' || e.key === 'T') { e.preventDefault(); take(); }
    });
}

// ============================================================
// CUSTOM TEMPLATE BUILDER
// ============================================================
async function loadCustomTemplates() {
    try {
        const res = await fetch('/api/templates/custom');
        if (!res.ok) return;
        const data = await res.json();
        (data.templates || []).forEach(t => mergeCustomTemplate(t));
        renderTemplates();
    } catch { /* standalone */ }
}

function mergeCustomTemplate(t) {
    state.customTemplatesMap[t.id] = t;
    TEMPLATES[t.id] = {
        name: t.name,
        category: t.category,
        icon: t.icon || '⬜',
        description: t.description || 'Custom template',
        params: (t.params || []).map(f => ({ name: f.name, label: f.label || f.name, type: f.type || 'text', default: f.default || '' })),
        isCustom: true
    };
}

function openTemplateBuilder(editId) {
    state.builderStep = 1;
    state.builderEditId = editId || null;
    if (editId && state.customTemplatesMap && state.customTemplatesMap[editId]) {
        const t = state.customTemplatesMap[editId];
        state.builderDraft = {
            name: t.name, category: t.category, icon: t.icon || '⬜',
            description: t.description || '', layout: t.layout || 'lower-third',
            params: JSON.parse(JSON.stringify(t.params || [])),
            style: JSON.parse(JSON.stringify(t.style || { accent: '00d9ff', bg: '0a0f1e', text: 'ffffff', font: 'Inter' }))
        };
    } else {
        state.builderDraft = { name: '', category: 'custom', icon: '⬜', description: '', layout: 'lower-third', params: [], style: { accent: '00d9ff', bg: '0a0f1e', text: 'ffffff', font: 'Inter' } };
    }
    document.getElementById('builderModal').classList.add('open');
    renderBuilderStep();
}

function renderBuilderStep() {
    const step = state.builderStep;
    document.querySelectorAll('.builder-step-dot').forEach((d, i) => {
        d.classList.toggle('active', i + 1 === step);
        d.classList.toggle('done', i + 1 < step);
    });
    document.getElementById('builderBackBtn').style.display = step > 1 ? 'inline-block' : 'none';
    document.getElementById('builderNextBtn').textContent = step === 4 ? 'Save & Publish' : 'Next →';

    const body = document.getElementById('builderStepBody');
    if (step === 1) {
        body.innerHTML = `
            <div class="builder-section">
                <div class="form-group"><label class="form-label">Template Name *</label><input class="form-input" id="bldrName" value="${escHtml(state.builderDraft.name)}" placeholder="My Custom Graphic"></div>
                <div class="form-group"><label class="form-label">Category</label>
                    <select class="form-select" id="bldrCategory">
                        ${['custom', 'lower-thirds', 'banners', 'fullscreen', 'tickers', 'score-bugs', 'bugs', 'social', 'special', 'election', 'streaming', 'advertising', 'sports-extended'].map(c => `<option value="${c}" ${c === state.builderDraft.category ? 'selected' : ''}>${formatCategory(c)}</option>`).join('')}
                    </select></div>
                <div class="form-group"><label class="form-label">Icon (emoji)</label><input class="form-input" id="bldrIcon" value="${escHtml(state.builderDraft.icon)}" placeholder="⬜" style="width:80px;font-size:24px"></div>
                <div class="form-group"><label class="form-label">Description</label><input class="form-input" id="bldrDesc" value="${escHtml(state.builderDraft.description)}" placeholder="Short description"></div>
            </div>`;
    } else if (step === 2) {
        const layouts = [
            { id: 'lower-third', label: 'Lower Third', icon: '▭', desc: 'Bottom-left overlay' },
            { id: 'banner', label: 'Banner', icon: '▬', desc: 'Top-full banner' },
            { id: 'bug', label: 'Bug / Corner', icon: '◉', desc: 'Corner bug' },
            { id: 'fullscreen', label: 'Fullscreen', icon: '⬜', desc: 'Full canvas' },
            { id: 'ticker', label: 'Ticker', icon: '↔', desc: 'Scrolling bottom' }
        ];
        body.innerHTML = `
            <div class="builder-section">
                <div class="form-label" style="margin-bottom:12px">Choose a base layout:</div>
                <div class="builder-layout-grid">
                    ${layouts.map(l => `
                        <div class="builder-layout-tile ${l.id === state.builderDraft.layout ? 'selected' : ''}" data-layout="${l.id}">
                            <span class="layout-tile-icon">${l.icon}</span>
                            <div class="layout-tile-label">${l.label}</div>
                            <div class="layout-tile-desc">${l.desc}</div>
                        </div>`).join('')}
                </div>
            </div>`;
        body.querySelectorAll('.builder-layout-tile').forEach(tile => {
            tile.addEventListener('click', () => {
                state.builderDraft.layout = tile.dataset.layout;
                body.querySelectorAll('.builder-layout-tile').forEach(t => t.classList.toggle('selected', t === tile));
            });
        });
    } else if (step === 3) {
        body.innerHTML = `
            <div class="builder-section">
                <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px">
                    <div class="form-label">Fields (parameters)</div>
                    <button class="btn btn-small" id="bldrAddField">+ Add Field</button>
                </div>
                <div id="bldrFieldList">${renderBuilderFields()}</div>
            </div>`;
        document.getElementById('bldrAddField').addEventListener('click', () => {
            state.builderDraft.params.push({ name: `field${state.builderDraft.params.length + 1}`, label: 'New Field', type: 'text', default: '' });
            document.getElementById('bldrFieldList').innerHTML = renderBuilderFields();
            attachBuilderFieldListeners();
        });
        attachBuilderFieldListeners();
    } else if (step === 4) {
        const s = state.builderDraft.style;
        body.innerHTML = `
            <div class="builder-section">
                <div class="form-group"><label class="form-label">Accent Color</label><div style="display:flex;gap:8px;align-items:center"><input type="color" id="bldrAccent" value="#${s.accent || '00d9ff'}" style="width:48px;height:36px;border:none;background:none;cursor:pointer"><input class="form-input" id="bldrAccentHex" value="${s.accent || '00d9ff'}" style="width:100px"></div></div>
                <div class="form-group"><label class="form-label">Background Color</label><div style="display:flex;gap:8px;align-items:center"><input type="color" id="bldrBg" value="#${s.bg || '0a0f1e'}" style="width:48px;height:36px;border:none;background:none;cursor:pointer"><input class="form-input" id="bldrBgHex" value="${s.bg || '0a0f1e'}" style="width:100px"></div></div>
                <div class="form-group"><label class="form-label">Text Color</label><div style="display:flex;gap:8px;align-items:center"><input type="color" id="bldrText" value="#${s.text || 'ffffff'}" style="width:48px;height:36px;border:none;background:none;cursor:pointer"><input class="form-input" id="bldrTextHex" value="${s.text || 'ffffff'}" style="width:100px"></div></div>
                <div class="form-group"><label class="form-label">Font Family</label>
                    <select class="form-select" id="bldrFont">
                        ${['Inter', 'Roboto', 'Oswald', 'Bebas Neue', 'Montserrat', 'Arial'].map(f => `<option value="${f}" ${f === s.font ? 'selected' : ''}>${f}</option>`).join('')}
                    </select></div>
                <div class="form-group"><label class="form-label">Preview</label>
                    <div class="builder-preview-wrap"><iframe id="bldrPreviewFrame" style="width:100%;height:180px;border:1px solid rgba(255,255,255,0.1);border-radius:6px;background:#000" src=""></iframe>
                    <button class="btn btn-small" style="margin-top:6px" id="bldrPreviewBtn">Refresh Preview</button></div>
                </div>
            </div>`;
        // Sync color pickers ↔ hex inputs
        ['Accent', 'Bg', 'Text'].forEach(name => {
            const picker = document.getElementById(`bldr${name}`);
            const hex = document.getElementById(`bldr${name}Hex`);
            const key = name.toLowerCase() === 'accent' ? 'accent' : name.toLowerCase() === 'bg' ? 'bg' : 'text';
            picker.addEventListener('input', () => { hex.value = picker.value.replace('#', ''); state.builderDraft.style[key] = hex.value; });
            hex.addEventListener('input', () => { picker.value = '#' + hex.value.replace('#', ''); state.builderDraft.style[key] = hex.value; });
        });
        document.getElementById('bldrFont').addEventListener('change', (e) => { state.builderDraft.style.font = e.target.value; });
        document.getElementById('bldrPreviewBtn').addEventListener('click', refreshBuilderPreview);
        refreshBuilderPreview();
    }
}

function renderBuilderFields() {
    if (state.builderDraft.params.length === 0) {
        return '<div class="rundown-empty">No fields yet. Click "+ Add Field".</div>';
    }
    return state.builderDraft.params.map((f, i) => `
        <div class="builder-field-row" data-idx="${i}">
            <div style="display:flex;gap:6px;flex:1;flex-wrap:wrap">
                <input class="form-input bldr-fname" data-idx="${i}" placeholder="name" value="${escHtml(f.name)}" style="width:100px">
                <input class="form-input bldr-flabel" data-idx="${i}" placeholder="label" value="${escHtml(f.label || f.name)}" style="width:120px">
                <select class="form-select bldr-ftype" data-idx="${i}" style="width:90px">
                    ${['text', 'number', 'color', 'select', 'checkbox', 'textarea'].map(t => `<option value="${t}" ${t === f.type ? 'selected' : ''}>${t}</option>`).join('')}
                </select>
                <input class="form-input bldr-fdefault" data-idx="${i}" placeholder="default" value="${escHtml(f.default || '')}" style="width:120px">
            </div>
            <button class="rundown-remove bldr-fremove" data-idx="${i}" title="Remove">✕</button>
        </div>`).join('');
}

function attachBuilderFieldListeners() {
    document.querySelectorAll('.bldr-fname').forEach(el => {
        el.addEventListener('input', () => { state.builderDraft.params[+el.dataset.idx].name = el.value; });
    });
    document.querySelectorAll('.bldr-flabel').forEach(el => {
        el.addEventListener('input', () => { state.builderDraft.params[+el.dataset.idx].label = el.value; });
    });
    document.querySelectorAll('.bldr-ftype').forEach(el => {
        el.addEventListener('change', () => { state.builderDraft.params[+el.dataset.idx].type = el.value; });
    });
    document.querySelectorAll('.bldr-fdefault').forEach(el => {
        el.addEventListener('input', () => { state.builderDraft.params[+el.dataset.idx].default = el.value; });
    });
    document.querySelectorAll('.bldr-fremove').forEach(el => {
        el.addEventListener('click', () => {
            state.builderDraft.params.splice(+el.dataset.idx, 1);
            document.getElementById('bldrFieldList').innerHTML = renderBuilderFields();
            attachBuilderFieldListeners();
        });
    });
}

async function refreshBuilderPreview() {
    // Save style from form
    ['Accent', 'Bg', 'Text'].forEach(n => {
        const key = n.toLowerCase() === 'accent' ? 'accent' : n.toLowerCase() === 'bg' ? 'bg' : 'text';
        const hexEl = document.getElementById(`bldr${n}Hex`);
        if (hexEl) state.builderDraft.style[key] = hexEl.value;
    });
    const fontEl = document.getElementById('bldrFont');
    if (fontEl) state.builderDraft.style.font = fontEl.value;

    try {
        const payload = { ...state.builderDraft, preview: true };
        const res = await fetch('/api/templates/custom', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
        const data = await res.json();
        if (data.success) {
            const frame = document.getElementById('bldrPreviewFrame');
            if (frame) frame.src = `/api/templates/custom/${data.template.id}/html`;
            // Delete the temp template after a moment
            setTimeout(() => fetch(`/api/templates/custom/${data.template.id}`, { method: 'DELETE' }).catch(() => { }), 10000);
        }
    } catch { /* standalone */ }
}

function builderNext() {
    const step = state.builderStep;
    // Validate / collect step data
    if (step === 1) {
        const name = document.getElementById('bldrName');
        if (!name || !name.value.trim()) { showToast('Please enter a template name', 'warning'); return; }
        state.builderDraft.name = name.value.trim();
        state.builderDraft.category = document.getElementById('bldrCategory').value;
        state.builderDraft.icon = document.getElementById('bldrIcon').value || '⬜';
        state.builderDraft.description = document.getElementById('bldrDesc').value;
    }
    if (step < 4) {
        state.builderStep++;
        renderBuilderStep();
    } else {
        saveCustomTemplate();
    }
}

function builderBack() {
    if (state.builderStep > 1) { state.builderStep--; renderBuilderStep(); }
}

async function saveCustomTemplate() {
    // Collect final style values
    ['Accent', 'Bg', 'Text'].forEach(n => {
        const key = n.toLowerCase() === 'accent' ? 'accent' : n.toLowerCase() === 'bg' ? 'bg' : 'text';
        const hexEl = document.getElementById(`bldr${n}Hex`);
        if (hexEl) state.builderDraft.style[key] = hexEl.value;
    });
    const fontEl = document.getElementById('bldrFont');
    if (fontEl) state.builderDraft.style.font = fontEl.value;

    try {
        const isEdit = !!state.builderEditId;
        const url = isEdit ? `/api/templates/custom/${state.builderEditId}` : '/api/templates/custom';
        const method = isEdit ? 'PUT' : 'POST';
        const res = await fetch(url, { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(state.builderDraft) });
        const data = await res.json();
        if (data.success) {
            mergeCustomTemplate(data.template);
            renderTemplates();
            document.getElementById('builderModal').classList.remove('open');
            showToast(`Template "${state.builderDraft.name}" published!`, 'success');
        }
    } catch { showToast('Server not running — template not saved', 'error'); }
}

async function deleteCustomTemplate(id) {
    if (!confirm('Delete this custom template?')) return;
    try {
        await fetch(`/api/templates/custom/${id}`, { method: 'DELETE' });
        delete TEMPLATES[id];
        if (state.customTemplatesMap) delete state.customTemplatesMap[id];
        renderTemplates();
        showToast('Custom template deleted', 'success');
    } catch { showToast('Server not running', 'error'); }
}

function escHtml(str) {
    return String(str || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// ============================================================
// FULLSCREEN PREVIEW
// ============================================================
function fullscreenPreview() {
    const url = elements.outputUrl.textContent;
    if (url && url !== 'No template selected') {
        window.open(url, '_blank', 'width=1920,height=1080,fullscreen=yes');
    }
}

// ============================================================
// CLOSE CONFIG PANEL
// ============================================================
function closeConfigPanel() {
    elements.configPanel.classList.remove('open');
    state.selectedTemplate = null;
    document.querySelectorAll('.template-card').forEach(card => card.classList.remove('selected'));
    stopRemotePolling();
}

// ============================================================
// OUTPUT MONITOR
// ============================================================
function openOutputMonitor() {
    const modal = document.getElementById('outputMonitorModal');
    modal.classList.add('open');
    renderMonitorChannelTabs();
    showMonitorChannel(state.outputChannel || 'main');
    renderHistoryList();
}

// ============================================================
// DEPLOYMENT HISTORY
// ============================================================
function renderHistoryList() {
    const list = document.getElementById('historyList');
    if (!list) return;

    if (state.deploymentHistory.length === 0) {
        list.innerHTML = '<div class="empty-state">No deployments yet</div>';
        return;
    }

    list.innerHTML = state.deploymentHistory.slice(0, 20).map((item, idx) => `
        <div class="history-item" data-idx="${idx}">
            <div class="history-template">${item.name || item.template}</div>
            <div class="history-time">${new Date(item.timestamp).toLocaleTimeString()}</div>
            <span class="history-redeploy" data-idx="${idx}">↺ Redeploy</span>
        </div>
    `).join('');

    list.querySelectorAll('.history-redeploy').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            const item = state.deploymentHistory[parseInt(btn.dataset.idx)];
            if (item) {
                state.selectedTemplate = item.template;
                state.templateParams = { ...item.params };
                if (item.animation) state.animation = { ...item.animation };
                deployTemplate();
                showToast(`Redeployed: ${item.name || item.template}`, 'success');
            }
        });
    });
}

// ============================================================
// ANIMATION PRESETS
// ============================================================
function renderPresetGrid() {
    const grid = document.getElementById('presetGrid');
    if (!grid) return;

    grid.innerHTML = ANIMATION_PRESETS.map(preset => `
        <div class="preset-card" data-preset="${preset.id}">
            <span class="preset-icon">${preset.icon}</span>
            <div class="preset-name">${preset.name}</div>
            <div class="preset-desc">${preset.in} → ${preset.out}</div>
        </div>
    `).join('');

    grid.querySelectorAll('.preset-card').forEach(card => {
        card.addEventListener('click', () => {
            const preset = ANIMATION_PRESETS.find(p => p.id === card.dataset.preset);
            if (!preset) return;
            applyAnimationPreset(preset);
            document.getElementById('animPresetsModal').classList.remove('open');
            showToast(`Applied preset: ${preset.name}`, 'success');
        });
    });
}

function applyAnimationPreset(preset) {
    state.animation.in = preset.in;
    state.animation.out = preset.out;
    state.animation.inDuration = preset.duration;
    state.animation.outDuration = Math.round(preset.duration * 0.7);
    state.animation.easing = preset.easing;

    // Update UI
    document.querySelectorAll('#animInGrid .anim-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.anim === preset.in);
    });
    document.querySelectorAll('#animOutGrid .anim-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.anim === preset.out);
    });
    document.getElementById('animInDuration').value = preset.duration;
    document.getElementById('animOutDuration').value = Math.round(preset.duration * 0.7);
    document.getElementById('animEasing').value = preset.easing;

    updatePreview();
}

// ============================================================
// DATA SOURCES
// ============================================================
function saveDataSource() {
    const name = document.getElementById('dsName').value.trim();
    const type = document.getElementById('dsType').value;
    const url = document.getElementById('dsUrl').value.trim();
    const jsonPath = document.getElementById('dsJsonPath').value.trim();
    const refresh = parseInt(document.getElementById('dsRefresh').value) || 30;

    if (!name) { showToast('Please enter a name', 'warning'); return; }
    if (!url) { showToast('Please enter a URL or value', 'warning'); return; }

    const source = { id: Date.now(), name, type, url, jsonPath, refresh, active: false };
    state.dataSources.push(source);
    localStorage.setItem('cg_data_sources', JSON.stringify(state.dataSources));

    // Clear form
    document.getElementById('dsName').value = '';
    document.getElementById('dsUrl').value = '';
    document.getElementById('dsJsonPath').value = '';

    renderDataSourceList();
    showToast(`Data source "${name}" saved`, 'success');
}

function renderDataSourceList() {
    const list = document.getElementById('dataSourceList');
    if (!list) return;

    if (state.dataSources.length === 0) {
        list.innerHTML = '<div class="empty-state">No data sources configured</div>';
        return;
    }

    list.innerHTML = state.dataSources.map(source => `
        <div class="data-source-item ${source.active ? 'active' : ''}" data-id="${source.id}">
            <div>
                <div class="data-source-name">${source.name}</div>
                <div class="data-source-type">${source.type}</div>
                <div class="data-source-value">${source.url}</div>
            </div>
            <div style="display:flex;gap:6px;flex-shrink:0;">
                <button class="btn btn-small" onclick="useDataSource(${source.id})">Use</button>
                <button class="btn btn-small btn-danger-sm" onclick="deleteDataSource(${source.id})">✕</button>
            </div>
        </div>
    `).join('');
}

function useDataSource(id) {
    const source = state.dataSources.find(s => s.id === id);
    if (!source) return;

    // Apply to remote text config
    document.getElementById('remoteTextEnabled').checked = true;
    state.remote.enabled = true;
    document.getElementById('remoteUrl').value = source.url;
    state.remote.url = source.url;
    document.getElementById('remoteJsonPath').value = source.jsonPath || '';
    state.remote.jsonPath = source.jsonPath || '';
    document.getElementById('remoteRefreshInterval').value = source.refresh * 1000;
    state.remote.refreshInterval = source.refresh * 1000;

    document.getElementById('remoteTextModal').classList.remove('open');
    showToast(`Data source "${source.name}" applied`, 'success');
}

function deleteDataSource(id) {
    state.dataSources = state.dataSources.filter(s => s.id !== id);
    localStorage.setItem('cg_data_sources', JSON.stringify(state.dataSources));
    renderDataSourceList();
    showToast('Data source deleted', 'success');
}

// ============================================================
// SERVER CONNECTION CHECK
// ============================================================
async function checkServerConnection() {
    try {
        const response = await fetch('/api/templates', { signal: AbortSignal.timeout(2000) });
        if (response.ok) {
            state.serverConnected = true;
            elements.statusDot.classList.add('connected');
            elements.statusText.textContent = 'Server Connected';
            elements.wsStatus.textContent = 'ws://localhost:8060';
        } else {
            throw new Error('Server error');
        }
    } catch {
        state.serverConnected = false;
        elements.statusDot.classList.remove('connected');
        elements.statusText.textContent = 'Standalone Mode';
        elements.wsStatus.textContent = '';
    }
}

// ============================================================
// PERSISTENT WEBSOCKET (NDI / As-Run / PGM events)
// ============================================================

let _globalWs = null;

function connectGlobalWs() {
    if (_globalWs && _globalWs.readyState <= 1) return;
    try {
        _globalWs = new WebSocket(`ws://${location.hostname}:8060`);
    } catch (e) { return; }

    _globalWs.onopen = () => console.log('[WS] Connected to server');

    _globalWs.onmessage = (evt) => {
        let msg;
        try { msg = JSON.parse(evt.data); } catch { return; }

        switch (msg.type) {
            case 'ndi-cut':
            case 'ndi-auto':
                state.ndi.programSource = msg.program;
                state.ndi.previewSource = msg.preview;
                renderNdiSources();
                // Update PGM label
                document.dispatchEvent(new CustomEvent('ndi-pgm-changed', {
                    detail: { sourceId: msg.program }
                }));
                break;

            case 'ndi-sources-updated':
                if (msg.sources) {
                    state.ndi.sources = msg.sources;
                    renderNdiSources();
                }
                break;

            case 'as-run-updated':
                asRunState.schedule = msg.schedule || [];
                renderAsRunEvents();
                break;

            case 'as-run-started':
                asRunState.isRunning = true;
                updateAsRunToolbarState();
                break;

            case 'as-run-paused':
            case 'as-run-stopped':
                asRunState.isRunning = false;
                if (msg.type === 'as-run-stopped') asRunState.currentEventIndex = -1;
                updateAsRunToolbarState();
                renderAsRunEvents();
                break;

            case 'as-run-event-fired':
                showToast(`As-Run: ${msg.event?.action} at ${msg.event?.time}`, 'info');
                break;

            case 'pgm-normalization':
                // Sync gain slider if PGM monitor is open
                const slider = document.getElementById('pgmGainSlider');
                const valEl = document.getElementById('pgmGainValue');
                if (slider && msg.gain !== undefined) {
                    slider.value = msg.gain;
                    if (valEl) valEl.textContent = msg.gain + ' dB';
                }
                break;

            case 'cg-marriage-updated':
                if (msg.slots) {
                    msg.slots.forEach(s => {
                        const idx = s.slotNum - 1;
                        if (cgSlotState[idx]) Object.assign(cgSlotState[idx], s);
                    });
                    renderCGSlots();
                }
                break;
        }
    };

    _globalWs.onclose = () => {
        _globalWs = null;
        setTimeout(connectGlobalWs, 3000);
    };

    _globalWs.onerror = () => {
        _globalWs?.close();
    };
}

// ============================================================
// TOAST NOTIFICATIONS
// ============================================================
function showToast(message, type = 'success') {
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;

    const icons = { success: '✓', error: '✕', warning: '⚠', info: 'ℹ' };

    toast.innerHTML = `
        <span class="toast-icon">${icons[type] || '•'}</span>
        <span class="toast-message">${message}</span>
    `;

    elements.toastContainer.appendChild(toast);

    setTimeout(() => {
        toast.style.animation = 'slideIn 0.3s ease reverse';
        setTimeout(() => toast.remove(), 300);
    }, 3000);
}

// ============================================================
// PRESETS
// ============================================================
function loadPresets() {
    return JSON.parse(localStorage.getItem('cg_presets') || '[]');
}

function renderPresetSelect() {
    const select = document.getElementById('presetSelect');
    if (!select) return;
    const presets = loadPresets();
    select.innerHTML = '<option value="">— load preset —</option>' +
        presets.map(p => `<option value="${p.id}">${p.name} (${p.templateName})</option>`).join('');
}

function saveCurrentAsPreset() {
    if (!state.selectedTemplate) {
        showToast('Select a template first', 'warning');
        return;
    }
    const template = TEMPLATES[state.selectedTemplate] || state.customTemplatesMap[state.selectedTemplate];
    const defaultName = template ? template.name : state.selectedTemplate;
    const name = prompt('Preset name:', defaultName);
    if (!name || !name.trim()) return;

    const presets = loadPresets();
    const preset = {
        id: Date.now(),
        name: name.trim(),
        template: state.selectedTemplate,
        templateName: defaultName,
        params: { ...state.templateParams },
        animation: { ...state.animation },
        created: new Date().toISOString()
    };
    presets.unshift(preset);
    if (presets.length > 100) presets.pop();
    localStorage.setItem('cg_presets', JSON.stringify(presets));
    renderPresetSelect();
    showToast(`Preset "${name.trim()}" saved`, 'success');
}

function loadSelectedPreset() {
    const select = document.getElementById('presetSelect');
    if (!select || !select.value) return;

    const presets = loadPresets();
    const preset = presets.find(p => p.id === parseInt(select.value));
    if (!preset) return;

    const apply = () => {
        state.templateParams = { ...preset.params };
        Object.entries(preset.params).forEach(([name, value]) => {
            const el = elements.formFields.querySelector(`[name="${name}"]`);
            if (!el) return;
            if (el.type === 'checkbox') el.checked = !!value;
            else el.value = value;
            if (el.type === 'color') {
                const hexEl = elements.formFields.querySelector(`[name="${name}_hex"]`);
                if (hexEl) hexEl.value = '#' + String(value).replace('#', '');
            }
        });
        if (preset.animation) {
            Object.assign(state.animation, preset.animation);
            document.querySelectorAll('#animInGrid .anim-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.anim === state.animation.in);
            });
            document.querySelectorAll('#animOutGrid .anim-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.anim === state.animation.out);
            });
            const durEl = document.getElementById('animInDuration');
            if (durEl) durEl.value = state.animation.inDuration;
            const easEl = document.getElementById('animEasing');
            if (easEl) easEl.value = state.animation.easing;
        }
        updatePreview();
        showToast(`Loaded: ${preset.name}`, 'success');
    };

    if (preset.template !== state.selectedTemplate) {
        selectTemplate(preset.template);
        setTimeout(apply, 150);
    } else {
        apply();
    }
}

function deleteSelectedPreset() {
    const select = document.getElementById('presetSelect');
    if (!select || !select.value) {
        showToast('Select a preset to delete', 'warning');
        return;
    }
    let presets = loadPresets();
    const preset = presets.find(p => p.id === parseInt(select.value));
    if (!preset) return;
    if (!confirm(`Delete preset "${preset.name}"?`)) return;
    presets = presets.filter(p => p.id !== parseInt(select.value));
    localStorage.setItem('cg_presets', JSON.stringify(presets));
    renderPresetSelect();
    showToast('Preset deleted', 'success');
}

// ============================================================
// NDI SOURCE SWITCHER - PROGRAM / PREVIEW LOGIC
// ============================================================

function setupNdiModal() {
    const modal = document.getElementById('ndiSwitcherModal');
    if (!modal) return;

    // Refresh sources
    document.getElementById('btnRefreshNdiSources')?.addEventListener('click', refreshNdiSources);

    // CUT and AUTO buttons
    document.getElementById('btnCut')?.addEventListener('click', ndiCut);
    document.getElementById('btnAuto')?.addEventListener('click', ndiAuto);

    // Transition controls
    document.getElementById('transitionType')?.addEventListener('change', (e) => {
        state.ndi.transition.type = e.target.value;
    });
    document.getElementById('transitionDuration')?.addEventListener('change', (e) => {
        state.ndi.transition.duration = parseInt(e.target.value) || 500;
    });

    // Preset buttons
    document.querySelectorAll('.btn-preset[data-preset]').forEach(btn => {
        btn.addEventListener('click', () => ndiLoadPreset(btn.dataset.preset));
    });

    // Playlist
    document.getElementById('btnAddPlaylistItem')?.addEventListener('click', () => ndiAddToPlaylist());
    document.getElementById('btnPlaylistClear')?.addEventListener('click', ndiClearPlaylist);
    document.getElementById('btnPlaylistPlay')?.addEventListener('click', ndiPlayPlaylist);
    document.getElementById('btnPlaylistPause')?.addEventListener('click', ndiPausePlaylist);
    document.getElementById('btnPlaylistStop')?.addEventListener('click', ndiStopPlaylist);

    // Schedule
    const schedEn = document.getElementById('scheduleEnabled');
    const schedTime = document.getElementById('scheduleTime');
    if (schedEn) schedEn.addEventListener('change', (e) => {
        state.ndi.schedule.enabled = e.target.checked;
        if (schedTime) schedTime.disabled = !e.target.checked;
    });
    if (schedTime) schedTime.addEventListener('change', (e) => {
        state.ndi.schedule.time = e.target.value;
    });

    // CG toggle
    document.getElementById('cgDuringSwitch')?.addEventListener('change', (e) => {
        state.ndi.cgDuringSwitch.enabled = e.target.checked;
    });

    // PGM / PVW Dropdowns
    document.getElementById('programSourceSelect')?.addEventListener('change', (e) => {
        const id = e.target.value;
        if (id) {
            fetch(`/api/ndi/switch/${id}`, { method: 'POST' }).then(() => refreshNdiSources());
        }
    });
    document.getElementById('previewSourceSelect')?.addEventListener('change', (e) => {
        const id = e.target.value;
        if (id) {
            fetch(`/api/ndi/preview/${id}`, { method: 'POST' }).then(() => refreshNdiSources());
        }
    });

    // Audio Volume
    document.getElementById('masterAudioVolume')?.addEventListener('input', (e) => {
        const audio = document.getElementById('masterAudioPlayer');
        if (audio) audio.volume = parseFloat(e.target.value);
    });

    initAudioMetering();
    refreshNdiSources();
}

async function refreshNdiSources() {
    try {
        // Fetch sources and current PGM/PVW state in parallel
        const [srcRes, curRes] = await Promise.all([
            fetch('/api/ndi/sources'),
            fetch('/api/ndi/current')
        ]);
        const srcData = await srcRes.json();
        const curData = await curRes.json();

        if (srcData.success) {
            state.ndi.sources = srcData.sources;
        }
        if (curData.success) {
            state.ndi.programSource = curData.program;
            state.ndi.previewSource = curData.preview;
        }
        renderNdiSources();
    } catch (err) {
        console.error('[NDI] Source refresh error:', err);
    }
}

function renderNdiSources() {
    const grid = document.getElementById('ndiSourcesGrid');
    const pgmSelect = document.getElementById('programSourceSelect');
    const pvwSelect = document.getElementById('previewSourceSelect');

    if (pgmSelect && pvwSelect) {
        const options = '<option value="">-- Select Source --</option>' +
            state.ndi.sources.map(s => `<option value="${s.id}">${s.name}</option>`).join('');

        if (pgmSelect.innerHTML !== options) {
            pgmSelect.innerHTML = options;
            pvwSelect.innerHTML = options;
        }
        pgmSelect.value = state.ndi.programSource || '';
        pvwSelect.value = state.ndi.previewSource || '';
    }

    // Play preview audio (or program if no preview)
    const audioSrcId = state.ndi.previewSource || state.ndi.programSource;
    updateAudioSource(audioSrcId);

    if (!grid) return;

    if (state.ndi.sources.length === 0) {
        grid.innerHTML = '<div class="empty-state">No NDI sources found</div>';
        return;
    }

    grid.innerHTML = state.ndi.sources.map(source => {
        const isPgm = source.id === state.ndi.programSource;
        const isPvw = source.id === state.ndi.previewSource;
        const cls = isPgm ? 'program' : isPvw ? 'preview' : '';
        const tallyText = isPgm ? 'PGM' : isPvw ? 'PVW' : '';
        const icon = source.icon || '📺';

        return `
        <div class="ndi-source-tile ${cls}" onclick="ndiSelectSource('${source.id}')">
            ${tallyText ? `<div class="tally-label">${tallyText}</div>` : ''}
            <div class="tile-video">
                <img src="${source.streamUrl}" alt="${source.name}" class="tile-mjpeg">
            </div>
            <div class="tile-audio-meter">
                <div class="meter-bar" id="meter-${source.id}"></div>
            </div>
            <div class="tile-label">${source.name}</div>
        </div>`;
    }).join('');
}

let audioCtx = null;
let audioAnalyzer = null;
let meterAnimFrame = null;
let currentAudioSrcId = null;

function initAudioMetering() {
    const audio = document.getElementById('masterAudioPlayer');
    if (!audio) return;

    // Only init audio context once user interaction has happened if required, but we'll try here
    try {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        audioAnalyzer = audioCtx.createAnalyser();
        audioAnalyzer.fftSize = 256;

        const source = audioCtx.createMediaElementSource(audio);
        source.connect(audioAnalyzer);
        audioAnalyzer.connect(audioCtx.destination);

        updateMeters();
    } catch (e) {
        console.warn("AudioContext init failed (waiting for user interaction)", e);
    }
}

function updateAudioSource(sourceId) {
    const audio = document.getElementById('masterAudioPlayer');
    if (!audio || !sourceId || currentAudioSrcId === sourceId) return;

    const s = state.ndi.sources.find(x => x.id === sourceId);
    if (s && s.audioUrl) {
        audio.src = s.audioUrl;
        audio.play().catch(e => console.warn("Autoplay blocked for audio", e));
        currentAudioSrcId = sourceId;

        if (audioCtx && audioCtx.state === 'suspended') {
            audioCtx.resume();
        }
    }
}

function updateMeters() {
    if (!audioAnalyzer) return;

    const dataArray = new Uint8Array(audioAnalyzer.frequencyBinCount);
    audioAnalyzer.getByteFrequencyData(dataArray);

    // Calculate simple average for volume
    let sum = 0;
    for (let i = 0; i < dataArray.length; i++) {
        sum += dataArray[i];
    }
    const avg = sum / dataArray.length;
    // Map 0-255 to 0-100%
    const percent = Math.min(100, Math.max(0, (avg / 128) * 100));

    // Default to updating the active audio source's meter
    if (currentAudioSrcId) {
        const meter = document.getElementById(`meter-${currentAudioSrcId}`);
        if (meter) {
            meter.style.width = percent + '%';
            if (percent > 85) meter.style.background = '#ef4444'; // peak red
            else if (percent > 65) meter.style.background = '#eab308'; // warn yellow
            else meter.style.background = '#22c55e'; // good green
        }
    }

    meterAnimFrame = requestAnimationFrame(updateMeters);
}
// Click a source tile = set as PREVIEW
async function ndiSelectSource(sourceId) {
    try {
        const res = await fetch(`/api/ndi/preview/${sourceId}`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.ndi.previewSource = sourceId;
            renderNdiSources();
        }
    } catch (err) {
        console.error('[NDI] Preview select error:', err);
    }
}

// CUT: Swap PREVIEW to PROGRAM instantly
async function ndiCut() {
    if (!state.ndi.previewSource) {
        showToast('Select a PREVIEW source first', 'warning');
        return;
    }
    try {
        const res = await fetch('/api/ndi/cut', { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.ndi.programSource = data.program;
            state.ndi.previewSource = data.preview;
            renderNdiSources();
            showToast('CUT', 'success');
        }
    } catch (err) {
        console.error('[NDI] Cut error:', err);
    }
}

// AUTO: Transition PREVIEW to PROGRAM with effect
async function ndiAuto() {
    if (!state.ndi.previewSource) {
        showToast('Select a PREVIEW source first', 'warning');
        return;
    }
    try {
        const res = await fetch('/api/ndi/auto', { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.ndi.programSource = data.program;
            state.ndi.previewSource = data.preview;
            renderNdiSources();
            showToast(`AUTO (${data.transition.type} ${data.transition.duration}ms)`, 'success');
        }
    } catch (err) {
        console.error('[NDI] Auto error:', err);
    }
}

async function ndiAddToPlaylist(sourceId) {
    sourceId = sourceId || state.ndi.previewSource || state.ndi.programSource;
    if (!sourceId) { showToast('Select a source first', 'warning'); return; }
    try {
        const res = await fetch('/api/ndi/playlist/add', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ sourceId })
        });
        const data = await res.json();
        if (data.success) {
            state.ndi.playlist.push(data.item);
            renderNdiPlaylist();
        }
    } catch (err) { showToast('Failed to add', 'error'); }
}

function ndiClearPlaylist() {
    state.ndi.playlist = [];
    state.ndi.playlistIndex = 0;
    renderNdiPlaylist();
}

async function ndiPlayPlaylist() {
    try {
        const res = await fetch('/api/ndi/playlist/play', { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            state.ndi.isPlaying = true;
            document.getElementById('btnPlaylistPlay')?.classList.add('active');
        }
    } catch (err) { showToast('Failed to play', 'error'); }
}

async function ndiPausePlaylist() {
    try {
        await fetch('/api/ndi/playlist/pause', { method: 'POST' });
        state.ndi.isPlaying = false;
        document.getElementById('btnPlaylistPlay')?.classList.remove('active');
    } catch (err) { }
}

async function ndiStopPlaylist() {
    try {
        await fetch('/api/ndi/playlist/stop', { method: 'POST' });
        state.ndi.isPlaying = false;
        state.ndi.playlistIndex = 0;
        document.getElementById('btnPlaylistPlay')?.classList.remove('active');
    } catch (err) { }
}

function renderNdiPlaylist() {
    const el = document.getElementById('playlistItems');
    if (!el) return;
    if (state.ndi.playlist.length === 0) {
        el.innerHTML = '<div class="empty-state">Empty</div>';
        return;
    }
    el.innerHTML = state.ndi.playlist.map(item => `
        <div class="playlist-item" data-item-id="${item.id}">
            <span class="playlist-drag-handle">⋮</span>
            <span class="playlist-item-source">${item.sourceName}</span>
            <button class="playlist-item-remove" onclick="ndiRemovePlaylistItem('${item.id}')">✕</button>
        </div>
    `).join('');
}

function ndiRemovePlaylistItem(itemId) {
    fetch(`/api/ndi/playlist/remove/${itemId}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.success) {
                state.ndi.playlist = state.ndi.playlist.filter(i => i.id !== itemId);
                renderNdiPlaylist();
            }
        }).catch(() => { });
}

async function ndiLoadPreset(letter) {
    try {
        const res = await fetch(`/api/ndi/presets/${letter}/load`, { method: 'POST' });
        const data = await res.json();
        if (data.success) {
            document.querySelectorAll('.btn-preset').forEach(b => {
                b.classList.toggle('active', b.dataset.preset === letter);
            });
            showToast(`Preset ${letter} loaded`, 'success');
        } else {
            showToast(`Preset ${letter} empty`, 'warning');
        }
    } catch (err) { }
}

// ============================================================
// AUDIO METERING SYSTEM
// ============================================================

const audioContexts = new Map(); // sourceId -> { audioContext, analyser, meter }

function initializeAudioMeter(sourceId, audioUrl) {
    if (audioContexts.has(sourceId)) return;

    const meterEl = document.getElementById(`meter-${sourceId}`);
    if (!meterEl) return;

    try {
        const audioContext = new (window.AudioContext || window.webkitAudioContext)();
        const analyser = audioContext.createAnalyser();
        analyser.fftSize = 256;

        const audio = new Audio(audioUrl);
        audio.crossOrigin = 'anonymous';
        audio.volume = 0; // Mute playback, just for metering
        audio.play().catch(() => { });

        // createMediaElementAudioSource may not be available in all contexts
        if (!audioContext.createMediaElementSource && !audioContext.createMediaElementAudioSource) {
            return; // Can't meter without MediaElement source support
        }

        const createSource = audioContext.createMediaElementSource || audioContext.createMediaElementAudioSource;
        const source = createSource.call(audioContext, audio);
        source.connect(analyser);
        // Don't connect to destination — keeps audio muted while metering

        const meter = {
            analyser,
            audio,
            currentLevel: -Infinity,
            peakLevel: -Infinity,
            peakHoldTime: 0
        };

        audioContexts.set(sourceId, meter);

        // Metering loop
        function updateMeter() {
            const dataArray = new Uint8Array(analyser.frequencyBinCount);
            analyser.getByteFrequencyData(dataArray);

            // Calculate RMS from frequency data
            let sum = 0;
            for (let i = 0; i < dataArray.length; i++) {
                sum += (dataArray[i] / 255) ** 2;
            }
            const rms = Math.sqrt(sum / dataArray.length);
            const dbfs = 20 * Math.log10(rms + 1e-10); // dBFS

            meter.currentLevel = dbfs;
            meter.peakHoldTime--;

            if (dbfs > meter.peakLevel) {
                meter.peakLevel = dbfs;
                meter.peakHoldTime = 20; // Hold peak for ~200ms
            } else if (meter.peakHoldTime < 0) {
                meter.peakLevel = dbfs;
            }

            // Update meter bar visualization
            const normalizedLevel = Math.max(0, Math.min(1, (dbfs + 35) / 35)); // -35dB to 0dB scale
            const hue = Math.max(0, 120 - normalizedLevel * 180); // green to red
            const opacity = Math.min(1, normalizedLevel + 0.3);

            meterEl.style.width = (normalizedLevel * 100) + '%';
            meterEl.style.backgroundColor = `hsl(${hue}, 100%, 50%)`;
            meterEl.style.opacity = opacity;

            if (audioContexts.has(sourceId)) {
                requestAnimationFrame(updateMeter);
            }
        }

        updateMeter();
    } catch (err) {
        console.error(`[Audio Meter] Failed to initialize for ${sourceId}:`, err);
    }
}

// ============================================================
// MEDIA LIBRARY
// ============================================================

if (!state.mediaLibrary) {
    state.mediaLibrary = { items: [], selectedId: null, filters: { assetType: null, search: '' } };
}

function setupMediaLibrary() {
    const modal = document.getElementById('mediaLibraryModal');
    const closeBtn = document.getElementById('btnCloseMediaModal');
    const sideBtn = document.getElementById('btnMediaLibrary');
    const grid = document.getElementById('mediaGrid');
    const search = document.getElementById('mediaSearchInput');
    const fileInput = document.getElementById('mediaFileInput');

    if (!modal) return;

    // Open modal
    sideBtn?.addEventListener('click', () => {
        modal.classList.add('open');
        refreshMediaLibrary();
    });

    // Close
    closeBtn?.addEventListener('click', () => modal.classList.remove('open'));
    modal.addEventListener('click', (e) => { if (e.target === modal) modal.classList.remove('open'); });

    // Asset type filter
    modal.querySelectorAll('.asset-type-item').forEach(item => {
        item.addEventListener('click', () => {
            modal.querySelectorAll('.asset-type-item').forEach(i => i.classList.remove('active'));
            item.classList.add('active');
            state.mediaLibrary.filters.assetType = item.dataset.type || null;
            renderMediaGrid();
        });
    });

    // Search
    search?.addEventListener('input', () => {
        state.mediaLibrary.filters.search = search.value;
        renderMediaGrid();
    });

    // File import
    fileInput?.addEventListener('change', () => {
        if (!fileInput.files.length) return;
        [...fileInput.files].forEach(f => importMediaFile(f));
        fileInput.value = '';
    });
}

async function refreshMediaLibrary() {
    try {
        const params = new URLSearchParams();
        if (state.mediaLibrary.filters.assetType) params.set('assetType', state.mediaLibrary.filters.assetType);
        if (state.mediaLibrary.filters.search) params.set('search', state.mediaLibrary.filters.search);
        const res = await fetch('/api/media?' + params);
        const data = await res.json();
        if (data.success) {
            state.mediaLibrary.items = data.items;
            renderMediaGrid();
        }
    } catch (err) { console.error('[Media] Refresh error:', err); }
}

function renderMediaGrid() {
    const grid = document.getElementById('mediaGrid');
    if (!grid) return;

    const { items, filters } = state.mediaLibrary;
    let filtered = items;
    if (filters.assetType) filtered = filtered.filter(i => i.assetType === filters.assetType);
    if (filters.search) filtered = filtered.filter(i =>
        i.title.toLowerCase().includes(filters.search.toLowerCase())
    );

    if (!filtered.length) {
        grid.innerHTML = '<div class="empty-state">No media found</div>';
        return;
    }

    grid.innerHTML = filtered.map(item => {
        const dur = item.duration ? formatDuration(item.duration) : '';
        const thumb = item.thumbUrl
            ? `<img src="${item.thumbUrl}" loading="lazy" alt="">`
            : `<span class="no-thumb">🎬</span>`;
        return `<div class="media-card${state.mediaLibrary.selectedId === item.id ? ' selected' : ''}"
                     onclick="selectMedia('${item.id}')"
                     data-media-id="${item.id}">
            <div class="media-card-thumb">${thumb}</div>
            ${dur ? `<div class="media-card-dur">${dur}</div>` : ''}
            <div class="media-card-type">${item.assetType}</div>
            <div class="media-card-info">
                <div class="media-card-title">${item.title || item.fileName}</div>
                <div class="media-card-meta"><span>${item.assetType}</span><span>${dur}</span></div>
            </div>
        </div>`;
    }).join('');
}

function selectMedia(id) {
    state.mediaLibrary.selectedId = id;
    renderMediaGrid();
    renderMediaDetail(id);
}

function renderMediaDetail(id) {
    const panel = document.getElementById('mediaDetailPanel');
    if (!panel) return;
    const item = state.mediaLibrary.items.find(i => i.id === id);
    if (!item) { panel.innerHTML = '<div class="empty-state">Item not found</div>'; return; }

    const dur = item.duration ? formatDuration(item.duration) : '—';
    const thumb = item.thumbUrl
        ? `<img src="${item.thumbUrl}" alt="">`
        : `<span style="font-size:40px;opacity:.4">🎬</span>`;

    panel.innerHTML = `
        <div class="media-detail-thumb">${thumb}</div>
        <div class="media-detail-field">
            <label>Title</label>
            <input class="form-input" id="det-title" value="${escHtml(item.title || '')}" placeholder="Title…">
        </div>
        <div class="media-detail-field">
            <label>Asset Type</label>
            <select class="form-input" id="det-assetType">
                ${['client', 'ad', 'bumper', 'program', 'show', 'weather', 'obits', 'live', 'id'].map(t =>
        `<option value="${t}"${item.assetType === t ? ' selected' : ''}>${t}</option>`
    ).join('')}
            </select>
        </div>
        <div class="media-detail-field">
            <label>Client</label>
            <input class="form-input" id="det-client" value="${escHtml(item.client || '')}" placeholder="Client name…">
        </div>
        <div class="media-detail-field">
            <label>Air Date</label>
            <input class="form-input" id="det-airDate" value="${escHtml(item.airDate || '')}" placeholder="YYYY-MM-DD">
        </div>
        <div class="media-detail-field">
            <label>Notes</label>
            <input class="form-input" id="det-notes" value="${escHtml(item.notes || '')}" placeholder="Notes…">
        </div>
        <div style="font-size:10px;color:var(--text-muted)">Duration: ${dur} &nbsp;|&nbsp; Added: ${item.dateAdded ? item.dateAdded.slice(0, 10) : '—'}</div>
        <div class="media-detail-actions">
            <button class="btn btn-primary btn-small" onclick="saveMediaMeta('${id}')">Save Changes</button>
            <button class="btn btn-small" onclick="addMediaToPlaylist('${id}')">Add to Playlist</button>
            <button class="btn btn-small danger" onclick="deleteMedia('${id}')">Delete</button>
        </div>`;
}

function escHtml(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function formatDuration(sec) {
    const s = Math.round(sec);
    return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

async function saveMediaMeta(id) {
    const title = document.getElementById('det-title')?.value;
    const assetType = document.getElementById('det-assetType')?.value;
    const client = document.getElementById('det-client')?.value;
    const airDate = document.getElementById('det-airDate')?.value;
    const notes = document.getElementById('det-notes')?.value;
    try {
        const res = await fetch(`/api/media/${id}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ title, assetType, client, airDate, notes })
        });
        const data = await res.json();
        if (data.success) {
            const idx = state.mediaLibrary.items.findIndex(i => i.id === id);
            if (idx >= 0) state.mediaLibrary.items[idx] = data.item;
            renderMediaGrid();
            renderMediaDetail(id);
            showToast('Saved', 'success');
        }
    } catch (err) { showToast('Save failed', 'error'); }
}

async function deleteMedia(id) {
    if (!confirm('Delete this media file? This cannot be undone.')) return;
    try {
        await fetch(`/api/media/${id}`, { method: 'DELETE' });
        state.mediaLibrary.items = state.mediaLibrary.items.filter(i => i.id !== id);
        state.mediaLibrary.selectedId = null;
        renderMediaGrid();
        document.getElementById('mediaDetailPanel').innerHTML = '<div class="empty-state">Select a media item</div>';
        showToast('Deleted', 'success');
    } catch (err) { showToast('Delete failed', 'error'); }
}

function addMediaToPlaylist(id) {
    const item = state.mediaLibrary.items.find(i => i.id === id);
    if (!item) return;
    showToast(`Added "${item.title || item.fileName}" to playlist`, 'success');
    // TODO: hook into DDR playlist
}

async function importMediaFile(file) {
    const progress = document.getElementById('mediaUploadProgress');
    const fill = document.getElementById('mediaUploadFill');
    const status = document.getElementById('mediaUploadStatus');
    if (progress) progress.style.display = 'flex';

    const formData = new FormData();
    formData.append('file', file);
    formData.append('assetType', 'program');
    formData.append('title', file.name.replace(/\.[^.]+$/, ''));

    try {
        if (status) status.textContent = `Uploading ${file.name}…`;
        if (fill) fill.style.width = '10%';

        const res = await fetch('/api/media/import', { method: 'POST', body: formData });
        if (fill) fill.style.width = '100%';
        const data = await res.json();
        if (data.success) {
            state.mediaLibrary.items.unshift(data.item);
            renderMediaGrid();
            selectMedia(data.item.id);
            showToast(`Imported: ${file.name}`, 'success');
        } else {
            showToast('Import failed: ' + data.error, 'error');
        }
    } catch (err) {
        showToast('Upload error', 'error');
    } finally {
        if (progress) progress.style.display = 'none';
        if (fill) fill.style.width = '0%';
    }
}

window.selectMedia = selectMedia;
window.saveMediaMeta = saveMediaMeta;
window.deleteMedia = deleteMedia;
window.addMediaToPlaylist = addMediaToPlaylist;

// ============================================================
// CG MARRIAGE SLOTS UI
// ============================================================

const cgSlotState = [
    { slotNum: 1, templateId: null, templateName: null, mode: 'FOLLOW', sourceId: null },
    { slotNum: 2, templateId: null, templateName: null, mode: 'FOLLOW', sourceId: null },
    { slotNum: 3, templateId: null, templateName: null, mode: 'FOLLOW', sourceId: null },
    { slotNum: 4, templateId: null, templateName: null, mode: 'FOLLOW', sourceId: null }
];

async function loadCGMarriageState() {
    try {
        const r = await fetch('/api/cg-marriage');
        if (!r.ok) return;
        const data = await r.json();
        if (data.slots) {
            data.slots.forEach(s => {
                const idx = s.slotNum - 1;
                if (cgSlotState[idx]) {
                    cgSlotState[idx].templateId = s.templateId;
                    cgSlotState[idx].mode = s.mode;
                    cgSlotState[idx].sourceId = s.sourceId;
                }
            });
        }
        renderCGSlots();
    } catch (e) { /* server may not have CG marriage yet */ }
}

function renderCGSlots() {
    cgSlotState.forEach(slot => {
        const titleEl = document.getElementById(`cgSlotTitle${slot.slotNum}`);
        const thumbEl = document.getElementById(`cgSlotThumb${slot.slotNum}`);
        const modeEl = document.getElementById(`cgSlotMode${slot.slotNum}`);
        const slotEl = document.getElementById(`cgSlot${slot.slotNum}`);

        if (!titleEl) return;

        if (slot.templateId) {
            titleEl.textContent = slot.templateName || slot.templateId;
            thumbEl.innerHTML = `<span style="font-size:18px">🎨</span>`;
            slotEl.classList.add('has-template');
        } else {
            titleEl.textContent = '—';
            thumbEl.innerHTML = `<span class="cg-slot-empty">Empty</span>`;
            slotEl.classList.remove('has-template');
        }

        if (modeEl) modeEl.value = slot.mode;
    });
}

async function openCGSlotPicker(slotNum) {
    // Get templates list
    const templates = state.templates || [];
    if (!templates.length) {
        alert('No templates loaded yet. Open a template first.');
        return;
    }

    const options = templates.map((t, i) =>
        `<option value="${i}">${t.name || `Template ${i + 1}`}</option>`
    ).join('');

    const sel = document.createElement('select');
    sel.innerHTML = '<option value="">— Select Template —</option>' + options;
    sel.className = 'form-select';
    sel.style.cssText = 'width:300px;padding:8px;background:#1a1a2e;color:#e0e0e0;border:1px solid #404050;border-radius:4px;font-size:14px;';

    const container = document.createElement('div');
    container.style.cssText = 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:#1c1c2e;border:1px solid #404050;border-radius:8px;padding:20px;z-index:10000;min-width:340px;box-shadow:0 8px 32px rgba(0,0,0,0.8);';
    container.innerHTML = `<h3 style="margin:0 0 12px;font-size:14px;color:#e0e0e0;">Attach Template to CG Slot ${slotNum}</h3>`;
    container.appendChild(sel);

    const btnRow = document.createElement('div');
    btnRow.style.cssText = 'display:flex;gap:8px;margin-top:14px;justify-content:flex-end;';
    btnRow.innerHTML = `<button id="cgPickCancel" class="btn">Cancel</button><button id="cgPickConfirm" class="btn btn-primary">Attach</button>`;
    container.appendChild(btnRow);

    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.6);z-index:9999;';
    document.body.appendChild(overlay);
    document.body.appendChild(container);

    return new Promise(resolve => {
        document.getElementById('cgPickCancel').onclick = () => {
            overlay.remove(); container.remove(); resolve(null);
        };
        document.getElementById('cgPickConfirm').onclick = async () => {
            const idx = sel.value;
            if (idx === '') { overlay.remove(); container.remove(); resolve(null); return; }
            const tpl = templates[parseInt(idx)];
            overlay.remove(); container.remove();

            // Save to server
            try {
                await fetch(`/api/cg-marriage/${slotNum}`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ templateId: tpl.id || String(idx), templateName: tpl.name || `Template ${parseInt(idx) + 1}` })
                });
                cgSlotState[slotNum - 1].templateId = tpl.id || String(idx);
                cgSlotState[slotNum - 1].templateName = tpl.name || `Template ${parseInt(idx) + 1}`;
                renderCGSlots();
            } catch (e) { console.error('CG slot attach failed:', e); }
            resolve(tpl);
        };
        overlay.onclick = () => { overlay.remove(); container.remove(); resolve(null); };
    });
}

async function clearCGSlot(slotNum) {
    try {
        await fetch(`/api/cg-marriage/clear/${slotNum}`, { method: 'POST' });
        cgSlotState[slotNum - 1].templateId = null;
        cgSlotState[slotNum - 1].templateName = null;
        renderCGSlots();
    } catch (e) { console.error('CG slot clear failed:', e); }
}

function setupCGSlotModeListeners() {
    [1, 2, 3, 4].forEach(n => {
        const sel = document.getElementById(`cgSlotMode${n}`);
        if (!sel) return;
        sel.addEventListener('change', async () => {
            const mode = sel.value;
            cgSlotState[n - 1].mode = mode;
            try {
                await fetch(`/api/cg-marriage/${n}/${mode}`, { method: 'PUT' });
            } catch (e) { /* non-fatal */ }
        });
    });
}

window.openCGSlotPicker = openCGSlotPicker;
window.clearCGSlot = clearCGSlot;

// ============================================================
// AS-RUN AUTOMATION
// ============================================================

const asRunState = {
    enabled: false,
    isRunning: false,
    schedule: [],
    currentEventIndex: -1,
    clockTimer: null,
    selectedEventId: null
};

function setupAsRunModal() {
    const sideBtn = document.getElementById('btnAsRun');
    const modal = document.getElementById('asRunModal');
    const closeBtn = document.getElementById('btnCloseAsRunModal');
    if (!sideBtn || !modal) return;

    sideBtn.addEventListener('click', () => {
        modal.classList.add('open');
        refreshAsRunSchedule();
        startAsRunClock();
    });

    closeBtn?.addEventListener('click', () => {
        modal.classList.remove('open');
        stopAsRunClock();
    });

    // Toolbar buttons
    document.getElementById('btnAsRunPlay')?.addEventListener('click', asRunPlay);
    document.getElementById('btnAsRunPause')?.addEventListener('click', asRunPause);
    document.getElementById('btnAsRunStop')?.addEventListener('click', asRunStop);
    document.getElementById('btnAddAsRunEvent')?.addEventListener('click', () => showEventEditor(null));
    document.getElementById('btnSaveAsRunEvent')?.addEventListener('click', saveAsRunEvent);

    // Event type selector shows/hides fields
    document.getElementById('evActionType')?.addEventListener('change', updateEventEditorFields);
}

async function refreshAsRunSchedule() {
    try {
        const r = await fetch('/api/as-run');
        if (!r.ok) return;
        const data = await r.json();
        asRunState.schedule = data.schedule || [];
        asRunState.isRunning = data.isRunning || false;
        asRunState.currentEventIndex = data.currentEventIndex ?? -1;
        renderAsRunEvents();
        updateAsRunToolbarState();
    } catch (e) { console.warn('As-run load failed:', e); }
}

function renderAsRunEvents() {
    const container = document.getElementById('asRunEvents');
    if (!container) return;

    if (!asRunState.schedule.length) {
        container.innerHTML = '<div style="padding:20px;text-align:center;color:var(--text-muted);font-size:13px;">No events scheduled. Click + Add Event.</div>';
        return;
    }

    const now = new Date();
    const nowStr = now.toTimeString().slice(0, 8);

    container.innerHTML = asRunState.schedule.map((ev, i) => {
        const isPast = ev.time < nowStr && !asRunState.isRunning;
        const isActive = i === asRunState.currentEventIndex;
        const typeClass = {
            PLAY_MEDIA: 'type-play',
            SWITCH_SOURCE: 'type-switch',
            DEPLOY_CG: 'type-cg',
            CLEAR_CG: 'type-clear'
        }[ev.action] || '';

        return `<div class="as-run-event ${isActive ? 'active' : ''} ${isPast ? 'past' : ''}"
                     onclick="asRunSelectEvent('${ev.id}')">
            <span class="ev-time">${ev.time}</span>
            <span class="ev-type ${typeClass}">${ev.action.replace('_', ' ')}</span>
            <span class="ev-desc">${escHtmlAR(ev.description || '—')}</span>
            <button class="ev-del" onclick="event.stopPropagation();deleteAsRunEvent('${ev.id}')" title="Delete">✕</button>
        </div>`;
    }).join('');
}

function escHtmlAR(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function updateAsRunToolbarState() {
    const playBtn = document.getElementById('btnAsRunPlay');
    const pauseBtn = document.getElementById('btnAsRunPause');
    const stopBtn = document.getElementById('btnAsRunStop');
    const statusEl = document.getElementById('asRunStatus');

    if (playBtn) playBtn.disabled = asRunState.isRunning;
    if (pauseBtn) pauseBtn.disabled = !asRunState.isRunning;
    if (stopBtn) stopBtn.disabled = !asRunState.isRunning;
    if (statusEl) statusEl.textContent = asRunState.isRunning ? 'RUNNING' : 'STOPPED';
    if (statusEl) statusEl.className = `as-run-status ${asRunState.isRunning ? 'running' : ''}`;
}

async function asRunPlay() {
    try {
        await fetch('/api/as-run/start', { method: 'POST' });
        asRunState.isRunning = true;
        updateAsRunToolbarState();
    } catch (e) { console.error('As-run start failed:', e); }
}

async function asRunPause() {
    try {
        await fetch('/api/as-run/pause', { method: 'POST' });
        asRunState.isRunning = false;
        updateAsRunToolbarState();
    } catch (e) { console.error('As-run pause failed:', e); }
}

async function asRunStop() {
    try {
        await fetch('/api/as-run/stop', { method: 'POST' });
        asRunState.isRunning = false;
        asRunState.currentEventIndex = -1;
        updateAsRunToolbarState();
        renderAsRunEvents();
    } catch (e) { console.error('As-run stop failed:', e); }
}

function asRunSelectEvent(id) {
    asRunState.selectedEventId = id;
    const ev = asRunState.schedule.find(e => e.id === id);
    if (ev) showEventEditor(ev);
}

async function deleteAsRunEvent(id) {
    try {
        await fetch(`/api/as-run/${id}`, { method: 'DELETE' });
        await refreshAsRunSchedule();
    } catch (e) { console.error('Delete event failed:', e); }
}

function showEventEditor(ev) {
    const panel = document.getElementById('asRunEditorInner');
    if (!panel) return;

    const isNew = !ev;
    const sources = state.ndi?.sources || [];
    const sourceOpts = sources.map(s => `<option value="${s.id}" ${ev?.sourceId === s.id ? 'selected' : ''}>${escHtmlAR(s.name)}</option>`).join('');

    panel.innerHTML = `
        <h3>${isNew ? 'New Event' : 'Edit Event'}</h3>
        <input type="hidden" id="evId" value="${ev?.id || ''}">
        <div class="form-group">
            <label class="form-label">Time (HH:MM:SS)</label>
            <input type="time" step="1" id="evTime" class="form-input" value="${ev?.time || ''}">
        </div>
        <div class="form-group">
            <label class="form-label">Action</label>
            <select id="evActionType" class="form-select">
                <option value="SWITCH_SOURCE" ${ev?.action === 'SWITCH_SOURCE' ? 'selected' : ''}>Switch Source</option>
                <option value="PLAY_MEDIA"    ${ev?.action === 'PLAY_MEDIA' ? 'selected' : ''}>Play Media</option>
                <option value="DEPLOY_CG"     ${ev?.action === 'DEPLOY_CG' ? 'selected' : ''}>Deploy CG</option>
                <option value="CLEAR_CG"      ${ev?.action === 'CLEAR_CG' ? 'selected' : ''}>Clear CG</option>
            </select>
        </div>
        <div id="evSourceGroup" class="form-group">
            <label class="form-label">NDI Source</label>
            <select id="evSourceId" class="form-select">
                <option value="">— Select Source —</option>
                ${sourceOpts}
            </select>
        </div>
        <div id="evMediaGroup" class="form-group" style="display:none;">
            <label class="form-label">Media ID</label>
            <input type="text" id="evMediaId" class="form-input" value="${ev?.mediaId || ''}" placeholder="media-xxxxxx">
        </div>
        <div id="evCGGroup" class="form-group" style="display:none;">
            <label class="form-label">CG Slot (1-4)</label>
            <select id="evCGSlot" class="form-select">
                <option value="1" ${ev?.cgSlot === 1 ? 'selected' : ''}>Slot 1</option>
                <option value="2" ${ev?.cgSlot === 2 ? 'selected' : ''}>Slot 2</option>
                <option value="3" ${ev?.cgSlot === 3 ? 'selected' : ''}>Slot 3</option>
                <option value="4" ${ev?.cgSlot === 4 ? 'selected' : ''}>Slot 4</option>
            </select>
        </div>
        <div class="form-group">
            <label class="form-label">Description</label>
            <input type="text" id="evDesc" class="form-input" value="${escHtmlAR(ev?.description || '')}" placeholder="Optional note">
        </div>
        <button class="btn btn-primary" style="width:100%;margin-top:4px;" onclick="saveAsRunEvent()">
            ${isNew ? 'Add Event' : 'Save Changes'}
        </button>
    `;

    updateEventEditorFields();
}

function updateEventEditorFields() {
    const action = document.getElementById('evActionType')?.value;
    if (!action) return;
    const sourceGrp = document.getElementById('evSourceGroup');
    const mediaGrp = document.getElementById('evMediaGroup');
    const cgGrp = document.getElementById('evCGGroup');
    if (sourceGrp) sourceGrp.style.display = action === 'SWITCH_SOURCE' ? '' : 'none';
    if (mediaGrp) mediaGrp.style.display = action === 'PLAY_MEDIA' ? '' : 'none';
    if (cgGrp) cgGrp.style.display = (action === 'DEPLOY_CG' || action === 'CLEAR_CG') ? '' : 'none';
}

async function saveAsRunEvent() {
    const id = document.getElementById('evId')?.value;
    const time = document.getElementById('evTime')?.value;
    const action = document.getElementById('evActionType')?.value;
    const desc = document.getElementById('evDesc')?.value;

    if (!time || !action) { alert('Time and Action are required.'); return; }

    const body = { time, action, description: desc };

    if (action === 'SWITCH_SOURCE') body.sourceId = document.getElementById('evSourceId')?.value;
    if (action === 'PLAY_MEDIA') body.mediaId = document.getElementById('evMediaId')?.value;
    if (action === 'DEPLOY_CG' || action === 'CLEAR_CG') {
        body.cgSlot = parseInt(document.getElementById('evCGSlot')?.value);
    }

    try {
        if (id) {
            await fetch(`/api/as-run/${id}`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
        } else {
            await fetch('/api/as-run', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
        }
        await refreshAsRunSchedule();
    } catch (e) { console.error('Save event failed:', e); }
}

function startAsRunClock() {
    stopAsRunClock();
    const clockEl = document.getElementById('asRunClock');
    if (!clockEl) return;
    asRunState.clockTimer = setInterval(() => {
        const now = new Date();
        clockEl.textContent = now.toTimeString().slice(0, 8);
    }, 500);
}

function stopAsRunClock() {
    if (asRunState.clockTimer) { clearInterval(asRunState.clockTimer); asRunState.clockTimer = null; }
}

window.asRunSelectEvent = asRunSelectEvent;
window.deleteAsRunEvent = deleteAsRunEvent;
window.saveAsRunEvent = saveAsRunEvent;

// ============================================================
// PGM MONITOR
// ============================================================

function setupPGMMonitor() {
    const btn = document.getElementById('btnPGMMonitor');
    const modal = document.getElementById('pgmMonitorModal');
    const close = document.getElementById('btnClosePGMModal');
    if (!btn || !modal) return;

    btn.addEventListener('click', () => {
        modal.classList.add('open');
        const img = document.getElementById('pgmMonitorVideo');
        if (img) img.src = '/api/pgm/stream?' + Date.now();
    });

    close?.addEventListener('click', () => {
        modal.classList.remove('open');
        const img = document.getElementById('pgmMonitorVideo');
        if (img) img.src = '';
    });

    // Gain slider
    const gainSlider = document.getElementById('pgmGainSlider');
    const gainValue = document.getElementById('pgmGainValue');
    gainSlider?.addEventListener('input', () => {
        const db = parseFloat(gainSlider.value);
        gainValue.textContent = db + ' dB';
        fetch('/api/pgm/normalization', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ gain: db })
        }).catch(() => { });
    });

    // Update source label when PGM changes
    document.addEventListener('ndi-pgm-changed', (e) => {
        const label = document.getElementById('pgmSourceLabel');
        if (label) label.textContent = e.detail?.sourceName || 'No Source';
    });
}

// Update PGM timecode display
setInterval(() => {
    const tc = document.getElementById('pgmTimecode');
    if (tc && document.getElementById('pgmMonitorModal')?.classList.contains('open')) {
        tc.textContent = new Date().toTimeString().slice(0, 8);
    }
}, 1000);

// ============================================================
// CONTEXT MENU SYSTEM
// ============================================================

let activeContextMenu = null;

function showContextMenu(x, y, items) {
    closeContextMenu();

    const menu = document.createElement('div');
    menu.className = 'context-menu';
    menu.id = 'activeContextMenu';

    items.forEach(item => {
        if (item === 'sep') {
            const sep = document.createElement('div');
            sep.className = 'context-menu-sep';
            menu.appendChild(sep);
            return;
        }
        const el = document.createElement('div');
        el.className = 'context-menu-item' + (item.danger ? ' danger' : '') + (item.sub ? ' has-submenu' : '');
        el.innerHTML = `<span class="cm-icon">${item.icon || ''}</span>${item.label}`;

        if (item.sub) {
            const sub = document.createElement('div');
            sub.className = 'context-menu-submenu';
            item.sub.forEach(s => {
                const sel = document.createElement('div');
                sel.className = 'context-menu-item';
                sel.innerHTML = `<span class="cm-icon">${s.icon || ''}</span>${s.label}`;
                sel.addEventListener('click', (e) => { e.stopPropagation(); closeContextMenu(); s.action(); });
                sub.appendChild(sel);
            });
            el.appendChild(sub);
        } else if (item.action) {
            el.addEventListener('click', (e) => { e.stopPropagation(); closeContextMenu(); item.action(); });
        }

        menu.appendChild(el);
    });

    // Position within viewport
    document.body.appendChild(menu);
    const rect = menu.getBoundingClientRect();
    const vw = window.innerWidth, vh = window.innerHeight;
    const finalX = x + rect.width > vw ? x - rect.width : x;
    const finalY = y + rect.height > vh ? y - rect.height : y;
    menu.style.left = finalX + 'px';
    menu.style.top = finalY + 'px';

    activeContextMenu = menu;
}

function closeContextMenu() {
    if (activeContextMenu) { activeContextMenu.remove(); activeContextMenu = null; }
}

document.addEventListener('click', closeContextMenu);
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeContextMenu(); });

function setupSourceContextMenus() {
    document.addEventListener('contextmenu', (e) => {
        const tile = e.target.closest('.ndi-source-tile');
        if (!tile) return;
        e.preventDefault();
        const sourceId = tile.getAttribute('onclick')?.match(/'([^']+)'/)?.[1];
        if (!sourceId) return;
        const source = state.ndi.sources.find(s => s.id === sourceId);
        if (!source) return;

        showContextMenu(e.clientX, e.clientY, [
            { icon: '🟢', label: 'Set as PREVIEW', action: () => ndiSelectSource(sourceId) },
            { icon: '🔴', label: 'Set as PROGRAM', action: () => ndiCutTo(sourceId) },
            'sep',
            { icon: '🎬', label: 'Attach CG 1', action: () => attachCGToSource(1, sourceId) },
            { icon: '🎬', label: 'Attach CG 2', action: () => attachCGToSource(2, sourceId) },
            { icon: '🎬', label: 'Attach CG 3', action: () => attachCGToSource(3, sourceId) },
            { icon: '🎬', label: 'Attach CG 4', action: () => attachCGToSource(4, sourceId) },
            'sep',
            {
                icon: '💾', label: 'Save as Preset', sub: ['A', 'B', 'C', 'D'].map(l => ({
                    label: `Preset ${l}`,
                    action: () => ndiSavePreset(l)
                }))
            },
            { icon: 'ℹ️', label: source.name, danger: false, action: () => { } }
        ]);
    });
}

async function ndiCutTo(sourceId) {
    await ndiSelectSource(sourceId);
    await ndiCut();
}

function attachCGToSource(slot, sourceId) {
    showToast(`CG slot ${slot} — select a template to attach`, 'info');
    // TODO: open template picker for this slot
}

// ============================================================
// API OBJECT (for external access)
// ============================================================
const API = {
    getTemplates: () => Object.entries(TEMPLATES).map(([key, t]) => ({
        id: key, name: t.name, category: t.category, icon: t.icon,
        params: t.params.map(p => ({ name: p.name, type: p.type, required: p.required }))
    })),
    getTemplate: (id) => TEMPLATES[id] || null,
    getAnimationTypes: () => Object.keys(ANIMATION_TYPES),
    getAnimationPresets: () => ANIMATION_PRESETS,
    generateUrl: (templateId, params, animation) => {
        state.selectedTemplate = templateId;
        state.templateParams = params;
        if (animation) Object.assign(state.animation, animation);
        return generateTemplateUrl();
    },
    deploy: (templateId, params, animation) => {
        state.selectedTemplate = templateId;
        state.templateParams = params;
        if (animation) Object.assign(state.animation, animation);
        const url = generateTemplateUrl();
        window.open(url, 'cg_output', 'width=1920,height=1080');
        return { success: true, url, timestamp: new Date().toISOString() };
    },
    setRemoteText: (field, value) => {
        if (state.selectedTemplate) {
            state.templateParams[field] = value;
            const el = elements.formFields.querySelector(`[name="${field}"]`);
            if (el) el.value = value;
            updatePreview();
        }
    },
    applyPreset: (presetId) => {
        const preset = ANIMATION_PRESETS.find(p => p.id === presetId);
        if (preset) applyAnimationPreset(preset);
    },
    openRundown: () => openRundown(),
    openTemplateBuilder: () => openTemplateBuilder(null),
    selectChannel: (ch) => selectChannel(ch),
    playRundown: () => playRundown(),
    nextRundownItem: () => nextRundownItem(),
    prevRundownItem: () => prevRundownItem(),
    stopRundown: () => stopRundown()
};

window.CGControl = API;
// Expose NDI functions for inline handlers
window.ndiSelectSource = ndiSelectSource;
window.ndiRemovePlaylistItem = ndiRemovePlaylistItem;

// ============================================================
// CONFIG BODY HEIGHT FIX
// ============================================================
function fixConfigBodyHeight() {
    // CSS flex handles layout; nothing to do
}

window.addEventListener('resize', fixConfigBodyHeight);

// ============================================================
// INIT
// ============================================================
document.addEventListener('DOMContentLoaded', init);
