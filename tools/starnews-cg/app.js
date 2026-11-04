'use strict';
const $ = id => document.getElementById(id);
const fields = ['type', 'headline', 'detail', 'label', 'blue', 'accent', 'inMs', 'outMs'];
let scene, timer, previewTimer, signature = '';
async function api(path, data) {
  const response = await fetch(path, data === undefined ? {cache:'no-store'} : {method:'POST', headers:{'Content-Type':'application/json','X-CG-Control':'1'}, body:JSON.stringify(data)});
  const result = await response.json(); if (!response.ok) throw Error(result.error || 'Request failed'); return result;
}
function report(error) { $('error').textContent = error ? error.message : ''; }
function form() { for (const key of fields) $(key).value = scene[key]; }
function drawPreview(phase='IDLE') { clearTimeout(previewTimer); CGRender($('preview'), scene, phase); if (phase === 'OUT') previewTimer = setTimeout(() => CGRender($('preview'), scene, 'CLEARED'), scene.outMs); }
function changed() {
  for (const key of fields) scene[key] = key.endsWith('Ms') ? Math.max(0, Math.min(10000, Number($(key).value) || 0)) : $(key).value;
  drawPreview(); $('saveStatus').textContent = 'Saving…'; clearTimeout(timer);
  timer = setTimeout(async () => { try { await api('/api/project', scene); $('saveStatus').textContent = 'Saved locally'; report(); } catch (e) { $('saveStatus').textContent = 'Save failed'; report(e); } }, 350);
}
async function command(cmd) { try { await api('/api/command', {channel:$('channel').value, command:cmd, scene}); await poll(); report(); } catch(e) { report(e); } }
async function poll() {
  try {
    const state = await api('/api/channel/' + $('channel').value);
    $('connection').textContent = 'Connected'; $('phase').textContent = state.phase;
    $('onAir').textContent = state.phase === 'CLEARED' ? 'Nothing on air' : (state.scene.headline || state.scene.label || 'Untitled scene');
    const current = JSON.stringify([$('channel').value, state]);
    if (current !== signature) { signature = current; CGRender($('program'), state.scene, state.phase, Math.max(0, Date.now() - state.since)); }
  } catch(e) { $('connection').textContent = 'Disconnected · last known picture retained'; }
}
function updateURL() { $('url').value = location.origin + '/output?channel=' + encodeURIComponent($('channel').value); signature = ''; poll(); }
fields.forEach(key => $(key).addEventListener('input', changed));
$('previewIn').onclick = () => drawPreview('IN'); $('previewOut').onclick = () => drawPreview('OUT'); $('previewReset').onclick = () => drawPreview();
$('take').onclick = () => command('take'); $('out').onclick = () => command('out');
$('clear').onclick = () => { if (confirm('Clear this channel immediately?')) command('clear'); };
$('panic').onclick = async () => { if (confirm('Clear ALL program channels immediately?')) { try { await api('/api/panic', {}); await poll(); report(); } catch(e) { report(e); } } };
$('copy').onclick = async () => { try { await navigator.clipboard.writeText($('url').value); $('copy').textContent = 'Copied'; setTimeout(() => $('copy').textContent = 'Copy URL', 1500); } catch(e) { $('url').select(); report(Error('Clipboard unavailable; copy the selected URL manually.')); } };
$('channel').onchange = updateURL;
api('/api/project').then(value => { scene = value; form(); drawPreview(); $('saveStatus').textContent = 'Saved locally'; updateURL(); setInterval(poll, 300); }).catch(report);
