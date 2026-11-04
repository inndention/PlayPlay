/* Star News browser-CG lifecycle. Clear is manual unless autoHide is supplied. */
(function () {
    'use strict';
    function fields(value) {
        if (typeof value !== 'string') return value;
        try { return JSON.parse(value); } catch (_) {}
        const doc = new DOMParser().parseFromString(value, 'application/xml'), result = {};
        if (doc.querySelector('parsererror')) return {};
        doc.querySelectorAll('componentData[id]').forEach(el => { const d = el.querySelector('data'); result[el.getAttribute('id')] = d ? d.getAttribute('value') || d.textContent : el.textContent; });
        return result;
    }
    window.update = value => window.CGFields ? window.CGFields.update(fields(value)) : 0;
    function animate(out) {
        const root = document.querySelector('[data-cg-root]'); if (!root || !window.CGAnimations) return;
        const q = new URLSearchParams(location.search);
        const ms = Number(q.get(out ? 'animOutDuration' : 'animInDuration')) || (out ? 350 : 420);
        window.CGAnimations[out ? 'animateOut' : 'animateIn'](root, q.get(out ? 'animOut' : 'animIn') || 'fade', ms, 'ease-out');
    }
    window.play = () => animate(false); window.stop = () => animate(true); window.next = () => {};
    window.GFXController = {play: window.play, stop: window.stop, next: window.next, update: window.update}; window.spx = window.GFXController;
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', window.play); else window.play();
})();
