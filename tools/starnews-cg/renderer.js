/* Shared local renderer. Text is assigned through textContent, never HTML. */
'use strict';
window.CGRender = function(root, scene, phase, elapsed = 0) {
  root.replaceChildren();
  if (!scene || phase === 'CLEARED') return;
  const outer = document.createElement('div'); outer.className = 'graphic ' + scene.type;
  outer.style.setProperty('--blue', scene.blue); outer.style.setProperty('--accent', scene.accent);
  function add(parent, tag, cls, text) { const el = document.createElement(tag); el.className = cls; el.textContent = text; parent.appendChild(el); return el; }
  if (scene.type === 'starfield') { for (let i = 0; i < 36; i++) { const star = add(outer, 'i', 'star', ''); star.style.left = ((i * 137 + 29) % 100) + '%'; star.style.top = ((i * 61 + 11) % 100) + '%'; star.style.animationDelay = -(i % 10) + 's'; } }
  const panel = add(outer, 'div', 'plate', '');
  add(panel, 'div', 'label', scene.label || 'GENERIC LOGO');
  const words = add(panel, 'div', 'words', '');
  add(words, 'div', 'headline', scene.headline); add(words, 'div', 'detail', scene.detail);
  if (scene.type === 'weather') add(panel, 'div', 'sample', 'SAMPLE · VERIFY SOURCE');
  root.appendChild(outer);
  const duration = phase === 'IN' ? scene.inMs : phase === 'OUT' ? scene.outMs : 0;
  if (duration > 0 && elapsed < duration) {
    const start = Math.max(0, elapsed);
    outer.animate(phase === 'IN' ? [{opacity: 0, transform: 'translateY(2.2%)'}, {opacity: 1, transform: 'translateY(0)'}] : [{opacity: 1}, {opacity: 0}], {duration, delay: -start, easing: 'ease-out', fill: 'both'});
  }
};
