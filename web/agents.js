/* PLAY PLAY web dashboard — Agents page (docs/agents.md): the operator (this Supervisor's command table), StarAgent
   daemons, Star nodes and the LLM assistant behind one task runner with a persistent result log. All state lives in
   AG (window.AGS); every input writes into AG as it is typed. WS "agents" carries one changed task (upserted). */

const AG = {
  loaded: false, loading: false, agents: [], config: {}, cfg: null, cfgDirty: false, apiKey: "", commands: [], tasks: [], open: {},
  run: { agent: "station", command: "", args: {}, argsJson: "", prompt: "" }, busy: false, filter: { agent: "", state: "" },
};
window.AGS = AG;
import { modelPicker, keyPlaceholder, keyNote } from "./aimodels.js";
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const js = v => esc(JSON.stringify(String(v ?? "")));
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const STATE = { queued: "var(--warn)", running: "var(--accent)", done: "var(--ready)", failed: "var(--danger)", cancelled: "var(--t3)" };
const RISK = { read: "var(--ready)", action: "var(--standby)", exec: "var(--danger)" };
const KIND = { operator: "var(--accent)", staragent: "var(--standby)", starnode: "var(--accent2)", llm: "var(--cued)" };
const when = ms => ms ? new Date(ms).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit", second: "2-digit" }) : "—";
const dur = (a, b) => { if (!a) return "—"; const s = ((b || Date.now()) - a) / 1000; return s < 60 ? s.toFixed(1) + " s" : Math.floor(s / 60) + "m " + Math.round(s % 60) + "s"; };
const opts = (list, cur) => list.map(o => { const v = Array.isArray(o) ? o[0] : o, l = Array.isArray(o) ? o[1] : o; return `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(l)}</option>`; }).join("");
const pretty = v => { if (v === undefined || v === null) return ""; if (typeof v === "string") { try { return JSON.stringify(JSON.parse(v), null, 2); } catch { return v; } } return JSON.stringify(v, null, 2); };
const pre = (t, color) => `<pre class="mono" style="margin:0;max-height:36vh;overflow:auto;background:var(--bg1);border:1px solid var(--b0);border-radius:5px;padding:8px;font-size:11px;white-space:pre-wrap;color:${color || "var(--t0)"}">${esc(t)}</pre>`;
const enc = encodeURIComponent;

async function loadAgents() {
  if (AG.loading) return; AG.loading = true;
  try {
    const [a, c, t] = await Promise.all([pp.GET("/agents").catch(() => ({})), pp.GET("/agents/commands").catch(() => ({})), pp.GET(tasksPath()).catch(() => ({}))]);
    AG.agents = a.agents || []; AG.config = a.config || AG.config; if (!AG.cfgDirty) AG.cfg = JSON.parse(JSON.stringify(AG.config)); AG.commands = c.commands || []; AG.tasks = t.tasks || []; AG.loaded = true;
    if (!AG.agents.find(x => x.id === AG.run.agent) && AG.agents.length) AG.run.agent = AG.agents[0].id;
    if (!AG.run.command) { const cmds = commandsFor(agentById(AG.run.agent)); if (cmds.length) AG.run.command = cmds[0].name; }
  } finally { AG.loading = false; }
  pp.render();
}
function tasksPath() { const f = AG.filter; return "/agents/tasks?limit=100" + (f.agent ? "&agent=" + enc(f.agent) : "") + (f.state ? "&state=" + enc(f.state) : ""); }
const loadTasks = () => pp.GET(tasksPath()).then(t => { AG.tasks = t.tasks || []; pp.render(); }).catch(() => {});
const agentById = id => AG.agents.find(a => a.id === id) || null;
// The commands an agent runs, as [{name, risk, desc, params}]: the operator's command table, or the agent's own list.
function commandsFor(a) {
  if (!a) return [];
  if (a.kind === "operator") return AG.commands;
  const list = a.commands || a.actions || [];
  return list.map(c => typeof c === "string" ? { name: c, risk: c === "exec" ? "exec" : /restart|run|call/.test(c) ? "action" : "read", desc: "", params: null } : c);
}

export function onEnterAgents(v) { if (v === "agents") loadAgents(); }
export function onWsAgents(m) {
  if (m.type !== "agents") return false;
  const t = m.task; if (!t || !t.id) return true;
  const f = AG.filter; if ((f.agent && t.agent !== f.agent) || (f.state && t.state !== f.state)) { AG.tasks = AG.tasks.filter(x => x.id !== t.id); return true; }
  const i = AG.tasks.findIndex(x => x.id === t.id); if (i >= 0) AG.tasks[i] = t; else { AG.tasks.unshift(t); if (AG.tasks.length > 100) AG.tasks.pop(); }
  return true;
}

