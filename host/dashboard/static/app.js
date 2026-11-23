// LintasGuard dashboard: draws what lg-bridge reports. Read-only: nothing is ever sent back.
"use strict";

const cv = document.getElementById("track");
const ctx = cv.getContext("2d");
let geo = { strike_in_m: 400, island_m: 12, sensor_zone_m: 2 };
let state = null;
let link = { connected: false, age_s: null };
const SENSORS = ["west/a", "west/b", "west/island", "east/a", "east/b", "east/island"];

fetch("/config.json").then((r) => r.json()).then((g) => { if (g && g.strike_in_m) geo = g; }).catch(() => {});

function fmtMs(ms) {
  if (ms === undefined || ms === null) return "-";
  return (ms / 1000).toFixed(1) + " s";
}

function setText(id, text) { document.getElementById(id).textContent = text; }

function updateHeader() {
  const lv = document.getElementById("level");
  const l = state ? state.level : 4;
  lv.className = "level l" + Math.min(4, Math.max(0, l));
  lv.textContent = state ? "level " + l : "level ?";
  const ln = document.getElementById("link");
  const stale = state && state.stale;
  if (!link.connected) { ln.className = "link down"; ln.textContent = "bridge: no link"; }
  else if (stale) { ln.className = "link stale"; ln.textContent = "bridge: stale data"; }
  else { ln.className = "link up"; ln.textContent = "bridge: live (" + link.target + ")"; }
  if (!state) return;
  setText("gate", state.gate + " (" + state.gate_position + "°)");
  setText("lights", state.lights ? "ON, bell " + (state.bell ? "on" : "off") : "off");
  const act = [];
  if (state.active && state.active[0]) act.push("west");
  if (state.active && state.active[1]) act.push("east");
  setText("active", act.length ? act.join(", ") : "none");
  setText("plant", fmtMs(state.plant_ms));
  setText("tickets", String(state.tickets));
  setText("stalls", String(state.stalls));
}

// "t" on every bridge line is the bridge clock (target CLOCK_MONOTONIC, seconds since the VM booted).
// Shown as plant (scenario) time, worked out from the latest state line, which carries both clocks.
function eventTime(ev) {
  if (typeof ev.t !== "number") return "";
  if (state && typeof state.t === "number" && typeof state.plant_ms === "number") {
    const s = state.plant_ms / 1000 + (ev.t - state.t);
    if (s >= 0) return "plant " + s.toFixed(1) + " s";
  }
  return "uptime " + ev.t.toFixed(1) + " s";
}

function addInbox(ev) {
  const ul = document.getElementById("inbox");
  const li = document.createElement("li");
  const t = document.createElement("span");
  t.className = "t";
  t.textContent = eventTime(ev);
  li.appendChild(t);
  if (ev.kind === "level") {
    li.className = "level";
    li.appendChild(document.createTextNode("Level " + ev.from + " → " + ev.to + " (" + ev.reason + ")"));
  } else {
    li.className = ev.severity || "info";
    li.appendChild(document.createTextNode("[" + (ev.to || "maintenance") + "] " + (ev.text || ev.diag)));
  }
  ul.insertBefore(li, ul.firstChild);
  while (ul.children.length > 30) ul.removeChild(ul.lastChild);
}

function setMetric(name, value) {
  if (name === "warning_time_ms") setText("m-warning", fmtMs(value));
  else if (name === "closure_ms_per_hour") setText("m-closure", value ? (value / 60000).toFixed(1) + " min/h" : "-");
  else if (name === "trains") setText("m-trains", String(value));
}

function handle(msg) {
  if (msg.kind === "hello") {
    link = msg.link || link;
    if (msg.state) state = msg.state;
    (msg.history || []).forEach(addInbox);
    Object.entries(msg.metrics || {}).forEach(([k, v]) => setMetric(k, v));
  } else if (msg.kind === "link") {
    link = msg;
  } else if (msg.kind === "state") {
    state = msg;
    link.connected = true;
  } else if (msg.kind === "level" || msg.kind === "notify") {
    addInbox(msg);
  } else if (msg.kind === "metric") {
    setMetric(msg.name, msg.value);
  }
  updateHeader();
}

function connect() {
  const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
  ws.onmessage = (e) => { try { handle(JSON.parse(e.data)); } catch (err) { /* ignore a bad line */ } };
  ws.onclose = () => { link.connected = false; updateHeader(); setTimeout(connect, 1000); };
}

