/**
 * Broadcast CG Client Library
 * Standalone JavaScript client for deploying CG templates
 * Features: Animation types, Remote text, WebSocket live updates
 * Works without the Node.js server (standalone mode)
 */

class CGBroadcastClient {
    constructor(options = {}) {
        // Support legacy string argument (baseUrl)
        if (typeof options === 'string') {
            options = { baseUrl: options };
        }

        this.baseUrl       = options.baseUrl || this._getBaseUrl();
        this.serverUrl     = options.serverUrl || null; // e.g. 'http://localhost:3000'
        this.wsUrl         = options.wsUrl || null;     // e.g. 'ws://localhost:8080'
        this.currentOutput = null;
        this._outputWindow = null;
        this._ws           = null;
        this._wsCallbacks  = {};
        this._variables    = {};
        this._pollingIntervals = {};

        // Default animation settings
        this.defaultAnimation = {
            in:          options.animIn          || 'none',
            out:         options.animOut         || 'none',
            inDuration:  options.animInDuration  || 600,
            outDuration: options.animOutDuration || 400,
            easing:      options.easing          || 'ease-out',
            delay:       options.delay           || 0,
            autoHide:    options.autoHide        || 0,
            loop:        options.loop            || false
        };

        // Auto-connect WebSocket if URL provided
        if (this.wsUrl) this._connectWebSocket();
    }

    // ============================================================
    // INTERNAL HELPERS
    // ============================================================

    _getBaseUrl() {
        const path = window.location.pathname;
        const dir  = path.substring(0, path.lastIndexOf('/'));
        return window.location.origin + dir;
    }

    _buildAnimParams(animation) {
        const anim = Object.assign({}, this.defaultAnimation, animation || {});
        const params = {};
        if (anim.in  && anim.in  !== 'none') { params.animIn  = anim.in;  params.animInDuration  = anim.inDuration; }
        if (anim.out && anim.out !== 'none') { params.animOut = anim.out; params.animOutDuration = anim.outDuration; }
        if (anim.easing && anim.easing !== 'ease-out') params.animEasing = anim.easing;
        if (anim.delay  > 0) params.animDelay = anim.delay;
        if (anim.autoHide > 0) params.autoHide = anim.autoHide;
        if (anim.loop)         params.loop = '1';
        return params;
    }

    // ============================================================
    // URL GENERATION
    // ============================================================

    /**
     * Generate a template URL with parameters and animation
     * @param {string} template - Template name
     * @param {Object} params   - Template parameters
     * @param {Object} animation - Animation options
     * @returns {string} Full URL
     */
    generateUrl(template, params = {}, animation = {}) {
        const allParams = { ...params, ...this._buildAnimParams(animation) };
        const queryString = new URLSearchParams(allParams).toString();
        return `${this.baseUrl}/${template}.html${queryString ? '?' + queryString : ''}`;
    }

    // ============================================================
    // SHOW / DEPLOY
    // ============================================================

    /**
     * Open template in a new window (broadcast output)
     * @param {string} template  - Template name
     * @param {Object} params    - Template parameters
     * @param {Object} options   - Window + animation options
     * @returns {Window} The opened window
     */
    show(template, params = {}, options = {}) {
        const animation = options.animation || {};
        const url = this.generateUrl(template, params, animation);

        const width  = options.width  || 1920;
        const height = options.height || 1080;
        const target = options.target || 'cg_output';

        this._outputWindow = window.open(url, target, `width=${width},height=${height}`);

        this.currentOutput = {
            template,
            params,
            animation,
            url,
            timestamp: new Date().toISOString()
        };

        // Notify server if available
        if (this.serverUrl) {
            this._serverDeploy(template, params, animation).catch(() => {});
        }

        return this._outputWindow;
    }

    /**
     * Deploy via server API (returns promise)
     */
    async deploy(template, params = {}, animation = {}) {
        if (this.serverUrl) {
            return this._serverDeploy(template, params, animation);
        }
        return this.show(template, params, { animation });
    }