// ---------------------------------------------------------------------------------------------
export function viewAgents() {
  if (!AG.loaded) loadAgents();
  const on = AG.agents.filter(a => a.online !== false && a.available !== false).length;
  const running = AG.tasks.filter(t => t.state === "running").length, queued = AG.tasks.filter(t => t.state === "queued").length;
  const head = `<div class="panel"><h3>Agents <span class="right">${bdg(on + "/" + AG.agents.length + " available", on ? "var(--ready)" : "var(--t3)")}${running ? bdg(running + " running", "var(--accent)") : ""}${queued ? bdg(queued + " queued", "var(--warn)") : ""}
      ${AG.config.allowExec ? bdg("exec allowed", "var(--danger)") : ""}${AG.config.allowActions ? bdg("llm actions allowed", "var(--standby)") : ""}<button class="ghost small" onclick="pp.agentsReload()">&#8635;</button></span></h3>${viewAgentList()}</div>`;
  return `<div class="grid" style="gap:12px">${head}<div class="grid cols2" style="align-items:start">${viewRun()}<div class="grid" style="gap:12px">${viewConfig()}${viewCommands()}</div></div>${viewTasks()}</div>`;
}

function viewAgentList() {
  const rows = AG.agents.map(a => `<tr class="${a.id === AG.run.agent ? "selrow" : ""}" style="cursor:pointer" onclick="pp.agentsPick(${js(a.id)})"><td>${bdg(a.kind || "?", KIND[a.kind] || "var(--t2)")}</td><td><b>${esc(a.name || a.id)}</b><div class="mono" style="color:var(--t3);font-size:10px">${esc(a.id)}</div></td>
    <td>${a.online === false ? bdg("offline", "var(--danger)") : a.online ? bdg("online", "var(--ready)") : bdg("?", "var(--t3)")}${a.available === false ? " " + bdg("unavailable", "var(--t3)") : ""}</td>
    <td style="color:var(--t3);font-size:11px">${a.model ? "model " + esc(a.model) : ""}${(a.commands || a.actions) ? (a.commands || a.actions).length + " commands" : a.kind === "operator" ? AG.commands.length + " commands" : ""}</td></tr>`).join("");
  return `<div class="scroll" style="max-height:30vh"><table><thead><tr><th>Kind</th><th>Agent</th><th>State</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="4" class="empty">No agents.</td></tr>`}</tbody></table></div>`;
}

function viewRun() {
  const r = AG.run; const a = agentById(r.agent); const llm = a && a.kind === "llm";
  const cmds = commandsFor(a); const cmd = cmds.find(c => c.name === r.command) || null;
  const params = cmd && cmd.params && typeof cmd.params === "object" ? Object.entries(cmd.params) : [];
  const risk = llm ? (AG.config.allowActions ? "action" : "read") : (cmd && cmd.risk) || "read";
  let body;
  if (llm) body = `<label class="f" style="grid-column:1/-1">Prompt <textarea rows="5" placeholder="Ask about the station: what is on air, what expires this week, what failed today…" oninput="AGS.run.prompt=this.value">${esc(r.prompt)}</textarea></label>
    <div style="grid-column:1/-1;color:var(--t3);font-size:11px">Answered with a tool loop over the read commands${AG.config.allowActions ? " and the action commands (allowed in the config)" : ""}, at most 6 rounds.</div>`;
  else body = `<label class="f" style="grid-column:1/-1">Command <select onchange="AGS.run.command=this.value;AGS.run.args={};AGS.run.argsJson='';pp.render()">${opts([["", "(pick)"]].concat(cmds.map(c => [c.name, c.name + (c.risk && c.risk !== "read" ? " (" + c.risk + ")" : "")])), r.command)}</select></label>
    ${cmd && cmd.desc ? `<div style="grid-column:1/-1;color:var(--t2);font-size:11px">${esc(cmd.desc)}</div>` : ""}
    ${params.map(([k, d]) => `<label class="f">${esc(k)} <input value="${esc(r.args[k] ?? "")}" placeholder="${esc(typeof d === "string" ? d : JSON.stringify(d))}" title="${esc(typeof d === "string" ? d : JSON.stringify(d))}" oninput="AGS.run.args[${js(k)}]=this.value"></label>`).join("")}
    ${cmd && !params.length && a && a.kind !== "operator" ? `<label class="f" style="grid-column:1/-1">Args (JSON, optional) <textarea rows="3" class="mono" placeholder="{}" oninput="AGS.run.argsJson=this.value">${esc(r.argsJson)}</textarea></label>` : ""}
    ${risk === "exec" ? `<div class="alert error" style="grid-column:1/-1">&#9888; exec runs a shell command on ${esc(a ? a.name || a.id : "the agent")}${AG.config.allowExec ? "" : " — exec is off in the config, the task will be refused"}.</div>` : risk === "action" ? `<div class="alert warning" style="grid-column:1/-1">This command changes the station.</div>` : ""}`;
  return `<div class="panel"><h3>Run a task <span class="right">${bdg(risk, RISK[risk] || "var(--t2)")}</span></h3><div class="body grid cols2" style="gap:8px">
    <label class="f" style="grid-column:1/-1">Agent <select onchange="pp.agentsPick(this.value)">${opts(AG.agents.map(x => [x.id, (x.name || x.id) + " · " + x.kind]), r.agent)}</select></label>${body}
    <div class="row" style="grid-column:1/-1"><button class="${risk === "exec" ? "danger" : "primary"}" ${AG.busy || !a ? "disabled" : ""} onclick="pp.agentsRun()">${AG.busy ? "Queueing…" : llm ? "Ask" : "Run"}</button></div></div></div>`;
}

function viewCommands() {
  const rows = AG.commands.map(c => `<tr style="cursor:pointer" onclick="pp.agentsPickCommand(${js(c.name)})"><td class="mono">${esc(c.name)}</td><td>${bdg(c.risk || "read", RISK[c.risk] || "var(--t2)")}</td><td style="white-space:normal;color:var(--t2)">${esc(c.desc || "")}</td>
    <td class="mono" style="font-size:10px;color:var(--t3);white-space:normal">${c.params && typeof c.params === "object" ? Object.keys(c.params).map(esc).join(", ") : ""}</td></tr>`).join("");
  return `<div class="panel"><h3>Operator commands (${AG.commands.length})</h3><div class="scroll" style="max-height:34vh"><table><thead><tr><th>Command</th><th>Risk</th><th>Does</th><th>Params</th></tr></thead><tbody>${rows || `<tr><td colspan="4" class="empty">No commands.</td></tr>`}</tbody></table></div></div>`;
}

function viewConfig() {
  const c = AG.cfg || {}; const d = ";AGS.cfgDirty=true";
  return `<div class="panel"><h3>Agents config ${AG.cfgDirty ? bdg("unsaved", "var(--warn)") : ""}</h3><div class="body grid cols2" style="gap:8px">
    <label class="f">StarAgent exec <label style="text-transform:none;color:var(--danger)"><input type="checkbox" ${c.allowExec ? "checked" : ""} onchange="AGS.cfg.allowExec=this.checked${d};pp.render()"> allow exec tasks</label></label>
    <label class="f">LLM actions <label style="text-transform:none"><input type="checkbox" ${c.allowActions ? "checked" : ""} onchange="AGS.cfg.allowActions=this.checked${d};pp.render()"> let the assistant run action commands</label></label>
    ${modelPicker("agents", c.model ?? "", `AGS.cfg.model=this.value${d};pp.render()`, "Assistant model")}
    <label class="f">OpenRouter key (shared) <input type="password" autocomplete="new-password" value="${esc(AG.apiKey)}" placeholder="${esc(keyPlaceholder())}" oninput="AGS.apiKey=this.value${d}"><span style="font-size:10px;color:var(--t3);text-transform:none">${esc(keyNote())}</span></label>
    <div class="row" style="grid-column:1/-1"><button class="success small" ${AG.cfgDirty ? "" : "disabled"} onclick="pp.agentsCfgSave()">SAVE</button><button class="ghost small" ${AG.cfgDirty ? "" : "disabled"} onclick="AGS.cfgDirty=false;AGS.apiKey='';AGS.cfg=JSON.parse(JSON.stringify(AGS.config));pp.render()">revert</button></div></div></div>`;
}

function taskResult(t) {
  if (t.agentKind === "llm" || t.prompt) {
    const text = typeof t.result === "string" ? t.result : t.result && (t.result.text || t.result.answer || t.result.content) || "";
    const steps = (t.steps || []).map((s, i) => `<div style="border-left:2px solid var(--b2);padding:2px 8px;margin:4px 0"><span class="mono" style="color:var(--accent)">${i + 1}. ${esc(s.command || s.tool || s.name || "step")}</span> <span class="mono" style="color:var(--t3);font-size:10px">${esc(s.args ? JSON.stringify(s.args) : "")}</span>
      ${s.result !== undefined || s.error ? `<details><summary style="cursor:pointer;color:var(--t3);font-size:10px">${s.error ? "error" : "result"}</summary>${pre(s.error || pretty(s.result), s.error ? "var(--danger)" : "")}</details>` : ""}</div>`).join("");
    return (t.prompt ? `<div style="color:var(--t2);margin-bottom:6px"><b>Q:</b> ${esc(t.prompt)}</div>` : "") + (text ? `<div style="white-space:pre-wrap;line-height:1.5;margin-bottom:6px">${esc(text)}</div>` : "") + (steps ? `<div class="fh">TOOL STEPS (${t.steps.length})</div>${steps}` : "") + (!text && t.result && typeof t.result === "object" ? pre(pretty(t.result)) : "");
  }
  return t.result !== undefined && t.result !== null ? pre(pretty(t.result)) : "";
}
function viewTasks() {
  const f = AG.filter;
  const rows = AG.tasks.map(t => { const active = t.state === "queued" || t.state === "running"; const open = AG.open[t.id];
    const main = `<tr><td>${bdg(t.state || "?", STATE[t.state] || "var(--t3)")}</td><td>${bdg(t.risk || "read", RISK[t.risk] || "var(--t2)")}</td><td>${esc(t.agent)}<div style="color:var(--t3);font-size:10px">${esc(t.agentKind || "")}</div></td>
      <td class="mono">${esc(t.command || (t.prompt ? "prompt" : ""))}<div style="color:var(--t3);font-size:10px;max-width:320px;overflow:hidden;text-overflow:ellipsis">${esc(t.prompt ? t.prompt.slice(0, 80) : t.args && Object.keys(t.args).length ? JSON.stringify(t.args) : "")}</div></td>
      <td class="mono" style="color:var(--t3)">${when(t.createdMs)}</td><td class="mono">${dur(t.startedMs, t.finishedMs)}</td>
      <td style="max-width:320px">${t.error ? `<span style="color:var(--danger)" title="${esc(t.error)}">${esc(String(t.error).slice(0, 90))}</span>` : t.state === "done" ? `<span style="color:var(--t3)">${esc(summary(t))}</span>` : ""}</td>
      <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="AGS.open[${js(t.id)}]=!AGS.open[${js(t.id)}];pp.render()">${open ? "&#9662;" : "&#9656;"} result</button>${active ? `<button class="ghost small" onclick="pp.agentsCancel(${js(t.id)})">cancel</button>` : `<button class="ghost small" onclick="pp.agentsDelete(${js(t.id)})">&#10005;</button>`}</td></tr>`;
    return main + (open ? `<tr><td colspan="8" style="white-space:normal;max-width:none;background:var(--bg2)">${taskResult(t) || `<span style="color:var(--t3)">no result yet</span>`}</td></tr>` : ""); }).join("");
  return `<div class="panel"><h3>Tasks (${AG.tasks.length}) <span class="right"><select class="small" onchange="AGS.filter.agent=this.value;pp.agentsTasks()">${opts([["", "all agents"]].concat(AG.agents.map(a => [a.id, a.name || a.id])), f.agent)}</select>
      <select class="small" onchange="AGS.filter.state=this.value;pp.agentsTasks()">${opts([["", "all states"], "queued", "running", "done", "failed", "cancelled"], f.state)}</select><button class="ghost small" onclick="pp.agentsClear()">Clear finished</button></span></h3>
    <div class="scroll" style="max-height:60vh"><table><thead><tr><th>State</th><th>Risk</th><th>Agent</th><th>Task</th><th>Created</th><th>Took</th><th>Outcome</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="8" class="empty">No tasks yet.</td></tr>`}</tbody></table></div></div>`;
}
function summary(t) { const r = t.result; if (r === undefined || r === null) return "done"; if (typeof r === "string") return r.slice(0, 90); if (r.text) return String(r.text).slice(0, 90); if (Array.isArray(r)) return r.length + " item(s)"; const k = Object.keys(r); return k.slice(0, 5).join(", ") + (k.length > 5 ? "…" : ""); }

