/**
 * CG Fields runtime (docs/cg-designer.md "Template contract").
 *
 * Templates made in the CG Designer (and any template that wants it) hold no URL-parsing code of their own. They mark
 * their variable parts and include this script after js/cg-animations.js:
 *
 *   <div data-field="name" data-label="Name">Jane Smith</div>        text: the element's own text is the default
 *   <div data-field="body" data-type="textarea">…</div>               multi-line text (style it with white-space: pre-line)
 *   <img data-field="logo" data-type="image" src="">                  image URL (hidden while empty)
 *   <div data-field="sub" data-hide-empty>…</div>                     hidden while its value is empty
 *   :root { --accent: #e11d48; }                                       a CSS variable on :root is a param named "accent"
 *
 * On load: every URL query key fills the matching [data-field] elements and :root variables; text1/text2/text3 fill the
 * first three text fields (document order) when their own names are not given; then the [data-cg-root] element runs
 * the in animation (CGAnimations.init). Live: window.update(fields) (the engine's field push calls it) returns the number
 * of things it changed. Extra template code reads values with CGFields.get(name, fallback) and hears changes through
 * CGFields.onChange(fn(values, changed)).
 */
(function () {
    'use strict';
    var ANIM_KEYS = { animIn: 1, animOut: 1, animInDuration: 1, animOutDuration: 1, animDelay: 1, animEasing: 1, autoHide: 1, loop: 1,
                      remoteUrl: 1, remoteField: 1, remoteInterval: 1, remoteJsonPath: 1, wsUrl: 1, token: 1 };
    var root = document.documentElement;
    var values = {};
    var listeners = [];
    var ready = false;

    function esc(name) { return (window.CSS && CSS.escape) ? CSS.escape(name) : String(name).replace(/["\\]/g, ''); }
    function fieldEls(name) { return document.querySelectorAll('[data-field="' + esc(name) + '"]'); }
    function hasVar(name) {
        if (!/^[A-Za-z_][\w-]*$/.test(name)) return false;
        return getComputedStyle(root).getPropertyValue('--' + name).trim() !== '' || root.style.getPropertyValue('--' + name) !== '';
    }
    function isImage(el) { return el.tagName === 'IMG' || (el.getAttribute('data-type') || '').toLowerCase() === 'image'; }

    function setEl(el, v) {
        if (isImage(el)) {
            if (el.tagName === 'IMG') { if (v) el.setAttribute('src', v); else el.removeAttribute('src'); el.style.visibility = el.getAttribute('src') ? '' : 'hidden'; }
            else el.style.backgroundImage = v ? 'url("' + String(v).replace(/"/g, '%22') + '")' : '';
        } else if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.tagName === 'SELECT') {
            el.value = v;
        } else {
            el.textContent = v;
        }
        if (el.hasAttribute('data-hide-empty')) el.style.display = String(v).trim() === '' ? 'none' : '';
    }

    function applyOne(name, value) {
        if (value === null || value === undefined) return 0;
        var v = String(value), n = 0;
        values[name] = v;
        fieldEls(name).forEach(function (el) { setEl(el, v); n++; });
        if (hasVar(name)) { root.style.setProperty('--' + name, v); n++; }
        return n;
    }

    function textFieldNames() {
        var seen = {}, out = [];
        document.querySelectorAll('[data-field]').forEach(function (el) {
            var f = el.getAttribute('data-field');
            if (!f || seen[f] || isImage(el)) return;
            seen[f] = 1; out.push(f);
        });
        return out.slice(0, 3);
    }

    function update(fields) {
        if (!fields || typeof fields !== 'object') return 0;
        var n = 0, changed = {};
        var tf = textFieldNames();
        for (var i = 1; i <= 3; i++) {                       // text1..3 -> the first three text fields, unless named directly
            var k = 'text' + i;
            if (fields[k] !== undefined && fields[k] !== '' && tf[i - 1] && fields[tf[i - 1]] === undefined && fieldEls(k).length === 0) {
                n += applyOne(tf[i - 1], fields[k]); changed[tf[i - 1]] = String(fields[k]);
            }
        }
        for (var key in fields) {
            if (!Object.prototype.hasOwnProperty.call(fields, key) || ANIM_KEYS[key]) continue;
            var c = applyOne(key, fields[key]);
            if (c) { n += c; changed[key] = String(fields[key]); }
        }
        if (ready) listeners.forEach(function (fn) { try { fn(Object.assign({}, values), changed); } catch (e) { console.warn(e); } });
        return n;
    }

    function init() {
        document.querySelectorAll('[data-field]').forEach(function (el) {    // defaults: what the page holds
            var f = el.getAttribute('data-field');
            if (!f || values[f] !== undefined) return;
            values[f] = isImage(el) ? (el.getAttribute('src') || '') : el.textContent;
            if (isImage(el) && el.tagName === 'IMG' && !el.getAttribute('src')) el.style.visibility = 'hidden';
            if (el.hasAttribute('data-hide-empty') && String(values[f]).trim() === '') el.style.display = 'none';
        });
        var q = new URLSearchParams(window.location.search), fromUrl = {}, any = false;
        q.forEach(function (v, k) { fromUrl[k] = v; any = true; });
        if (any) update(fromUrl);
        ready = true;
        listeners.forEach(function (fn) { try { fn(Object.assign({}, values), {}); } catch (e) { console.warn(e); } });
        var r = document.querySelector('[data-cg-root]');
        if (window.CGAnimations && r && !r.hasAttribute('data-cg-auto')) {
            window.CGAnimations.init('[data-cg-root]');
            if (window.CGAnimations.initRemoteText) window.CGAnimations.initRemoteText();
            if (window.CGAnimations.initWebSocketText) window.CGAnimations.initWebSocketText();
        }
    }

    window.CGFields = {
        get: function (name, fallback) { return values[name] !== undefined && values[name] !== '' ? values[name] : fallback; },
        all: function () { return Object.assign({}, values); },
        set: function (name, value) { var o = {}; o[name] = value; return update(o); },
        onChange: function (fn) { if (typeof fn === 'function') { listeners.push(fn); if (ready) { try { fn(Object.assign({}, values), {}); } catch (e) { console.warn(e); } } } },
        update: update
    };
    if (typeof window.update !== 'function') window.update = update;

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();
