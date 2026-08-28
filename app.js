/* razerpad shared front-end: unit preference, API helpers, chart components */
"use strict";

const $ = id => document.getElementById(id);
const cssVar = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

/* ── temperature unit (display-only preference, stored per-browser) ── */
const Unit = {
  get() {
    try { return localStorage.getItem("razerpad-unit") === "f" ? "f" : "c"; }
    catch (e) { return "c"; }
  },
  set(u) {
    try { localStorage.setItem("razerpad-unit", u); } catch (e) {}
    document.dispatchEvent(new CustomEvent("unitchange"));
  },
  toggle() { Unit.set(Unit.get() === "c" ? "f" : "c"); },
  disp(c) { return Unit.get() === "f" ? c * 9 / 5 + 32 : c; },
  fromDisp(v) { return Unit.get() === "f" ? (v - 32) * 5 / 9 : v; },
  label() { return Unit.get() === "f" ? "°F" : "°C"; },
};

/* ── API ── */
async function apiStatus() {
  const r = await fetch("/api/status");
  return r.json();
}
async function apiConfig(body) {
  const r = await fetch("/api/config", { method: "POST", body: JSON.stringify(body) });
  return r.json();
}
function flash(el, ok, text) {
  el.className = "msg " + (ok ? "ok" : "err");
  el.textContent = text;
  clearTimeout(el._t);
  el._t = setTimeout(() => { el.textContent = ""; }, 4000);
}
function parseCurveStr(s) {
  return (s || "").split(",").map(p => p.split(":").map(Number))
    .filter(p => p.length === 2 && !p.some(isNaN));
}

/* ── single-series line chart with crosshair tooltip ── */
function lineChart(canvas, tipEl, colorVar, opts) {
  const ctx = canvas.getContext("2d");
  let pts = [];   // [{x, y, label}]
  let px = [];
  const pad = { l: 38, r: 8, t: 8, b: 16 };

  function draw(hoverI = -1) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    canvas.width = w * dpr; canvas.height = h * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    if (pts.length < 2) return;

    let ymin = opts.ymin, ymax = opts.ymax;
    if (ymin === undefined) {
      const vals = pts.map(p => p.y);
      ymin = Math.min(...vals); ymax = Math.max(...vals);
      const m = Math.max((ymax - ymin) * 0.15, 1); ymin -= m; ymax += m;
    }
    const xmin = pts[0].x, xmax = pts[pts.length - 1].x || 1;
    const X = x => pad.l + (x - xmin) / (xmax - xmin || 1) * (w - pad.l - pad.r);
    const Y = y => pad.t + (1 - (y - ymin) / (ymax - ymin || 1)) * (h - pad.t - pad.b);

    ctx.strokeStyle = cssVar("--grid"); ctx.lineWidth = 1;
    ctx.fillStyle = cssVar("--muted");
    ctx.font = "10px system-ui"; ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (let i = 0; i <= 3; i++) {
      const v = ymin + (ymax - ymin) * i / 3, y = Y(v);
      ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
      ctx.fillText(opts.fmt(v), pad.l - 5, y);
    }

    const color = cssVar(colorVar);
    ctx.strokeStyle = color; ctx.lineWidth = 2;
    ctx.lineJoin = "round"; ctx.lineCap = "round";
    ctx.beginPath();
    px = pts.map(p => [X(p.x), Y(p.y)]);
    px.forEach(([x, y], i) => i ? ctx.lineTo(x, y) : ctx.moveTo(x, y));
    ctx.stroke();

    if (hoverI >= 0 && px[hoverI]) {
      const [hx, hy] = px[hoverI];
      ctx.strokeStyle = cssVar("--axis");
      ctx.beginPath(); ctx.moveTo(hx, pad.t); ctx.lineTo(hx, h - pad.b); ctx.stroke();
      ctx.fillStyle = color;
      ctx.beginPath(); ctx.arc(hx, hy, 4, 0, 7); ctx.fill();
      ctx.strokeStyle = cssVar("--panel"); ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(hx, hy, 4, 0, 7); ctx.stroke();
    }
  }

  canvas.addEventListener("mousemove", e => {
    if (!px.length) return;
    const r = canvas.getBoundingClientRect();
    const mx = e.clientX - r.left;
    let best = 0, bd = 1e9;
    px.forEach(([x], i) => { const d = Math.abs(x - mx); if (d < bd) { bd = d; best = i; } });
    draw(best);
    tipEl.style.display = "block";
    tipEl.textContent = pts[best].label;
    tipEl.style.left = Math.min(Math.max(px[best][0] + 10, 0), r.width - 130) + "px";
    tipEl.style.top = Math.max(px[best][1] - 30, 0) + "px";
  });
  canvas.addEventListener("mouseleave", () => { tipEl.style.display = "none"; draw(); });

  return { set(p) { pts = p; draw(); }, redraw: () => draw() };
}

