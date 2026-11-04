'use strict';
const channel = new URLSearchParams(location.search).get('channel') || 'lower';
let last = '', serverOffset = 0;
async function poll() {
  try {
    const response = await fetch('/api/channel/' + encodeURIComponent(channel), {cache: 'no-store'});
    if (!response.ok) throw Error('Output unavailable');
    const state = await response.json(); const signature = JSON.stringify(state);
    if (signature !== last) { last = signature; CGRender(document.getElementById('output'), state.scene, state.phase, Math.max(0, Date.now() - state.since - serverOffset)); }
  } catch (_) { /* retain last-known picture on disconnect */ }
}
poll(); setInterval(poll, 250);
