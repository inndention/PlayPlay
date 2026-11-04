// System status footer: per-channel frame rate and output bitrate, CPU / memory, network in / out.
// One poll a second and a half of GET /api/v1/system/stats (the Supervisor samples engine telemetry and the Windows counters once a second),
// the same numbers the native app shows in its bottom status bar. Self-contained: injects its own style and footer, so it needs only a script tag.
(() => {
  const css = `
  body { padding-bottom: 28px }
  #sysbar { position: fixed; left: 0; right: 0; bottom: 0; height: 28px; z-index: 50; display: flex; align-items: center; gap: 10px; padding: 0 12px; background: var(--bg1, #0a0a12); border-top: 1px solid var(--b1, #2a2a40); font: 11px/1 var(--mono, Consolas, monospace); color: var(--t0, #f0f0f8); white-space: nowrap; overflow: hidden }
  #sysbar .k { color: var(--t3, #505068); font: 700 10px var(--sans, sans-serif); letter-spacing: 1px }
  #sysbar .sep { width: 1px; height: 14px; background: var(--b1, #2a2a40) }
  #sysbar .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--t3, #505068); display: inline-block }
  #sysbar .gap { flex: 1 }
  #sysbar .mu { color: var(--t2, #707088) }
  #sysbar .warn { color: var(--warn, #ffd60a) } #sysbar .bad { color: var(--danger, #ff453a) } #sysbar .ok { color: var(--ready, #30d158) } #sysbar .acc { color: var(--accent, #00d4ff) }
  #sysbar .m { display: inline-block; width: 34px; height: 6px; border-radius: 3px; background: var(--bg4, #1a1a28); vertical-align: middle; overflow: hidden } #sysbar .m i { display: block; height: 100%; background: var(--accent, #00d4ff) }
  #sysbar .ch { display: inline-flex; align-items: center; gap: 6px }`;
  const st = document.createElement("style"); st.textContent = css; document.head.appendChild(st);
  const bar = document.createElement("footer"); bar.id = "sysbar"; document.body.appendChild(bar);

  const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const bits = bps => { const b = Math.max(0, bps * 8); return b >= 1e9 ? (b / 1e9).toFixed(2) + " Gb/s" : b >= 1e6 ? (b / 1e6).toFixed(b >= 1e8 ? 0 : 1) + " Mb/s" : b >= 1e3 ? (b / 1e3).toFixed(0) + " kb/s" : b.toFixed(0) + " b/s"; };
  const kbps = k => !(k > 0) ? "—" : k >= 1000 ? (k / 1000).toFixed(k >= 10000 ? 1 : 2) + " Mb/s" : Math.round(k) + " kb/s";
  const gb = mb => (mb / 1024).toFixed(1);
  const lvl = p => p >= 90 ? "bad" : p >= 75 ? "warn" : "";
  const stateColor = s => s === "ON_AIR" ? "var(--onair,#ff2d55)" : s === "PAUSED" ? "var(--warn,#ffd60a)" : s === "HOLD" || s === "IDLE" ? "var(--standby,#ff9500)" : s === "OFFLINE" || s === "ERROR" ? "var(--danger,#ff453a)" : "var(--t2,#707088)";
  let fails = 0;

  function render(d) {
    const cpu = d.cpu || {}, mem = d.memory || {}, net = d.net || {};
    const chans = (d.channels || []).map(c => {
      if (c.stale) return `<span class="ch"><span class="dot"></span><span class="k">${E(String(c.id).toUpperCase())}</span><span class="mu">offline</span></span><span class="sep"></span>`;
      const slow = c.nominalFps > 0 && c.fps < c.nominalFps * 0.97;
      const tip = `${c.width}x${c.height} @ ${c.nominalFps} nominal · tick ${(c.tickMs || 0).toFixed(1)} ms (max ${(c.tickMsMax || 0).toFixed(0)}) · late ${c.lateTicks || 0}` + (c.cpu !== undefined ? ` · engine ${c.cpu}% CPU ${c.memMB} MB` : "") + (c.outputs || []).map(o => `\n${o.name} (${o.kind}) ${kbps(o.bitrateKbps)}${o.dropped ? " drops " + o.dropped : ""}${o.connected === false ? " DOWN" : ""}`).join("");
      return `<span class="ch" title="${E(tip)}"><span class="dot" style="background:${stateColor(c.state)}"></span><span class="k">${E(String(c.id).toUpperCase())}</span><span class="${slow ? "warn" : ""}">${c.fps.toFixed(2)} fps</span><span class="k">OUT</span><span>${kbps(c.bitrateKbps)}</span>`
        + (c.outputCount > 0 && c.outputsUp < c.outputCount ? `<span class="warn">▲${c.outputsUp}/${c.outputCount}</span>` : "") + (c.drops > 0 ? `<span class="bad">DROP ${Math.round(c.drops)}</span>` : "") + `</span><span class="sep"></span>`;
    }).join("");
    bar.innerHTML = `<span class="dot" style="background:var(--ready,#30d158)"></span><span class="k">SUPERVISOR</span><span class="sep"></span>${chans}<span class="gap"></span>`
      + `<span class="k">CPU</span><span class="${lvl(cpu.systemPct || 0)}">${Math.round(cpu.systemPct || 0)}%</span><span class="m"><i style="width:${Math.min(100, cpu.systemPct || 0)}%"></i></span><span class="mu">SUP ${(cpu.supervisorPct ?? 0).toFixed(1)} ENG ${(cpu.enginesPct ?? 0).toFixed(1)} %</span><span class="sep"></span>`
      + `<span class="k">MEM</span><span class="${lvl(mem.usedPct || 0)}">${mem.totalMB ? gb(mem.usedMB) + " / " + gb(mem.totalMB) + " GB" : "—"}</span><span class="m"><i style="width:${Math.min(100, mem.usedPct || 0)}%;background:var(--accent2,#a855f7)"></i></span><span class="mu">SUP ${Math.round(mem.supervisorMB || 0)} ENG ${Math.round(mem.enginesMB || 0)} MB</span><span class="sep"></span>`
      + `<span class="k">NET</span><span class="ok">↓ ${bits(net.inBps || 0)}</span><span class="acc">↑ ${bits(net.outBps || 0)}</span>`;
  }

  async function poll() {
    try {
      const token = localStorage.getItem("pp.token") || "";
      const r = await fetch("/api/v1/system/stats", { headers: token ? { Authorization: "Bearer " + token } : {} });
      if (!r.ok) throw new Error(r.status);
      render(await r.json()); fails = 0;
    } catch {
      if (++fails >= 2) bar.innerHTML = `<span class="dot" style="background:var(--danger,#ff453a)"></span><span class="k">SUPERVISOR</span><span class="mu">no system stats</span>`;
    }
  }
  poll(); setInterval(poll, 1500);
})();
