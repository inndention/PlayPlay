/* PLAY PLAY web dashboard — shared OpenRouter model picker (GET /api/v1/ai/models). A searchable input backed by a
   <datalist>: type part of a name or id; each option shows the price per million tokens and the context size. Models
   older than ~12 months are hidden unless "show older" is ticked; legacy families (gpt-4o, gpt-4, gpt-3.5, claude-3/2,
   gemini-1.x/2.0) are never offered. State lives in window.AIM so WS repaints keep it. */

const AIM = { loaded: false, loading: false, all: false, models: [], source: "", hasKey: false, keySource: "none", defaultModel: "anthropic/claude-sonnet-5.5" };
window.AIM = AIM;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const price = v => v === 0 ? "free" : "$" + (v >= 10 ? v.toFixed(0) : v >= 1 ? v.toFixed(2).replace(/\.?0+$/, "") : v.toFixed(3).replace(/\.?0+$/, ""));
const ctx = n => !n ? "" : n >= 1e6 ? (n / 1e6).toFixed(n % 1e6 ? 1 : 0) + "M" : Math.round(n / 1000) + "k";

export function loadAiModels(refresh = false) {
  if (AIM.loading) return Promise.resolve(AIM);
  AIM.loading = true;
  return pp.GET("/ai/models" + (AIM.all ? "?all=1" : "?all=0") + (refresh ? "&refresh=1" : "")).then(r => {
    AIM.models = r.models || []; AIM.source = r.source || ""; AIM.hasKey = !!r.hasKey; AIM.keySource = r.keySource || "none"; AIM.defaultModel = r.defaultModel || AIM.defaultModel; AIM.loaded = true;
  }).catch(() => {}).finally(() => { AIM.loading = false; pp.render(); });
}

export const modelLabel = m => `${m.name || m.id} · ${price(m.promptPerMtok)} in / ${price(m.completionPerMtok)} out per Mtok${m.context ? " · " + ctx(m.context) + " ctx" : ""}`;

/** Searchable model field. `onPick` is JS run with `this.value` = the chosen id; `key` makes the datalist id unique. */
export function modelPicker(key, current, onPick, label = "Model") {
  if (!AIM.loaded && !AIM.loading) loadAiModels();
  const cur = AIM.models.find(m => m.id === current);
  const id = "aim-list-" + key;
  return `<label class="f">${esc(label)} <input list="${id}" value="${esc(current || "")}" placeholder="${esc(AIM.defaultModel)} (default) · type to search" onchange="${onPick}">
    <datalist id="${id}">${AIM.models.map(m => `<option value="${esc(m.id)}" label="${esc(modelLabel(m))}">${esc(modelLabel(m))}</option>`).join("")}</datalist>
    <span style="font-size:10px;color:var(--t3);text-transform:none">${cur ? esc(modelLabel(cur)) : current ? "not in the current catalog" : "default: " + esc(AIM.defaultModel)} · ${AIM.models.length} models (${esc(AIM.source || "…")})
      <label style="display:inline;text-transform:none"><input type="checkbox" ${AIM.all ? "checked" : ""} onchange="AIM.all=this.checked;pp.aiModelsReload()"> show older</label>
      <a href="#" onclick="event.preventDefault();pp.aiModelsReload(true)">refresh</a></span></label>`;
}

/** Placeholder text for a key field: tells the operator the key already comes from the environment. */
export function keyPlaceholder() {
  return AIM.keySource === "env" ? "from environment (OPENROUTER_API_KEY) · type to override" : AIM.keySource === "settings" ? "•••••• saved override (blank keeps it)" : "sk-or-… (or set OPENROUTER_API_KEY)";
}
export const keyNote = () => AIM.keySource === "env" ? "key: from environment (OPENROUTER_API_KEY)" : AIM.keySource === "settings" ? "key: saved override" : "no key: set OPENROUTER_API_KEY";

export const ppExtAiModels = { aiModelsReload(refresh = false) { return loadAiModels(refresh); } };
