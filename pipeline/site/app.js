// HaloAI demo site (B6): the badge's states and buttons, driven by the same API the phone uses.
// Each visitor gets their own sample world (POST /v1/demo/session). Nothing real is created or sent.
import { draw, size } from "./badge.js";

const CONFIG = window.HALO_CONFIG || {};
const TZ = Intl.DateTimeFormat().resolvedOptions().timeZone || "America/Chicago";
const DECISION = ["review", "conflict_ambiguous", "conflict_schedule"];
const OUTCOME = ["success", "failure", "execution_uncertain", "read_only_result", "cancelled"];
const CONFIRM_GUARD_MS = 1000; // the badge ignores a press for about 1 s after REVIEW draws
// The badge can be landscape or portrait (a phone setting). Narrow screens get portrait, so the
// e-ink text stays readable; the backend wraps screen_lines to that orientation's budget.
const NARROW = matchMedia("(max-width: 600px)");
const orientationNow = () => (NARROW.matches ? "portrait" : "landscape");

// A student's week (the demo's sample world, app/mockdata.py DEMO_EVENTS and DEMO_CONTACTS).
const EXAMPLES = [
  ["Calendar", ["block 7 to 8 am tomorrow for the gym", "what do I have tomorrow?", "move the group project meeting to friday at 4",
                "book a study session tomorrow at 10 with Carlos"]],
  ["Email", ["email the TA saying I'll be late to lab", "email Professor Patel asking for an extension on homework 4",
             "email Alex about the group project slides", "email judge at example dot com saying thanks for trying HaloAI"]],
  ["Search, then act", ["what time does the UTRGV library in Edinburg close on Saturday?",
                        "block a study session at the UTRGV library this Saturday from 2 until it closes"]],
  ["Memory", ["remember that I prefer to study in the mornings", "what do you remember about me?"]],
  ["Routines (reusable skills)", ["save a routine called gym: block 7 to 8 am for the gym", "do my gym routine on friday",
                                  "save a routine called late: email the TA saying I'll be late to lab today", "do my late routine"]],
];

const $ = (id) => document.getElementById(id);
const S = {
  token: null, rid: null, result: null, revision: 0, phase: "ready", status: "", statuses: [],
  highlight: 0, adding: false, guardUntil: 0, expiresAt: null, busy: false, lastLines: null,
  orientation: orientationNow(),
};

// ---------- small helpers ----------