    async _serverDeploy(template, params, animation) {
        const response = await fetch(`${this.serverUrl}/api/deploy`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ template, params, animation })
        });
        return response.json();
    }

    // ============================================================
    // UPDATE / CLEAR
    // ============================================================

    /**
     * Update the current output with new parameters
     */
    update(params, animation) {
        if (!this.currentOutput) {
            console.warn('[CG] No active output to update');
            return;
        }
        const url = this.generateUrl(this.currentOutput.template, params, animation || this.currentOutput.animation);
        if (this._outputWindow && !this._outputWindow.closed) {
            this._outputWindow.location.href = url;
        } else {
            this.show(this.currentOutput.template, params, { animation });
        }
        this.currentOutput.params = params;
        this.currentOutput.url    = url;
    }

    /**
     * Clear the current output
     */
    clear() {
        if (this._outputWindow && !this._outputWindow.closed) {
            this._outputWindow.close();
        }
        this.currentOutput = null;
        if (this.serverUrl) {
            fetch(`${this.serverUrl}/api/clear`, { method: 'POST' }).catch(() => {});
        }
    }

    getCurrentOutput() { return this.currentOutput; }

    // ============================================================
    // ANIMATION PRESETS
    // ============================================================

    /**
     * Set default animation preset by name
     * @param {string} presetId - Preset ID (broadcast, news, smooth, dynamic, corporate, sports, elegant, snap)
     */
    setAnimationPreset(presetId) {
        const presets = {
            broadcast:  { in: 'slide-left',  out: 'slide-right', inDuration: 600,  easing: 'ease-out' },
            news:       { in: 'wipe-left',   out: 'wipe-right',  inDuration: 500,  easing: 'ease-in-out' },
            smooth:     { in: 'fade',        out: 'fade',        inDuration: 800,  easing: 'ease-in-out' },
            dynamic:    { in: 'bounce',      out: 'slide-down',  inDuration: 700,  easing: 'cubic-bezier(0.34,1.56,0.64,1)' },
            corporate:  { in: 'slide-up',    out: 'slide-down',  inDuration: 500,  easing: 'ease-out' },
            sports:     { in: 'zoom',        out: 'zoom',        inDuration: 400,  easing: 'ease-out' },
            elegant:    { in: 'fade',        out: 'fade',        inDuration: 1200, easing: 'ease-in-out' },
            snap:       { in: 'slide-right', out: 'slide-left',  inDuration: 300,  easing: 'ease-out' },
            typewriter: { in: 'typewriter',  out: 'fade',        inDuration: 1000, easing: 'linear' },
            flip:       { in: 'flip',        out: 'flip',        inDuration: 600,  easing: 'ease-in-out' }
        };
        const preset = presets[presetId];
        if (preset) {
            Object.assign(this.defaultAnimation, preset);
        } else {
            console.warn(`[CG] Unknown preset: ${presetId}`);
        }
        return this;
    }

    /**
     * Set animation for next show() call
     */
    withAnimation(animIn, animOut, duration = 600, easing = 'ease-out') {
        this.defaultAnimation.in         = animIn;
        this.defaultAnimation.out        = animOut;
        this.defaultAnimation.inDuration = duration;
        this.defaultAnimation.easing     = easing;
        return this; // chainable
    }

    /**
     * Set auto-hide duration
     */
    withAutoHide(ms) {
        this.defaultAnimation.autoHide = ms;
        return this;
    }

    // ============================================================
    // REMOTE TEXT / VARIABLES
    // ============================================================

    /**
     * Set a remote text variable (via server API)
     * @param {string} name  - Variable name
     * @param {string} value - Variable value
     */
    async setVariable(name, value) {
        this._variables[name] = value;
        if (this.serverUrl) {
            await fetch(`${this.serverUrl}/api/variables/${name}`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ value })
            });
        }
        // Also push via WebSocket
        if (this._ws && this._ws.readyState === WebSocket.OPEN) {
            this._ws.send(JSON.stringify({ type: 'variable', name, value }));
        }
    }

    /**
     * Get a variable value
     */
    async getVariable(name) {
        if (this.serverUrl) {
            const res = await fetch(`${this.serverUrl}/api/variables/${name}`);
            const data = await res.json();
            return data.value;
        }
        return this._variables[name];
    }

    /**
     * Bulk set variables
     */
    async setVariables(vars) {
        Object.assign(this._variables, vars);
        if (this.serverUrl) {
            await fetch(`${this.serverUrl}/api/variables`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ variables: vars })
            });
        }
    }

    /**
     * Poll a URL and update a variable at an interval
     * @param {string} varName      - Variable name to update
     * @param {string} url          - URL to fetch
     * @param {number} intervalMs   - Polling interval in ms
     * @param {string} jsonPath     - Optional JSON path (e.g. 'data.text')
     * @param {Function} transform  - Optional transform function
     */
    startPolling(varName, url, intervalMs = 30000, jsonPath = '', transform = null) {
        this.stopPolling(varName);

        const fetchAndSet = async () => {
            try {
                const response = await fetch(url);
                if (!response.ok) return;
                const data = await response.text();
                let value = data;
                if (jsonPath) {
                    try {
                        const parsed = JSON.parse(data);
                        value = jsonPath.split('.').reduce((obj, key) => {
                            const arrMatch = key.match(/^(\w+)\[(\d+)\]$/);
                            if (arrMatch) return obj[arrMatch[1]][parseInt(arrMatch[2])];
                            return obj[key];
                        }, parsed);
                    } catch { /* use raw */ }
                }
                if (transform) value = transform(String(value));
                await this.setVariable(varName, String(value));
            } catch (e) {
                console.warn(`[CG] Polling error for ${varName}:`, e);
            }
        };

        fetchAndSet();
        this._pollingIntervals[varName] = setInterval(fetchAndSet, intervalMs);
        return this;
    }

    /**
     * Stop polling for a variable
     */
    stopPolling(varName) {
        if (this._pollingIntervals[varName]) {
            clearInterval(this._pollingIntervals[varName]);
            delete this._pollingIntervals[varName];
        }
        return this;
    }

    /**
     * Stop all polling
     */
    stopAllPolling() {
        Object.keys(this._pollingIntervals).forEach(k => this.stopPolling(k));
        return this;
    }

    // ============================================================
    // WEBSOCKET
    // ============================================================

    _connectWebSocket() {
        if (!this.wsUrl) return;

        this._ws = new WebSocket(this.wsUrl);

        this._ws.onopen = () => {
            console.log('[CG] WebSocket connected');
            this._emit('connected');
        };

        this._ws.onmessage = (event) => {
            try {
                const msg = JSON.parse(event.data);
                this._emit(msg.type, msg);
                if (msg.type === 'variable') {
                    this._variables[msg.name] = msg.value;
                    this._emit('variable:' + msg.name, msg.value);
                }
            } catch { /* ignore */ }
        };

        this._ws.onclose = () => {
            console.log('[CG] WebSocket disconnected, reconnecting...');
            this._emit('disconnected');
            setTimeout(() => this._connectWebSocket(), 3000);
        };

        this._ws.onerror = (err) => {
            console.warn('[CG] WebSocket error:', err);
        };
    }

    /**
     * Subscribe to a variable via WebSocket
     */
    subscribe(varName, callback) {
        if (this._ws && this._ws.readyState === WebSocket.OPEN) {
            this._ws.send(JSON.stringify({ type: 'subscribe', variable: varName }));
        }
        this.on('variable:' + varName, callback);
        return this;
    }

    on(event, callback) {
        if (!this._wsCallbacks[event]) this._wsCallbacks[event] = [];
        this._wsCallbacks[event].push(callback);
        return this;
    }

    _emit(event, data) {
        (this._wsCallbacks[event] || []).forEach(cb => cb(data));
    }

    // ============================================================
    // CONVENIENCE METHODS
    // ============================================================

    /**
     * Show a lower third
     */
    lowerThird(name, title, position = 'left', options = {}) {
        return this.show('lower_third_modern', { name, title, position, ...options });
    }

    /**
     * Show a breaking news banner
     */
    breakingNews(text, animation) {
        return this.show('banner_breaking_news', { text }, { animation });
    }

    /**
     * Show emergency alert
     */
    emergency(message) {
        return this.show('emergency', { message });
    }

    /**
     * Show a countdown timer
     */
    countdown(target, label = 'Time Remaining') {
        const targetStr = target instanceof Date ? target.toISOString() : target;
        return this.show('countdown_timer', { target: targetStr, label });
    }

    /**
     * Show a news ticker
     */
    ticker(text, label = 'NEWS', speed = 100, animation) {
        return this.show('ticker_news', { text, label, speed }, { animation });
    }

    /**
     * Show a sports score bug
     */
    scoreBug(team1, score1, team2, score2, period = 'Q1', time = '12:00') {
        return this.show('score_bug_sports', { team1, score1, team2, score2, period, time });
    }

    /**
     * Show live indicator
     */
    live(text = 'LIVE') {
        return this.show('bug_live_indicator', { text });
    }

    /**
     * Show recording indicator
     */
    recording(text = 'REC') {
        return this.show('bug_recording', { text });
    }

    /**
     * Show weather forecast
     */
    weather(location) {
        return this.show('weather_forecast_ai', { location });
    }

    /**
     * Show social Twitter overlay
     */
    tweet(handle, text, name) {
        return this.show('social_twitter', { handle, text, name });
    }

    /**
     * Show YouTube subscribe overlay
     */
    youtubeSubscribe(channel, subscribers) {
        return this.show('social_youtube_subscribe', { channel, subscribers });
    }

    /**
     * Show fullscreen title card
     */
    titleCard(title, subtitle = '') {
        return this.show('fullscreen_title_card', { title, subtitle });
    }

    /**
     * Show fullscreen coming up
     */
    comingUp(title, items) {
        const itemsStr = Array.isArray(items) ? items.join(', ') : items;
        return this.show('fullscreen_coming_up', { title, items: itemsStr });
    }
}

// ============================================================
// EXPORT
// ============================================================
if (typeof module !== 'undefined' && module.exports) {
    module.exports = CGBroadcastClient;
} else if (typeof window !== 'undefined') {
    window.CGBroadcast = CGBroadcastClient;
}

// Create default instance
const CG = new CGBroadcastClient();
