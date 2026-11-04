// ============================================
// Help Documentation Index
// Searchable help topics for the Broadcast Suite
// ============================================

export interface HelpTopic {
    id: string;
    title: string;
    category: string;
    keywords: string[];
    content: string;
    relatedTopics?: string[];
}

export const HELP_CATEGORIES = [
    { id: 'getting-started', name: 'Getting Started', icon: '🚀' },
    { id: 'playout', name: 'Playout', icon: '▶️' },
    { id: 'scheduler', name: 'Weekly Scheduler', icon: '📅' },
    { id: 'media', name: 'Media Library', icon: '🎬' },
    { id: 'ddr', name: 'DDR Players', icon: '🎞️' },
    { id: 'obs', name: 'OBS Integration', icon: '📡' },
    { id: 'cg', name: 'CG System', icon: '🎨' },
    { id: 'analytics', name: 'Analytics', icon: '📊' },
    { id: 'settings', name: 'Settings', icon: '⚙️' },
    { id: 'shortcuts', name: 'Keyboard Shortcuts', icon: '⌨️' },
];

export const HELP_TOPICS: HelpTopic[] = [
    // --- Getting Started ---
    {
        id: 'intro',
        title: 'Welcome to Broadcast Suite',
        category: 'getting-started',
        keywords: ['intro', 'start', 'welcome', 'overview', 'basics'],
        content: `
**Broadcast Suite** is a web-based TV automation system for scheduling, playout, and CG overlay.

**Quick Start:**
1. Add media to your playlist from the **Media Library** (left panel)
2. Press **Play** to start playout
3. Use the **Weekly Scheduler** to plan your broadcast week
4. Add **CG elements** for lower thirds, tickers, and bugs

**Navigation:**
- **Sidebar icons** switch between views (Playout, Scheduler, Analytics, Settings)
- **Bottom icons** toggle side panels (OBS, DDR, CG)
- **Right-click** anywhere for context menus
        `.trim(),
        relatedTopics: ['playout-basics', 'sidebar-nav'],
    },
    {
        id: 'sidebar-nav',
        title: 'Sidebar Navigation',
        category: 'getting-started',
        keywords: ['sidebar', 'navigation', 'menu', 'icons'],
        content: `
The **sidebar** on the left provides quick access to all views:

**Main Views (Top):**
- ▶️ **Playout** - Main playout interface with player and playlist
- 📅 **Scheduler** - Weekly 24×7 grid scheduler
- 📊 **Analytics** - View play statistics and reports
- ⚙️ **Settings** - Configure automation preferences

**Side Panels (Bottom):**
- 📡 **OBS** - Control OBS Studio instances
- 🎞️ **DDR** - B-roll players (4 channels)
- 🎨 **CG** - Character generator overlays

Click any icon to switch views. Panels toggle on/off.
        `.trim(),
    },

    // --- Playout ---
    {
        id: 'playout-basics',
        title: 'Playout Basics',
        category: 'playout',
        keywords: ['playout', 'play', 'playback', 'schedule', 'start'],
        content: `
The **Playout View** is your main control center.

**Adding Media:**
- Click items in the Media Library to add to playlist
- Drag to reorder (drag-and-drop)
- Right-click for context menu options

**Transport Controls:**
- ▶️ **Play/Pause** - Start or pause playback
- ⏹️ **Stop** - Stop and reset to beginning
- ⏭️ **Next** - Skip to next item

**Schedule Info Bar:**
- Shows schedule start time, total duration, and end time
- **Snap to TOH** - Align schedule to next top-of-hour
        `.trim(),
        relatedTopics: ['fixed-time', 'smart-slots'],
    },
    {
        id: 'fixed-time',
        title: 'Fixed Time Items',
        category: 'playout',
        keywords: ['fixed', 'time', 'hard start', 'schedule'],
        content: `
**Fixed Time** items start at an exact time, regardless of previous items.

**To set fixed time:**
1. Right-click a playlist item
2. Select "Set Fixed Time"
3. Choose the exact start time

**Gap Handling:**
- If there's time before the fixed item, a **GAP** is inserted
- Fill gaps automatically with IDs/bumpers (see Settings)
        `.trim(),
    },
    {
        id: 'smart-slots',
        title: 'Smart Slots',
        category: 'playout',
        keywords: ['smart', 'slot', 'latest', 'episode', 'dynamic'],
        content: `
**Smart Slots** dynamically load content at runtime.

**Types:**
- **Latest Episode** - Plays the most recent episode of a series
- **Previous Episode** - Plays the second most recent

**To use:**
1. Go to Media Library → Tools tab
2. Click a Smart Slot option
3. The slot resolves when playout starts

Configure "Always play latest" in Settings for automatic resolution.
        `.trim(),
    },

    // --- Scheduler ---
    {
        id: 'scheduler-overview',
        title: 'Weekly Scheduler Overview',
        category: 'scheduler',
        keywords: ['scheduler', 'weekly', 'grid', 'calendar'],
        content: `
The **Weekly Scheduler** is a 24×7 grid for planning your broadcast week.

**Grid Layout:**
- Columns = Days (Sun-Sat)
- Rows = Hours (00:00-23:00)
- Each cell = One hour slot

**Cell Indicators:**
- **Fill bar** shows how full the hour is (green = under 60min, yellow = over)
- **Loop icon** = Hour set to loop
- **Shuffle icon** = Ads randomized

**Context Menu (Right-Click):**
- Copy/Paste hour content
- Quick copy to next hour/day
- Toggle loop, randomize, lock order
- Export to DDR or Free Playlist
        `.trim(),
        relatedTopics: ['copy-paste-hours'],
    },
    {
        id: 'copy-paste-hours',
        title: 'Copying Hours',
        category: 'scheduler',
        keywords: ['copy', 'paste', 'duplicate', 'hour'],
        content: `
**Copy hour content** to reuse across the week.

**Methods:**
1. **Right-click → Copy Hour** then **Paste Hour** on target
2. **Quick Copy → Next Hour** - Copy to the hour directly below
3. **Quick Copy → Next Day** - Copy to same hour, next day

The clipboard persists until you copy something else.
        `.trim(),
    },

    // --- Media Library ---
    {
        id: 'media-types',
        title: 'Media Types',
        category: 'media',
        keywords: ['media', 'type', 'program', 'ad', 'bumper', 'id'],
        content: `
**Media Types** categorize content for automation.

| Type | Description |
|------|-------------|
| **Program** | Main shows, episodes |
| **Ad** | Commercial spots |
| **Promo** | Network promos |
| **Station ID** | Short idents |
| **Bumper** | Transition elements |
| **Paid Program** | Infomercials |

Set type when editing media. Used for gap-fill and scheduling rules.
        `.trim(),
    },
    {
        id: 'importing-media',
        title: 'Importing Media',
        category: 'media',
        keywords: ['import', 'upload', 'local', 'file'],
        content: `
**Import local video files** for playout.

1. Click **Local** tab in Media Library
2. Click **Import Video Files**
3. Select one or multiple video files
4. Files load with auto-detected duration

**Supported formats:** MP4, WebM, MOV (browser-dependent)

Imported files persist in browser memory during session.
        `.trim(),
    },

    // --- DDR ---
    {
        id: 'ddr-overview',
        title: 'DDR Players Overview',
        category: 'ddr',
        keywords: ['ddr', 'b-roll', 'player', 'channel'],
        content: `
**DDR (Digital Disk Recorder)** provides 4 independent B-roll players.

**Channels:** A (red), B (green), C (blue), D (purple)

**Features:**
- Independent playlists per channel
- Loop and shuffle modes
- Volume control
- Picture-in-Picture overlay

**To add content:**
- Right-click media → Send to DDR A/B/C/D
- Or drag from library to DDR channel
        `.trim(),
        relatedTopics: ['ddr-pip'],
    },
    {
        id: 'ddr-pip',
        title: 'DDR Picture-in-Picture',
        category: 'ddr',
        keywords: ['pip', 'picture', 'overlay', 'corner'],
        content: `
**Picture-in-Picture** overlays DDR content on main playout.

**To enable:**
1. Open DDR panel (Film icon)
2. Click the **Layers** button on a channel
3. DDR video appears in corner of main player

**Settings (per channel):**
- Position: top-left, top-right, bottom-left, bottom-right
- Size: 5-50% of screen
- Opacity: 0-100%
        `.trim(),
    },

    // --- OBS ---
    {
        id: 'obs-setup',
        title: 'OBS WebSocket Setup',
        category: 'obs',
        keywords: ['obs', 'websocket', 'connect', 'setup'],
        content: `
**Connect to OBS Studio** for scene switching and recording control.

**Requirements:**
1. OBS Studio 28+ (includes obs-websocket)
2. Enable WebSocket server in OBS Tools menu

**Default Configuration:**
- Host: localhost
- Port: 4455
- Password: (optional, set in OBS)

**Connecting:**
1. Open OBS panel (Radio icon)
2. Click **Connect**
3. Green dot = connected
        `.trim(),
        relatedTopics: ['obs-controls', 'obs-multiple'],
    },
    {
        id: 'obs-controls',
        title: 'OBS Controls',
        category: 'obs',
        keywords: ['obs', 'scene', 'record', 'stream'],
        content: `
**Available OBS controls:**

| Control | Description |
|---------|-------------|
| **Connect/Disconnect** | Toggle WebSocket connection |
| **Scenes** | Click to switch active scene |
| **Record** | Start/stop recording |
| **Stream** | Start/stop streaming |

**Status Indicators:**
- 🔴 Recording active
- 📡 Streaming active
        `.trim(),
    },
    {
        id: 'obs-multiple',
        title: 'Multiple OBS Sessions',
        category: 'obs',
        keywords: ['obs', 'multiple', 'session', 'backup'],
        content: `
**Control up to 3 OBS instances** for redundancy or multi-cam.

**Setup (in Settings):**
1. Go to Settings → OBS
2. Add sessions with Name, Host, Port, Password
3. Each session appears separately in OBS panel

**Use Cases:**
- Main + Backup OBS
- Multi-computer setups
- Different scene collections
        `.trim(),
    },

    // --- CG ---
    {
        id: 'cg-overview',
        title: 'CG System Overview',
        category: 'cg',
        keywords: ['cg', 'graphics', 'lower third', 'ticker'],
        content: `
**Character Generator (CG)** adds graphics overlays to playout.

**Element Types:**
- **Lower Third** - Name/title bars
- **Ticker** - Scrolling text
- **Bug** - Corner logos
- **Clock** - Live time display
- **Weather** - Weather graphics

**Quick Start:**
1. Open CG panel (Layers icon)
2. Select type and click **New** or use **Presets**
3. Click eye icon to show/hide
        `.trim(),
        relatedTopics: ['cg-animations', 'cg-presets'],
    },
    {
        id: 'cg-animations',
        title: 'CG Animations',
        category: 'cg',
        keywords: ['cg', 'animation', 'fade', 'slide'],
        content: `
**Animation options** for smooth CG transitions.

**Animation In:**
fade, slide_left, slide_right, slide_up, slide_down, zoom, bounce

**Animation Out:**
fade, slide_left, slide_right, slide_up, slide_down, zoom

**Loop Effects:**
pulse, sheen, shimmer, glow_pulse, ticker_scroll, bounce

Configure in CG Builder (Edit button on element).
        `.trim(),
    },
    {
        id: 'cg-presets',
        title: 'CG Presets',
        category: 'cg',
        keywords: ['cg', 'preset', 'template'],
        content: `
**Presets** are pre-styled CG templates.

**Built-in Presets:**
- Blue Lower Third
- Red Lower Third
- News Ticker
- Corner Bug
- Live Clock

**Using Presets:**
1. Open CG panel
2. Click **Presets** button
3. Click any preset to add as new element
4. Edit to customize
        `.trim(),
    },

    // --- Analytics ---
    {
        id: 'analytics-overview',
        title: 'Analytics Dashboard',
        category: 'analytics',
        keywords: ['analytics', 'stats', 'report', 'play count'],
        content: `
**Analytics** tracks playout history and performance.

**Summary Cards:**
- Total plays across all media
- Total play time
- Unique items played

**Hourly Chart:**
Visual distribution of plays by hour (useful for peak detection)

**Media Table:**
- Sortable by plays, time, or recency
- Filter by media type
        `.trim(),
    },

    // --- Settings ---
    {
        id: 'settings-overview',
        title: 'Settings Overview',
        category: 'settings',
        keywords: ['settings', 'config', 'preferences'],
        content: `
**Settings** configure automation behavior.

**Sections:**
- **Program** - Latest episode preference, ad shuffling
- **Gap Fill** - Auto-fill with IDs/bumpers
- **Playback** - Auto-advance, loop behavior
- **CG Defaults** - Default animations and duration
- **Analytics** - History retention

Changes save to browser localStorage.
        `.trim(),
    },

    // --- Keyboard Shortcuts ---
    {
        id: 'shortcuts-general',
        title: 'General Shortcuts',
        category: 'shortcuts',
        keywords: ['keyboard', 'shortcut', 'hotkey'],
        content: `
**Keyboard Shortcuts:**

| Key | Action |
|-----|--------|
| **Space** | Play/Pause |
| **S** | Stop |
| **N** | Next item |
| **F** | Fullscreen player |
| **1-4** | Toggle DDR A-D |
| **G** | Toggle CG panel |
| **F1** | Open Help |
| **Esc** | Close modal/panel |
        `.trim(),
    },
];

// Search function
export function searchHelp(query: string): HelpTopic[] {
    if (!query.trim()) return HELP_TOPICS;

    const q = query.toLowerCase();
    return HELP_TOPICS.filter(topic =>
        topic.title.toLowerCase().includes(q) ||
        topic.keywords.some(k => k.includes(q)) ||
        topic.content.toLowerCase().includes(q)
    ).sort((a, b) => {
        // Prioritize title matches
        const aTitle = a.title.toLowerCase().includes(q) ? 0 : 1;
        const bTitle = b.title.toLowerCase().includes(q) ? 0 : 1;
        return aTitle - bTitle;
    });
}

// Get topic by ID
export function getHelpTopic(id: string): HelpTopic | undefined {
    return HELP_TOPICS.find(t => t.id === id);
}

// Get topics by category
export function getTopicsByCategory(categoryId: string): HelpTopic[] {
    return HELP_TOPICS.filter(t => t.category === categoryId);
}
