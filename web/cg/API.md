# Broadcast CG Control Center — API Reference

## Overview

The CG Control Center provides:
- **HTTP REST API** for template deployment and management
- **WebSocket API** for real-time remote text updates
- **JavaScript Client Library** (`js/cg-client.js`) for browser/Node.js integration
- **Animation System** (`js/cg-animations.js`) for all templates
- **Remote Text Variables** for live data feeds

---

## HTTP REST API

Base URL: `http://localhost:3000`

### Templates

#### List All Templates
```
GET /api/templates
```
Response:
```json
{
  "success": true,
  "count": 32,
  "templates": [
    { "id": "lower_third_modern", "category": "lower-thirds", "params": ["name","title","position","logo","color1","color2"] }
  ]
}
```

#### Get Template Details
```
GET /api/templates/:id
```

#### List Categories
```
GET /api/categories
```

---

### Deployment

#### Generate Template URL
```
POST /api/generate
Content-Type: application/json

{
  "template": "lower_third_modern",
  "params": {
    "name": "John Doe",
    "title": "Senior Correspondent",
    "position": "left"
  },
  "animation": {
    "in": "slide-left",
    "out": "fade",
    "duration": 600,
    "easing": "ease-out",
    "autoHide": 8000
  }
}
```
Response:
```json
{
  "success": true,
  "url": "http://localhost:3000/lower_third_modern.html?name=John+Doe&title=Senior+Correspondent&animIn=slide-left&animOut=fade&animInDuration=600",
  "template": "lower_third_modern"
}
```

#### Deploy Template
```
POST /api/deploy
Content-Type: application/json

{
  "template": "lower_third_modern",
  "params": { "name": "Jane Smith", "title": "Anchor" },
  "animation": { "in": "wipe-left", "out": "wipe-right", "duration": 500 }
}
```

#### Get Current Output
```
GET /api/output
```

#### Clear Output
```
POST /api/clear
```

#### Deployment History
```
GET /api/history?limit=20
```

---

### Remote Text Variables

Variables are named text values that can be pushed to live templates in real-time.

#### Get All Variables
```
GET /api/variables
```
Response:
```json
{
  "success": true,
  "variables": {
    "ticker_text": "Breaking: Major story developing...",
    "lower_third_name": "John Doe"
  }
}
```

#### Get Variable
```
GET /api/variables/:name
```

#### Set Variable
```
POST /api/variables/:name
Content-Type: application/json

{ "value": "Breaking: New story developing..." }
```

#### Bulk Set Variables
```
POST /api/variables
Content-Type: application/json

{
  "variables": {
    "ticker_text": "Latest news update...",
    "score_home": "3",
    "score_away": "1"
  }
}
```

#### Delete Variable
```
DELETE /api/variables/:name
```

---

### Animation Presets

#### List Presets
```
GET /api/animation-presets
```

#### Get Preset
```
GET /api/animation-presets/:id
```

Available preset IDs: `broadcast`, `news`, `smooth`, `dynamic`, `corporate`, `sports`, `elegant`, `snap`

---

## Animation System

### URL Parameters

All templates support these animation URL parameters:

| Parameter       | Type   | Default    | Description                          |
|----------------|--------|------------|--------------------------------------|
| `animIn`        | string | `none`     | In animation type                    |
| `animOut`       | string | `none`     | Out animation type                   |
| `animInDuration`| number | `600`      | In animation duration (ms)           |
| `animOutDuration`| number| `400`      | Out animation duration (ms)          |
| `animDelay`     | number | `0`        | Delay before in animation (ms)       |
| `animEasing`    | string | `ease-out` | CSS easing function                  |
| `autoHide`      | number | `0`        | Auto-hide after N ms (0 = manual)    |
| `loop`          | string | `''`       | Set to `1` to loop animation         |

### Animation Types

| Type          | Description                    |
|--------------|--------------------------------|
| `none`        | No animation (instant)         |
| `fade`        | Fade in/out                    |
| `slide-left`  | Slide in from left             |
| `slide-right` | Slide in from right            |
| `slide-up`    | Slide in from bottom           |
| `slide-down`  | Slide in from top              |
| `wipe-left`   | Wipe reveal from left          |
| `wipe-right`  | Wipe reveal from right         |
| `bounce`      | Bounce in from bottom          |
| `zoom`        | Zoom in/out                    |
| `flip`        | 3D flip on X axis              |
| `typewriter`  | Typewriter character-by-character |

### Example URL
```
lower_third_modern.html?name=John+Doe&title=Reporter&animIn=slide-left&animOut=fade&animInDuration=600&autoHide=8000
```

---

## Remote Text in Templates

Templates support live text updates via URL parameters:

| Parameter         | Description                              |
|------------------|------------------------------------------|
| `remoteUrl`       | URL to fetch text from                   |
| `remoteField`     | Element ID or data-field to update       |
| `remoteInterval`  | Polling interval in ms                   |
| `remoteJsonPath`  | JSON path to extract value               |
| `wsUrl`           | WebSocket URL for live updates           |

### Example
```
ticker_news.html?text=Loading...&remoteUrl=https://api.example.com/news&remoteField=tickerText&remoteInterval=10000&remoteJsonPath=data.headline
```