const uuid = () => crypto.randomUUID();
function isoNow() { // local time with its UTC offset, as the phone sends it
  const d = new Date(), off = -d.getTimezoneOffset(), sign = off >= 0 ? "+" : "-", a = Math.abs(off);
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}${sign}${p(Math.floor(a / 60))}:${p(a % 60)}`;
}
function store(key, value) { try { value == null ? localStorage.removeItem(key) : localStorage.setItem(key, JSON.stringify(value)); } catch { /* private mode */ } }
function load(key) { try { return JSON.parse(localStorage.getItem(key)); } catch { return null; } }
function el(tag, attrs = {}, ...children) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) k === "class" ? (n.className = v) : n.setAttribute(k, v);
  for (const c of children) n.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return n;
}

// ---------- API ----------

async function session(force = false) {
  const saved = load("halo.visitor");
  if (!force && saved && new Date(saved.expires_at) > new Date(Date.now() + 60000)) return (S.token = saved.token);
  if (!CONFIG.demoKey) throw new Error("This demo is not configured yet.");
  const r = await fetch("/v1/demo/session", { method: "POST", headers: { "Authorization": `Bearer ${CONFIG.demoKey}`, "Content-Type": "application/json" },
                                              body: JSON.stringify({ call_id: uuid() }) });
  const body = await r.json();
  if (!r.ok) throw new Error(body.error?.message || "Could not start a demo session.");
  store("halo.visitor", body);
  return (S.token = body.token);
}

async function api(path, { method = "GET", body } = {}, retried = false) {
  const r = await fetch(path, { method, headers: { "Authorization": `Bearer ${S.token}`, ...(body ? { "Content-Type": "application/json" } : {}) },
                                body: body ? JSON.stringify(body) : undefined });
  if (r.status === 401 && !retried) { await session(true); return api(path, { method, body }, true); }
  const data = await r.json().catch(() => ({}));
  return { ok: r.ok, status: r.status, data };
}

// POST that answers with Server-Sent Events: accepted, status, slow, then result.
async function stream(path, body, onEvent, retried = false) {
  const r = await fetch(path, { method: "POST", headers: { "Authorization": `Bearer ${S.token}`, "Content-Type": "application/json" },
                                body: JSON.stringify(body) });
  if (r.status === 401 && !retried) { await session(true); return stream(path, body, onEvent, true); }
  if (!r.ok) { const data = await r.json().catch(() => ({})); throw Object.assign(new Error(data.error?.message || `HTTP ${r.status}`), { data }); }
  const reader = r.body.getReader(), dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let i;
    while ((i = buf.indexOf("\n\n")) >= 0) {
      const block = buf.slice(0, i); buf = buf.slice(i + 2);
      let event = "message", data = "";
      for (const line of block.split("\n")) {
        if (line.startsWith("event:")) event = line.slice(6).trim();
        else if (line.startsWith("data:")) data += line.slice(5).trim();
      }
      if (data) onEvent(event, JSON.parse(data));
    }
  }
}

// ---------- the badge screen ----------

// Word wrap for the screens written here (the backend already wraps its own screen_lines).
function wrap(lines) {
  const cols = size(S.orientation).cols, out = [];
  for (const line of lines) {
    let cur = "";
    for (const word of String(line).split(" ")) {
      if (cur && (cur + " " + word).length > cols) { out.push(cur); cur = word; } else cur = cur ? `${cur} ${word}` : word;
    }
    out.push(cur);
  }
  return out;
}

function screen() {
  const r = S.result, lines = r?.screen_lines || [];
  const left = S.expiresAt ? Math.max(0, Math.ceil((S.expiresAt - Date.now()) / 1000)) : null;
  const right = left != null && DECISION.includes(S.phase) ? `${Math.floor(left / 60)}:${String(left % 60).padStart(2, "0")}` : "";
  switch (S.phase) {
    case "ready": return { status: "Ready", lines: wrap(["Hold the button and speak.", "", "On this demo, type a command below or tap an example."]), footer: "hold to talk" };
    case "processing": return { status: S.status || "Working on it", lines: S.adding ? ["Adding to your request..."] : ["Working on it..."], footer: "hold to add, double press to cancel" };
    case "adding": return { status: "Adding", lines: wrap(["Type what to add below, then Send."]), footer: "double press to stop" };
    case "executing": return { status: "Sending", lines: ["Working on it..."], footer: "" };
    case "review": return { status: "Ready to confirm", right, lines, footer: "press to approve, double press to cancel" };
    case "conflict_schedule": return { status: "Conflict", right, lines, footer: "" };
    case "conflict_ambiguous": {
      const c = r.candidates || [];
      if (!c.length) return { status: "Need more", right, lines, footer: "hold to answer, double press to cancel" };
      return { status: "Choose one", right, lines: [...lines, ...c.map((x) => x.label)], highlight: lines.length + S.highlight,
               footer: "rotate to choose, press to select" };
    }
    case "read_only_result": return { status: "Answer", lines, footer: "press to dismiss" };
    case "success": return { status: "Done", lines, footer: "press to continue" };
    case "failure": return { status: "Not done", lines, footer: "press to continue" };
    case "execution_uncertain": return { status: "Not sure", lines, footer: "press to check again" };
    case "cancelled": return { status: "Cancelled", lines: wrap(["Nothing was created or sent."]), footer: "press to continue" };
    case "error": return { status: "Not now", lines: wrap(S.errorLines || ["Something went wrong."]), footer: "press to continue" };
    default: return { status: S.phase, lines };
  }
}

function render() {
  const sc = screen();
  if (!S.rid) S.orientation = orientationNow(); // keep a request's orientation until it ends
  draw($("screen"), sc, S.orientation);
  const said = [sc.status, ...(sc.lines || []), sc.footer].filter(Boolean)
    .map((s) => (/[.!?:]$/.test(s) ? s : `${s}.`)).join(" ");
  if (said !== S.lastLines) { $("screen-text").textContent = said; $("screen").setAttribute("aria-label", `Badge screen: ${said}`); S.lastLines = said; }
  const pre = ["processing", ...DECISION].includes(S.phase);
  $("btn-press").disabled = S.busy || S.phase === "ready" || S.phase === "processing" || S.phase === "executing" || S.phase === "adding";
  $("btn-double").disabled = S.busy || S.phase === "ready" || S.phase === "executing";
  $("btn-hold").disabled = S.busy || !(pre || S.phase === "ready");
  const canRotate = DECISION.includes(S.phase);
  $("btn-up").disabled = $("btn-down").disabled = S.busy || !canRotate;
  $("command").classList.toggle("adding", S.phase === "adding");
  $("command-label").textContent = S.phase === "adding" ? "Add to this request" : "Say it (type it here)";
}

// ---------- results ----------

function apply(result) {
  S.result = result;
  S.revision = result.revision || S.revision;
  S.phase = result.state;
  S.highlight = 0;
  S.expiresAt = result.expires_in_s != null ? Date.now() + result.expires_in_s * 1000 : null;
  if (result.state === "review") S.guardUntil = Date.now() + CONFIRM_GUARD_MS;
  if (result.state === "executing") pollExecution(result.execution_key);
  if (["success", "failure", "execution_uncertain"].includes(result.state)) refreshPanels(true);
  if (result.state === "read_only_result") refreshMemory();
  render();
}

async function pollExecution(key) {
  for (let i = 0; i < 30 && S.phase === "executing"; i++) {
    await new Promise((res) => setTimeout(res, 1000));
    const r = await api(`/v1/executions/${key}`);
    if (r.ok && r.data.state !== "executing") { apply(r.data); return; }
  }
}

function showError(err) {
  const msg = err?.data?.error?.message || err?.message || "Something went wrong.";
  S.phase = "error"; S.errorLines = [msg.charAt(0).toUpperCase() + msg.slice(1)]; S.busy = false;
  render();
}

function scene(entry) {
  const body = $("scenes-body");
  if (body.firstChild && body.firstChild.nodeType === 3) body.textContent = "";
  const t = entry.timings || {};
  const parts = [];
  if (entry.statuses.length) parts.push(`steps: ${entry.statuses.join(", ")}`);
  if (t.model_ms != null) parts.push(`model ${(t.model_ms / 1000).toFixed(1)} s`);
  if (t.tools_ms) parts.push(`tools ${(t.tools_ms / 1000).toFixed(1)} s`);
  parts.push(`total ${(entry.ms / 1000).toFixed(1)} s`);
  const li = el("li", {}, el("b", {}, `"${entry.text}"`), ` → ${entry.state.replace(/_/g, " ")}`, el("br"),
                el("span", { class: "muted" }, parts.join(" · ")));
  let ul = body.querySelector("ul");
  if (!ul) { ul = el("ul"); body.append(ul); }
  ul.prepend(li);
  while (ul.children.length > 5) ul.lastChild.remove();
}

// ---------- commands ----------

async function send(text) {
  text = text.trim();
  if (!text || S.busy) return;
  const adding = S.phase === "adding";
  const startedAt = performance.now(), statuses = [];
  S.busy = true; S.adding = adding; S.status = ""; S.phase = "processing"; S.expiresAt = null;
  render();
  try {
    const onEvent = (event, data) => {
      if (event === "status") { S.status = data.status_line || data.status || S.status; statuses.push(S.status); render(); }
      else if (event === "slow") { S.status = "Still working"; render(); }
      else if (event === "result") {
        S.busy = false; apply(data);
        scene({ text, state: data.state, statuses, timings: data.timings, ms: performance.now() - startedAt });
      }
    };
    if (adding && S.rid) {
      await stream(`/v1/requests/${S.rid}/additions`, { call_id: uuid(), segment_id: uuid(), text, revision: S.revision, sent_at: isoNow() }, onEvent);
    } else {
      S.rid = uuid();
      S.orientation = orientationNow();
      await stream("/v1/requests", { call_id: uuid(), request_id: S.rid, segment_id: uuid(), text, sent_at: isoNow(),
                                     timezone: TZ, language: "en", orientation: S.orientation }, onEvent);
    }
  } catch (err) { showError(err); }
  finally { S.busy = false; S.adding = false; render(); }
}

async function decide(path, body) {
  S.busy = true; render();
  try {
    const r = await api(path, { method: "POST", body: { call_id: uuid(), ...body } });
    if (r.ok) apply(r.data);
    else if (r.data?.error?.current) apply(r.data.error.current); // e.g. expired: show what the request is now
    else showError({ data: r.data });
  } catch (err) { showError(err); }
  finally { S.busy = false; render(); }
}

function dismiss() { S.phase = "ready"; S.result = null; S.rid = null; S.expiresAt = null; render(); }

async function press() {
  const r = S.result;
  if (S.phase === "review") {
    if (Date.now() < S.guardUntil) return; // confirm guard: an early press is ignored, like on the badge
    S.phase = "executing"; render();
    return decide(`/v1/requests/${S.rid}/approve`, { revision: r.revision, proposal_hash: r.proposal_hash });
  }
  if (S.phase === "conflict_schedule") return decide(`/v1/requests/${S.rid}/continue`, { revision: r.revision });
  if (S.phase === "conflict_ambiguous" && r.candidates?.length) {
    const c = r.candidates[S.highlight], startedAt = performance.now(), statuses = [];
    S.busy = true; S.phase = "processing"; S.status = ""; render();
    try {
      await stream(`/v1/requests/${S.rid}/select`, { call_id: uuid(), revision: r.revision, candidate_id: c.candidate_id }, (event, data) => {
        if (event === "status") { S.status = data.status_line || data.status || S.status; statuses.push(S.status); render(); }
        else if (event === "result") { S.busy = false; apply(data); scene({ text: `picked ${c.label}`, state: data.state, statuses, timings: data.timings, ms: performance.now() - startedAt }); }
      });
    } catch (err) { showError(err); }
    finally { S.busy = false; render(); }
    return;
  }
  if (S.phase === "execution_uncertain" && r?.execution_key) {
    const x = await api(`/v1/executions/${r.execution_key}`);
    if (x.ok) apply(x.data);
    return;
  }
  if (OUTCOME.includes(S.phase) || S.phase === "error") dismiss();
}

function double() {
  if (S.phase === "adding") { S.phase = S.result?.state || "ready"; render(); return; } // stop adding, keep the proposal
  if (["processing", ...DECISION].includes(S.phase) && S.rid) return decide(`/v1/requests/${S.rid}/cancel`, {});
  dismiss();
}

function hold() {
  if (S.phase === "ready") { $("command-input").focus(); return; }
  if (DECISION.includes(S.phase) && S.rid) { S.phase = "adding"; render(); $("command-input").focus(); }
}

async function rotate(step) {
  const r = S.result;
  if (S.phase === "conflict_ambiguous" && r.candidates?.length) {
    S.highlight = (S.highlight + step + r.candidates.length) % r.candidates.length;
    render();
  }
  if (DECISION.includes(S.phase) && S.rid) { // knob activity keeps the proposal from expiring
    const t = await api(`/v1/requests/${S.rid}/touch`, { method: "POST", body: { revision: r.revision } });
    if (t.ok && t.data.expires_in_s != null) S.expiresAt = Date.now() + t.data.expires_in_s * 1000;
  }
}

setInterval(async () => {
  if (!DECISION.includes(S.phase) || !S.expiresAt) return;
  if (Date.now() >= S.expiresAt && !S.busy) {
    const r = await api(`/v1/requests/${S.rid}`);
    if (r.ok) apply(r.data);
  } else render();
}, 1000);

// ---------- panels ----------

const timeFmt = new Intl.DateTimeFormat(undefined, { hour: "numeric", minute: "2-digit" });
const dayFmt = new Intl.DateTimeFormat(undefined, { weekday: "short", month: "short", day: "numeric" });
let seen = { calendar: new Set(), outbox: new Set(), memory: new Set() };

function fill(list, items, key, render, flash) {
  const ul = $(list);
  ul.replaceChildren(...(items.length ? items.flatMap((x) => {
    const node = render(x);
    if (flash && key && !seen[list].has(key(x)) && seen[list].size) node.classList?.add("new");
    return [node];
  }) : [el("li", { class: "muted" }, "Nothing here yet.")]));
  if (key) seen[list] = new Set(items.map(key));
}

async function refreshCalendar(flash) {
  const r = await api(`/v1/demo/calendar?tz=${encodeURIComponent(TZ)}&days=7`);
  if (!r.ok) return;
  const nodes = [];
  let day = "";
  const ul = $("calendar");
  for (const e of r.data.events) {
    const s = new Date(e.start), d = dayFmt.format(s);
    if (d !== day) { nodes.push(el("li", { class: "day" }, d)); day = d; }
    const li = el("li", { class: "row" }, el("span", {}, e.title), el("span", { class: "when" }, `${timeFmt.format(s)} to ${timeFmt.format(new Date(e.end))}`));
    if (flash && seen.calendar.size && !seen.calendar.has(e.event_id + e.start)) li.classList.add("new");
    nodes.push(li);
  }
  seen.calendar = new Set(r.data.events.map((e) => e.event_id + e.start));
  ul.replaceChildren(...(nodes.length ? nodes : [el("li", { class: "muted" }, "Nothing this week.")]));
}

async function refreshOutbox(flash) {
  const r = await api("/v1/demo/outbox");
  if (!r.ok) return;
  fill("outbox", r.data.emails, (m) => m.sent_at + m.to, (m) => el("li", {},
    el("div", { class: "row" }, el("b", {}, m.subject), el("span", { class: "when" }, timeFmt.format(new Date(m.sent_at)))),
    el("div", { class: "muted" }, `To: ${m.to}`), el("div", {}, m.body)), flash);
}

function readable(value) { // "2026-10-20T15:00:00-05:00" -> "Tue, Oct 20, 3:00 PM"
  return /^\d{4}-\d\d-\d\dT\d\d:\d\d/.test(value)
    ? `${dayFmt.format(new Date(value))}, ${timeFmt.format(new Date(value))}` : value;
}

async function refreshMemory(flash) {
  const r = await api("/v1/memory");
  if (!r.ok) return;
  fill("memory", r.data.items, (m) => m.memory_id + m.value, (m) => {
    const li = el("li", { class: "row" },
      el("span", {}, el("span", { class: "tag" }, m.kind), " ", el("b", {}, m.label), `: ${readable(m.value)}`));
    if (m.kind !== "action") {
      const b = el("button", { class: "link-btn", "aria-label": `Forget ${m.label}` }, "Forget");
      b.addEventListener("click", async () => {
        await api(`/v1/memory/${m.memory_id}`, { method: "DELETE", body: { call_id: uuid() } });
        refreshMemory();
      });
      li.append(b);
    }
    return li;
  }, flash);
}

async function refreshContacts() {
  const r = await api("/v1/demo/contacts");
  if (!r.ok) return;
  fill("contacts", r.data.contacts, null, (c) => el("li", { class: "row" }, el("span", {}, c.name), el("span", { class: "when" }, c.email)));
}

function refreshPanels(flash = false) {
  return Promise.all([refreshCalendar(flash), refreshOutbox(flash), refreshMemory(flash)]);
}

// ---------- theme: Auto follows the system; Light or Dark is remembered on this browser ----------

const THEMES = ["auto", "light", "dark"];
function applyTheme(theme) {
  if (theme === "auto") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = theme;
  const label = theme[0].toUpperCase() + theme.slice(1);
  $("theme").textContent = `Theme: ${label}`;
  $("theme").setAttribute("aria-label", theme === "auto" ? "Color theme: follows your system" : `Color theme: ${label}`);
  render(); // the badge is drawn on a canvas, so redraw it with the new colors
}

// ---------- start ----------

function chips() {
  const box = $("chips");
  for (const [group, items] of EXAMPLES) {
    box.append(el("div", { class: "chip-group" }, group));
    for (const t of items) {
      const b = el("button", { class: "chip", type: "button" }, t);
      b.addEventListener("click", () => {
        if (S.phase !== "ready" && !OUTCOME.includes(S.phase) && S.phase !== "error") return;
        dismiss(); $("command-input").value = ""; send(t);
      });
      box.append(b);
    }
  }
}

async function main() {
  chips();
  applyTheme(THEMES.includes(load("halo.theme")) ? load("halo.theme") : "auto");
  $("theme").addEventListener("click", () => {
    const next = THEMES[(THEMES.indexOf(document.documentElement.dataset.theme || "auto") + 1) % THEMES.length];
    store("halo.theme", next === "auto" ? null : next);
    applyTheme(next);
  });
  $("command").addEventListener("submit", (e) => {
    e.preventDefault();
    const input = $("command-input");
    if (OUTCOME.includes(S.phase) || S.phase === "error") dismiss();
    if (S.phase !== "ready" && S.phase !== "adding") return;
    const text = input.value; input.value = ""; send(text);
  });
  $("btn-press").addEventListener("click", press);
  $("btn-double").addEventListener("click", double);
  $("btn-hold").addEventListener("click", hold);
  $("btn-up").addEventListener("click", () => rotate(-1));
  $("btn-down").addEventListener("click", () => rotate(1));
  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", render);
  NARROW.addEventListener("change", render);
  try {
    await session();
    await Promise.all([refreshPanels(), refreshContacts()]);
  } catch (err) { showError(err); }
}

main();
