const A = { cfg: null, nodes: [], loading: false, last: 0, selected: '', count: 1, draft: {} };
const E = s => String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
function adopt(id) { A.selected = id; const p = (A.cfg?.channels || []).find(c => c.id === id); A.draft = Object.assign({mode:'local', nodeId:'', channelId:'main', inputUrl:''}, p?.slave || {}); }
async function load() {
  if (A.loading) return; A.loading = true;
  try { const [cfg, nodes] = await Promise.all([pp.GET('/automation/channels'), pp.GET('/remote/nodes')]); A.cfg = cfg; A.nodes = nodes.nodes || []; if (!A.selected) { A.count = cfg.channelCount; adopt((cfg.channels || [])[0]?.id || ''); } A.last = Date.now(); }
  catch(e) { A.last = Date.now(); A.error = e.message; }
  finally { A.loading = false; pp.render(); }
}
export function viewAutomationChannels() {
  if (!A.loading && Date.now() - A.last > 5000) load();
  if (!A.cfg) return `<div class="panel"><h3>AUTOMATION CHANNELS</h3><div class="body">${E(A.error || 'Loading…')}</div></div>`;
  const channels = A.cfg.channels || [], d = A.draft, p = channels.find(c => c.id === A.selected), status = p?.slaveStatus || {};
  return `<div class="panel"><h3>AUTOMATION CHANNELS / SLAVE NODES</h3><div class="body">
    <div class="row"><label>Automation channels <select onchange="pp.auCount(this.value)">${Array.from({length:8},(_,i)=>`<option ${A.count===i+1?'selected':''}>${i+1}</option>`).join('')}</select></label><button onclick="pp.auSaveCount()">Apply count</button><span>Reducing the count stops those automation slots and keeps their schedules.</span></div>
    <div class="row" style="margin-top:12px;flex-wrap:wrap"><label>Channel <select onchange="pp.auSelect(this.value)">${channels.map(c=>`<option value="${E(c.id)}" ${c.id===A.selected?'selected':''}>${E(c.name)} [${E(c.id)}]</option>`).join('')}</select></label>
    <label>Mode <select onchange="pp.auField('mode',this.value)">${[['local','Local automation'],['relay','Slave: receive remote video + audio'],['mirror','Slave: mirror schedule, play local media']].map(([v,l])=>`<option value="${v}" ${d.mode===v?'selected':''}>${l}</option>`).join('')}</select></label>
    ${d.mode!=='local'?`<label>Upstream node <select onchange="pp.auField('nodeId',this.value)"><option value="">Choose node…</option>${A.nodes.map(n=>`<option value="${E(n.id)}" ${d.nodeId===n.id?'selected':''}>${E(n.name || n.id)}</option>`).join('')}</select></label><label>Remote channel ID <input value="${E(d.channelId)}" onchange="pp.auField('channelId',this.value)"></label>${d.mode==='relay'?`<label>Receiver URL <input style="width:270px" placeholder="ndi://HOST (Program) or srt://…" value="${E(d.inputUrl)}" onchange="pp.auField('inputUrl',this.value)"></label>`:''}`:''}
    <button class="primary" ${A.selected?'':'disabled'} onclick="pp.auSave()">Save channel mode</button></div>
    <p style="color:${status.error?'var(--danger)':'var(--t2)'}">${E(status.error || (d.mode==='local'?'Local schedule automation':status.lastSyncMs?'Last sync: '+new Date(status.lastSyncMs).toLocaleTimeString():'Awaiting upstream configuration'))}</p>
    <p style="color:var(--t3)">Relay copies the upstream program, including graphics and audio. A blank receiver uses its NDI output. Schedule mirroring follows the active and next hours with matching local media; missing files are reported. Returning to local mode restores the original local hours.</p>
    </div></div>`;
}
export const ppExtAutomation = {
  auSelect(id) { adopt(id); pp.render(); }, auField(k,v) { A.draft[k]=v; pp.render(); }, auCount(v) { A.count=Number(v); },
  async auSaveCount() { try { A.cfg=await pp.PUT('/automation/channels',{count:A.count}); pp.toast('Automation channel count saved','ok'); pp.render(); } catch(e) { pp.toast(e.message,'err'); } },
  async auSave() { try { A.cfg=await pp.PUT('/automation/channels/'+encodeURIComponent(A.selected),A.draft); pp.toast('Channel mode saved','ok'); pp.render(); } catch(e) { pp.toast(e.message,'err'); } }
};
