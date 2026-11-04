/**
 * CG Animations Library
 * Shared animation system for all CG templates
 * Reads URL params: animIn, animOut, animInDuration, animOutDuration, animDelay, animEasing, autoHide, loop
 */

(function() {
    'use strict';

    // ============================================================
    // ANIMATION CSS KEYFRAMES
    // ============================================================
    const KEYFRAMES = `
        @keyframes cg-phase-in {
            0% { opacity: 0; filter: blur(12px) brightness(1.8); transform: scale(1.04); }
            65% { opacity: .8; filter: blur(2px) brightness(1.25); }
            100% { opacity: 1; filter: blur(0) brightness(1); transform: scale(1); }
        }
        @keyframes cg-phase-out {
            0% { opacity: 1; filter: blur(0) brightness(1); transform: scale(1); }
            100% { opacity: 0; filter: blur(12px) brightness(1.8); transform: scale(1.04); }
        }
        /* Fade */
        @keyframes cg-fade-in {
            from { opacity: 0; }
            to   { opacity: 1; }
        }
        @keyframes cg-fade-out {
            from { opacity: 1; }
            to   { opacity: 0; }
        }

        /* Slide Left (enter from left) */
        @keyframes cg-slide-left-in {
            from { transform: translateX(-120%); opacity: 0; }
            to   { transform: translateX(0);     opacity: 1; }
        }
        @keyframes cg-slide-left-out {
            from { transform: translateX(0);     opacity: 1; }
            to   { transform: translateX(-120%); opacity: 0; }
        }

        /* Slide Right (enter from right) */
        @keyframes cg-slide-right-in {
            from { transform: translateX(120%);  opacity: 0; }
            to   { transform: translateX(0);     opacity: 1; }
        }
        @keyframes cg-slide-right-out {
            from { transform: translateX(0);     opacity: 1; }
            to   { transform: translateX(120%);  opacity: 0; }
        }

        /* Slide Up (enter from bottom) */
        @keyframes cg-slide-up-in {
            from { transform: translateY(80px);  opacity: 0; }
            to   { transform: translateY(0);     opacity: 1; }
        }
        @keyframes cg-slide-up-out {
            from { transform: translateY(0);     opacity: 1; }
            to   { transform: translateY(-80px); opacity: 0; }
        }

        /* Slide Down (enter from top) */
        @keyframes cg-slide-down-in {
            from { transform: translateY(-80px); opacity: 0; }
            to   { transform: translateY(0);     opacity: 1; }
        }
        @keyframes cg-slide-down-out {
            from { transform: translateY(0);     opacity: 1; }
            to   { transform: translateY(80px);  opacity: 0; }
        }

        /* Wipe Left (reveal from left) */
        @keyframes cg-wipe-left-in {
            from { clip-path: inset(0 100% 0 0); }
            to   { clip-path: inset(0 0% 0 0);   }
        }
        @keyframes cg-wipe-left-out {
            from { clip-path: inset(0 0% 0 0);   }
            to   { clip-path: inset(0 0% 0 100%); }
        }

        /* Wipe Right (reveal from right) */
        @keyframes cg-wipe-right-in {
            from { clip-path: inset(0 0% 0 100%); }
            to   { clip-path: inset(0 0% 0 0);    }
        }
        @keyframes cg-wipe-right-out {
            from { clip-path: inset(0 0% 0 0);   }
            to   { clip-path: inset(0 100% 0 0); }
        }

        /* Bounce */
        @keyframes cg-bounce-in {
            0%   { transform: translateY(60px) scale(0.9); opacity: 0; }
            60%  { transform: translateY(-12px) scale(1.02); opacity: 1; }
            80%  { transform: translateY(6px) scale(0.99); }
            100% { transform: translateY(0) scale(1); opacity: 1; }
        }
        @keyframes cg-bounce-out {
            0%   { transform: translateY(0) scale(1); opacity: 1; }
            30%  { transform: translateY(-10px) scale(1.02); }
            100% { transform: translateY(60px) scale(0.9); opacity: 0; }
        }

        /* Zoom */
        @keyframes cg-zoom-in {
            from { transform: scale(0.5); opacity: 0; }
            to   { transform: scale(1);   opacity: 1; }
        }
        @keyframes cg-zoom-out {
            from { transform: scale(1);   opacity: 1; }
            to   { transform: scale(0.5); opacity: 0; }
        }

        /* Flip */
        @keyframes cg-flip-in {
            from { transform: perspective(600px) rotateX(-90deg); opacity: 0; }
            to   { transform: perspective(600px) rotateX(0deg);   opacity: 1; }
        }
        @keyframes cg-flip-out {
            from { transform: perspective(600px) rotateX(0deg);   opacity: 1; }
            to   { transform: perspective(600px) rotateX(90deg);  opacity: 0; }
        }
    `;

    // ============================================================
    // ANIMATION MAP
    // ============================================================
    const ANIM_MAP = {
        'phase':       { in: 'cg-phase-in',       out: 'cg-phase-out' },
        'fade':        { in: 'cg-fade-in',        out: 'cg-fade-out' },
        'slide-left':  { in: 'cg-slide-left-in',  out: 'cg-slide-left-out' },
        'slide-right': { in: 'cg-slide-right-in', out: 'cg-slide-right-out' },
        'slide-up':    { in: 'cg-slide-up-in',    out: 'cg-slide-up-out' },
        'slide-down':  { in: 'cg-slide-down-in',  out: 'cg-slide-down-out' },
        'wipe-left':   { in: 'cg-wipe-left-in',   out: 'cg-wipe-left-out' },
        'wipe-right':  { in: 'cg-wipe-right-in',  out: 'cg-wipe-right-out' },
        'bounce':      { in: 'cg-bounce-in',       out: 'cg-bounce-out' },
        'zoom':        { in: 'cg-zoom-in',         out: 'cg-zoom-out' },
        'flip':        { in: 'cg-flip-in',         out: 'cg-flip-out' }
    };

    // ============================================================
    // INJECT KEYFRAMES
    // ============================================================
    function injectKeyframes() {
        if (document.getElementById('cg-anim-keyframes')) return;
        const style = document.createElement('style');
        style.id = 'cg-anim-keyframes';
        style.textContent = KEYFRAMES;
        document.head.appendChild(style);
    }

    // ============================================================
    // TYPEWRITER EFFECT
    // ============================================================
    function applyTypewriter(element, duration) {
        const text = element.textContent;
        element.textContent = '';
        element.style.visibility = 'visible';
        const charDelay = duration / text.length;
        let i = 0;
        const interval = setInterval(() => {
            element.textContent += text[i];
            i++;
            if (i >= text.length) clearInterval(interval);
        }, charDelay);
    }

    // ============================================================
    // APPLY ANIMATION TO ELEMENT
    // ============================================================
    function animateIn(element, type, duration, easing, delay, callback) {
        injectKeyframes();
        if (!type || type === 'none') {
            element.style.opacity = '1';
            element.style.visibility = 'visible';
            if (callback) setTimeout(callback, delay);
            return;
        }

        if (type === 'typewriter') {
            element.style.visibility = 'visible';
            element.style.opacity = '1';
            setTimeout(() => {
                const textEl = element.querySelector('[data-typewriter]') || element;
                applyTypewriter(textEl, duration);
                if (callback) setTimeout(callback, duration + delay);
            }, delay);
            return;
        }

        const anim = ANIM_MAP[type];
        if (!anim) {
            element.style.opacity = '1';
            element.style.visibility = 'visible';
            if (callback) setTimeout(callback, delay);
            return;
        }

        element.style.visibility = 'visible';
        element.style.opacity = '0';

        setTimeout(() => {
            element.style.animation = `${anim.in} ${duration}ms ${easing} forwards`;
            element.addEventListener('animationend', function finish(event) {
                if (event.target !== element) return;
                element.removeEventListener('animationend', finish);
                element.style.opacity = '1';
                element.style.animation = '';
                if (callback) callback();
            });
        }, delay);
    }

    function animateOut(element, type, duration, easing, callback) {
        injectKeyframes();
        if (!type || type === 'none') {
            element.style.opacity = '0';
            element.style.visibility = 'hidden';
            if (callback) callback();
            return;
        }

        const anim = ANIM_MAP[type];
        if (!anim) {
            element.style.opacity = '0';
            element.style.visibility = 'hidden';
            if (callback) callback();
            return;
        }

        element.style.animation = `${anim.out} ${duration}ms ${easing} forwards`;
        element.addEventListener('animationend', function finish(event) {
            if (event.target !== element) return;
            element.removeEventListener('animationend', finish);
            element.style.opacity = '0';
            element.style.visibility = 'hidden';
            element.style.animation = '';
            if (callback) callback();
        });
    }

    // ============================================================
    // MAIN INIT — Called by templates
    // ============================================================
    function initAnimations(targetSelector) {
        const params = new URLSearchParams(window.location.search);
        const animIn       = params.get('animIn')       || 'none';
        const animOut      = params.get('animOut')      || 'none';
        const inDuration   = parseInt(params.get('animInDuration'))  || 600;
        const outDuration  = parseInt(params.get('animOutDuration')) || 400;
        const delay        = parseInt(params.get('animDelay'))       || 0;
        const easing       = params.get('animEasing')   || 'ease-out';
        const autoHide     = parseInt(params.get('autoHide'))        || 0;
        const loop         = params.get('loop') === '1';

        injectKeyframes();

        const target = document.querySelector(targetSelector || '[data-cg-root]') || document.body.firstElementChild;
        if (!target) return;

        // Initially hidden
        target.style.opacity = '0';
        target.style.visibility = 'hidden';

        // Animate in
        animateIn(target, animIn, inDuration, easing, delay, () => {
            // Auto-hide
            if (autoHide > 0) {
                setTimeout(() => {
                    animateOut(target, animOut, outDuration, easing, () => {
                        if (loop) {
                            // Replay
                            setTimeout(() => {
                                animateIn(target, animIn, inDuration, easing, 0);
                            }, 500);
                        }
                    });
                }, autoHide);
            }
        });

        // Expose control API
        window.CGAnim = {
            animateIn:  (cb) => animateIn(target, animIn, inDuration, easing, 0, cb),
            animateOut: (cb) => animateOut(target, animOut, outDuration, easing, cb),
            replay: () => {
                target.style.opacity = '0';
                target.style.visibility = 'hidden';
                target.style.animation = '';
                setTimeout(() => animateIn(target, animIn, inDuration, easing, 0), 100);
            }
        };
    }

    // ============================================================
    // REMOTE TEXT POLLING
    // ============================================================
    function initRemoteText() {
        const params = new URLSearchParams(window.location.search);
        const remoteUrl      = params.get('remoteUrl');
        const remoteField    = params.get('remoteField');
        const remoteInterval = parseInt(params.get('remoteInterval')) || 0;
        const remoteJsonPath = params.get('remoteJsonPath') || '';

        if (!remoteUrl || !remoteField || !remoteInterval) return;

        async function fetchAndUpdate() {
            try {
                const response = await fetch(remoteUrl);
                if (!response.ok) return;
                const data = await response.text();
                let value = data;
                if (remoteJsonPath) {
                    try {
                        const parsed = JSON.parse(data);
                        value = remoteJsonPath.split('.').reduce((obj, key) => {
                            const arrMatch = key.match(/^(\w+)\[(\d+)\]$/);
                            if (arrMatch) return obj[arrMatch[1]][parseInt(arrMatch[2])];
                            return obj[key];
                        }, parsed);
                    } catch { /* use raw */ }
                }
                // Update all elements with matching data-field attribute
                document.querySelectorAll(`[data-field="${remoteField}"]`).forEach(el => {
                    el.textContent = String(value);
                });
                // Also update by ID
                const el = document.getElementById(remoteField);
                if (el) el.textContent = String(value);
            } catch (e) {
                console.warn('Remote text fetch error:', e);
            }
        }

        fetchAndUpdate();
        setInterval(fetchAndUpdate, remoteInterval);
    }

    // ============================================================
    // WEBSOCKET REMOTE TEXT
    // ============================================================
    function initWebSocketText() {
        const params = new URLSearchParams(window.location.search);
        const wsUrl = params.get('wsUrl');
        if (!wsUrl) return;

        function connect() {
            const ws = new WebSocket(wsUrl);
            ws.onmessage = (event) => {
                try {
                    const msg = JSON.parse(event.data);
                    if (msg.type === 'variable' && msg.name && msg.value !== undefined) {
                        document.querySelectorAll(`[data-field="${msg.name}"]`).forEach(el => {
                            el.textContent = String(msg.value);
                        });
                        const el = document.getElementById(msg.name);
                        if (el) el.textContent = String(msg.value);
                    }
                } catch { /* ignore */ }
            };
            ws.onclose = () => setTimeout(connect, 3000); // Auto-reconnect
        }

        connect();
    }

    // ============================================================
    // EXPOSE GLOBALLY
    // ============================================================
    window.CGAnimations = {
        init: initAnimations,
        initRemoteText,
        initWebSocketText,
        animateIn,
        animateOut
    };

    // Auto-init if data-cg-auto attribute is present
    document.addEventListener('DOMContentLoaded', () => {
        const autoEl = document.querySelector('[data-cg-auto]');
        if (autoEl) {
            initAnimations('[data-cg-auto]');
            initRemoteText();
            initWebSocketText();
        }
    });

})();