// ---------------------------------------------------------------------------------------------
export const ppExtAgents = {
  agentsReload() { loadAgents(); },
  agentsTasks() { loadTasks(); },
  agentsPick(id) { const a = agentById(id); AG.run.agent = id; AG.run.args = {}; AG.run.argsJson = ""; const cmds = commandsFor(a); AG.run.command = cmds.find(c => c.name === AG.run.command) ? AG.run.command : (cmds[0] ? cmds[0].name : ""); pp.render(); },
  agentsPickCommand(name) { const op = AG.agents.find(a => a.kind === "operator"); if (op) AG.run.agent = op.id; AG.run.command = name; AG.run.args = {}; AG.run.argsJson = ""; pp.render(); },
  agentsRun() {
    const r = AG.run; const a = agentById(r.agent); if (!a) { pp.toast("Pick an agent", "err"); return; }
    const body = { agent: a.id };
    let risk = "read", label;
    if (a.kind === "llm") { if (!r.prompt.trim()) { pp.toast("Type a prompt", "err"); return; } body.prompt = r.prompt.trim(); label = "prompt"; risk = AG.config.allowActions ? "action" : "read"; }
    else {
      const cmd = commandsFor(a).find(c => c.name === r.command); if (!r.command) { pp.toast("Pick a command", "err"); return; }
      let args = {}; for (const [k, v] of Object.entries(r.args)) if (v !== "") { const n = Number(v); args[k] = v === "true" ? true : v === "false" ? false : (v.trim() !== "" && !isNaN(n) && /^-?\d+(\.\d+)?$/.test(v.trim())) ? n : v; }
      if (r.argsJson.trim()) { try { args = Object.assign(args, JSON.parse(r.argsJson)); } catch { pp.toast("Args are not valid JSON", "err"); return; } }
      body.command = r.command; body.args = args; label = r.command; risk = (cmd && cmd.risk) || "read";
    }
    const go = () => { AG.busy = true; pp.render(); pp.POST("/agents/tasks", body).then(t => { const task = t && (t.task || t); if (task && task.id) { const i = AG.tasks.findIndex(x => x.id === task.id); if (i < 0) AG.tasks.unshift(task); AG.open[task.id] = true; } if (a.kind === "llm") r.prompt = ""; pp.toast("Queued " + label + " on " + (a.name || a.id), "ok"); })
      .catch(() => {}).finally(() => { AG.busy = false; pp.render(); }); };
    if (risk === "exec") pp.confirmDo("Run a shell command on " + (a.name || a.id) + "?\n\n" + JSON.stringify(body.args), go);
    else if (risk === "action") pp.confirmDo("Run the action " + label + " on " + (a.name || a.id) + "? It changes the station.", go);
    else go();
  },
  // The contract has no separate cancel route: DELETE /agents/tasks/{id} on a queued/running task cancels it.
  agentsCancel(id) { pp.confirmDo("Cancel task " + id + "?", () => pp.api("DELETE", "/agents/tasks/" + enc(id)).then(() => loadTasks()).catch(() => {})); },
  agentsDelete(id) { pp.confirmDo("Delete task " + id + " and its result from the log?", () => pp.api("DELETE", "/agents/tasks/" + enc(id)).then(() => { AG.tasks = AG.tasks.filter(t => t.id !== id); pp.render(); }).catch(() => {})); },
  agentsClear() { pp.confirmDo("Clear every finished (done, failed, cancelled) task from the log?", () => pp.POST("/agents/tasks/clear").then(() => loadTasks()).catch(() => {})); },
  agentsCfgSave() {
    const c = AG.cfg; const body = { allowExec: !!c.allowExec, allowActions: !!c.allowActions, model: c.model || "" }; if (AG.apiKey) body.apiKey = AG.apiKey;      // blank keeps the stored key
    const go = () => pp.PUT("/agents/config", body).then(r => { AG.cfgDirty = false; AG.apiKey = ""; if (r && typeof r === "object") { AG.config = r.config || r; AG.cfg = JSON.parse(JSON.stringify(AG.config)); } pp.toast("Agents config saved", "ok"); loadAgents(); }).catch(() => {});
    const turningOn = (c.allowExec && !AG.config.allowExec) || (c.allowActions && !AG.config.allowActions);
    if (turningOn) pp.confirmDo((c.allowExec && !AG.config.allowExec ? "Allow exec tasks on StarAgent daemons. " : "") + (c.allowActions && !AG.config.allowActions ? "Let the assistant run action commands (skip, arm, snap…). " : "") + "Continue?", go); else go();
  },
};