// ---- drawing ----
function draw() {
  const W = cv.width, H = cv.height, y = 180;
  const S = geo.strike_in_m, I = geo.island_m, Z = geo.sensor_zone_m;
  const scale = (W - 80) / (2 * S + I);
  const x0 = W / 2;
  const westStrike = x0 - (S + I / 2) * scale;
  const eastStrike = x0 + (S + I / 2) * scale;
  const failed = state ? state.sensor_failed : 0;
  const blink = Math.floor(Date.now() / 500) % 2 === 0;

  ctx.clearRect(0, 0, W, H);
  // road across the track
  ctx.fillStyle = "#c9ccd3";
  ctx.fillRect(x0 - 26, 0, 52, H);
  // track
  ctx.strokeStyle = "#4a4f5a";
  ctx.lineWidth = 3;
  ctx.beginPath(); ctx.moveTo(20, y - 6); ctx.lineTo(W - 20, y - 6); ctx.moveTo(20, y + 6); ctx.lineTo(W - 20, y + 6); ctx.stroke();
  // island
  ctx.fillStyle = state && state.island ? "rgba(208,52,44,0.35)" : "rgba(29,35,48,0.12)";
  ctx.fillRect(x0 - (I / 2) * scale - 4, y - 16, I * scale + 8, 32);
  // sensors: A and B at each strike-in point, island sensors on the island
  const sensorAt = (x, idx, label, dy) => {
    const bad = (failed >> idx) & 1;
    ctx.fillStyle = bad ? "#d0342c" : "#2e9e4f";
    ctx.fillRect(x - 5, y + dy - 5, 10, 10);
    ctx.fillStyle = "#1d2330";
    ctx.font = "11px system-ui";
    ctx.fillText(label, x - 10, y + dy + (dy < 0 ? -9 : 18));
  };
  sensorAt(westStrike + (Z / 2) * scale, 0, "W-A", -26);
  sensorAt(westStrike + (Z / 2) * scale, 1, "W-B", 26);
  sensorAt(x0 - 10, 2, "W-I", 42);
  sensorAt(eastStrike - (Z / 2) * scale, 3, "E-A", -26);
  sensorAt(eastStrike - (Z / 2) * scale, 4, "E-B", 26);
  sensorAt(x0 + 10, 5, "E-I", 42);
  // trains (path coordinate: 0 at the strike-in point of their side)
  (state ? state.trains : []).forEach((t) => {
    const f = t.front_mm / 1000, r = t.rear_mm / 1000;
    let x1, x2;
    if (t.dir === "west") { x1 = westStrike + r * scale; x2 = westStrike + f * scale; }
    else { x1 = eastStrike - f * scale; x2 = eastStrike - r * scale; }
    ctx.fillStyle = t.dir === "west" ? "#2f6fd6" : "#7b3fc4";
    ctx.fillRect(Math.max(20, x1), y - 9, Math.max(2, Math.min(W - 20, x2) - Math.max(20, x1)), 18);
    ctx.fillStyle = "#fff";
    ctx.font = "bold 11px system-ui";
    const mid = (Math.max(20, x1) + Math.min(W - 20, x2)) / 2;
    if (mid > 20 && mid < W - 20) ctx.fillText("T" + t.id + (t.dir === "west" ? " →" : " ←"), mid - 14, y + 4);
  });
  // gate arms: 0 deg = up (vertical), 90 deg = down (across the road)
  const pos = state ? state.gate_position : 90;
  const ang = (Math.PI / 2) * (pos / 90);
  const arm = (px, py, dir) => {
    ctx.strokeStyle = "#d0342c";
    ctx.lineWidth = 6;
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(px + dir * Math.sin(ang) * 70, py - Math.cos(ang) * 70);
    ctx.stroke();
    ctx.fillStyle = "#1d2330";
    ctx.beginPath(); ctx.arc(px, py, 6, 0, 2 * Math.PI); ctx.fill();
  };
  arm(x0 - 40, y - 40, 1);
  arm(x0 + 40, y + 40, -1);
  // lights
  const lightsOn = state && state.lights;
  [[x0 - 60, y - 70], [x0 + 60, y + 70]].forEach(([lx, ly], i) => {
    ctx.fillStyle = lightsOn && (blink ? i === 0 : i === 1) ? "#ff3b30" : "#5a2a2a";
    ctx.beginPath(); ctx.arc(lx, ly, 9, 0, 2 * Math.PI); ctx.fill();
  });
  if (state && state.bell) { ctx.fillStyle = "#1d2330"; ctx.font = "bold 12px system-ui"; ctx.fillText("BELL", x0 - 80, y - 90); }
  // labels
  ctx.fillStyle = "#1d2330";
  ctx.font = "12px system-ui";
  ctx.fillText("west strike-in", westStrike - 10, 24);
  ctx.fillText("east strike-in", eastStrike - 70, 24);
  ctx.fillText("island", x0 - 16, y + 75);
  if (!link.connected || (state && state.stale)) {
    ctx.fillStyle = "rgba(208,52,44,0.85)";
    ctx.fillRect(W / 2 - 150, 40, 300, 30);
    ctx.fillStyle = "#fff";
    ctx.font = "bold 14px system-ui";
    ctx.fillText(link.connected ? "STALE DATA FROM THE TARGET" : "NO LINK TO lg-bridge", W / 2 - 110, 60);
  }
  requestAnimationFrame(draw);
}

connect();
requestAnimationFrame(draw);