/* ── fan-curve chart: percent vs temperature (curve stored in °C) ── */
function curveChart(canvas) {
  const ctx = canvas.getContext("2d");
  let ptsC = [];        // [[tempC, pct], ...]
  let status = null;

  function draw() {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    canvas.width = w * dpr; canvas.height = h * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const pad = { l: 40, r: 12, t: 12, b: 26 };
    const tmin = 30, tmax = 105; /* °C axis span */
    const X = t => pad.l + (t - tmin) / (tmax - tmin) * (w - pad.l - pad.r);
    const Y = p => pad.t + (1 - p / 100) * (h - pad.t - pad.b);

    ctx.strokeStyle = cssVar("--grid"); ctx.lineWidth = 1;
    ctx.fillStyle = cssVar("--muted"); ctx.font = "10px system-ui";
    ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (let p = 0; p <= 100; p += 25) {
      ctx.beginPath(); ctx.moveTo(pad.l, Y(p)); ctx.lineTo(w - pad.r, Y(p)); ctx.stroke();
      ctx.fillText(p + "%", pad.l - 6, Y(p));
    }
    ctx.textAlign = "center"; ctx.textBaseline = "top";
    for (let t = 40; t <= 100; t += 20)
      ctx.fillText(Math.round(Unit.disp(t)) + Unit.label(), X(t), h - pad.b + 8);

    const pts = [...ptsC].sort((a, b) => a[0] - b[0]);
    if (pts.length >= 2) {
      const color = cssVar("--fan");
      ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.lineJoin = "round";
      ctx.beginPath();
      ctx.moveTo(X(tmin), Y(pts[0][1]));
      pts.forEach(([t, p]) => ctx.lineTo(X(Math.min(Math.max(t, tmin), tmax)), Y(p)));
      ctx.lineTo(X(tmax), Y(pts[pts.length - 1][1]));
      ctx.stroke();
      pts.forEach(([t, p]) => {
        ctx.fillStyle = color;
        ctx.beginPath(); ctx.arc(X(t), Y(p), 4, 0, 7); ctx.fill();
        ctx.strokeStyle = cssVar("--panel"); ctx.lineWidth = 2; ctx.stroke();
      });
    }
    if (status && status.connected && status.temp > 0) {
      const p = Math.max((status.rpm - 500) / 2700 * 100, 0);
      ctx.fillStyle = cssVar("--temp");
      ctx.beginPath(); ctx.arc(X(status.temp), Y(p), 5, 0, 7); ctx.fill();
      ctx.strokeStyle = cssVar("--panel"); ctx.lineWidth = 2; ctx.stroke();
      ctx.fillStyle = cssVar("--ink-2"); ctx.textAlign = "left"; ctx.textBaseline = "middle";
      ctx.fillText("now", X(status.temp) + 10, Y(p));
    }
  }

  return {
    set(p, st) { ptsC = p; if (st !== undefined) status = st; draw(); },
    redraw: () => draw(),
  };
}

/* ── nav connection pill ── */
function renderConn(status) {
  const conn = $("conn");
  if (!conn) return;
  let cls = "conn", text;
  if (!status) text = "server unreachable";
  else if (status.stale) text = "daemon not running";
  else if (!status.connected) text = "pad unplugged";
  else { cls = "conn ok"; text = "connected"; }
  conn.className = cls;
  $("connText").textContent = text;
}