---

## WebSocket API

Connect to: `ws://localhost:8080`

### Messages (Client → Server)

#### Subscribe to Variable
```json
{ "type": "subscribe", "variable": "ticker_text" }
```

#### Unsubscribe
```json
{ "type": "unsubscribe", "variable": "ticker_text" }
```

#### Push Variable Update
```json
{ "type": "variable", "name": "ticker_text", "value": "Breaking: New story..." }
```

#### Remote Deploy
```json
{
  "type": "deploy",
  "template": "lower_third_modern",
  "params": { "name": "John Doe", "title": "Reporter" }
}
```

#### Clear Output
```json
{ "type": "clear" }
```

#### Ping
```json
{ "type": "ping" }
```

### Messages (Server → Client)

#### Connected
```json
{
  "type": "connected",
  "clientId": 1,
  "output": null,
  "variables": {}
}
```

#### Variable Update
```json
{ "type": "variable", "name": "ticker_text", "value": "New text..." }
```

#### Output Deployed
```json
{ "type": "output", "data": { "template": "lower_third_modern", "url": "...", "timestamp": "..." } }
```

#### Output Cleared
```json
{ "type": "cleared", "data": {} }
```

---

## JavaScript Client Library

### Installation
```html
<script src="js/cg-client.js"></script>
```

### Basic Usage
```javascript
// Create client
const cg = new CGBroadcastClient({
  serverUrl: 'http://localhost:3000',
  wsUrl: 'ws://localhost:8080'
});

// Show a lower third
cg.lowerThird('John Doe', 'Senior Correspondent', 'left');

// Show breaking news
cg.breakingNews('Major story developing');

// Show with animation
cg.show('lower_third_modern', { name: 'Jane Smith', title: 'Anchor' }, {
  animation: { in: 'slide-left', out: 'fade', inDuration: 600 }
});
```

### Animation Presets
```javascript
// Apply a preset
cg.setAnimationPreset('broadcast');

// Or chain
cg.withAnimation('slide-left', 'fade', 600, 'ease-out')
  .withAutoHide(8000)
  .lowerThird('John Doe', 'Reporter');
```

### Remote Text Variables
```javascript
// Set a variable (updates all subscribed templates)
await cg.setVariable('ticker_text', 'Breaking: New story...');

// Bulk set
await cg.setVariables({
  ticker_text: 'Latest update...',
  score_home: '3',
  score_away: '1'
});

// Poll a URL and update a variable
cg.startPolling('ticker_text', 'https://api.example.com/news', 10000, 'data.headline');

// Stop polling
cg.stopPolling('ticker_text');
```

### WebSocket Subscriptions
```javascript
// Subscribe to variable changes
cg.subscribe('ticker_text', (value) => {
  console.log('Ticker updated:', value);
});

// Listen to events
cg.on('connected', () => console.log('WS connected'));
cg.on('output', (data) => console.log('New output:', data));
```

### Convenience Methods

| Method | Description |
|--------|-------------|
| `lowerThird(name, title, position, options)` | Show lower third |
| `breakingNews(text, animation)` | Breaking news banner |
| `emergency(message)` | Emergency alert |
| `countdown(target, label)` | Countdown timer |
| `ticker(text, label, speed, animation)` | News ticker |
| `scoreBug(team1, score1, team2, score2, period, time)` | Sports score bug |
| `live(text)` | LIVE indicator |
| `recording(text)` | REC indicator |
| `weather(location)` | Weather forecast |
| `tweet(handle, text, name)` | Twitter overlay |
| `youtubeSubscribe(channel, subscribers)` | YouTube subscribe |
| `titleCard(title, subtitle)` | Fullscreen title card |
| `comingUp(title, items)` | Coming up next |

---

## Template Animation Integration

To add animation support to a template, include `cg-animations.js` and add `data-cg-auto` to the root element:

```html
<div class="my-template" data-cg-auto>
  <!-- template content -->
</div>

<script src="js/cg-animations.js"></script>
```

The animation system automatically reads URL parameters and applies the appropriate animations.

For remote text, add `data-field="fieldName"` to elements that should be updated:

```html
<div class="ticker-text" data-field="ticker_text">Default text</div>
```

---

## Template Categories

| Category | Templates |
|----------|-----------|
| `lower-thirds` | lower_third_modern, lower_third_classic, lower_third_corporate, lower_third_minimal, lower_third_sport, lower_third_sports, lower_third |
| `banners` | banner_breaking_news, banner_special_report, fullscreen_breaking_news |
| `fullscreen` | fullscreen_coming_up, fullscreen_end_credits, fullscreen_gfx, fullscreen_title_card |
| `tickers` | ticker_news, ticker_sports_scores, ticker_stock_market, ticker |
| `score-bugs` | score_bug_sports, score_bug_minimal |
| `weather` | weather_forecast_ai, weather_forecast_voice, weather_bug_rotating |
| `bugs` | bug_live_indicator, bug_logo_corner, bug_recording, master_overlay |
| `social` | social_twitter, social_youtube_subscribe |
| `special` | countdown_timer, emergency, directors_scene |
