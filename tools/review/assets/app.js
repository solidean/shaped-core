// The review page. Vanilla, no framework, no build step.
//
// Two invariants drive everything here.
// Typed text is never discarded -- not by a reload, not by a rejected save, not by a question that changed underneath it.
// And the page never holds authoritative state: the server is re-asked, because the folder on disk is the review.

const state = {
  data: null,
  current: null,
  pending: new Map(),
  dirty: new Set(),
  staleContent: false,
  sendArmed: false,
  shutdownArmed: false,
  stopped: false,
  // Digests this tab caused by saving an answer. A reload carrying one of them is our own write coming back.
  selfDigests: new Set(),
  // Forms whose last save did not reach the server, and when the first of them started failing.
  unsaved: new Map(),
  failingSince: 0,
  // The round this window rendered. Once the server moves past it, everything on screen is history.
  round: null,
  stale: false,
};

const SAVE_DEBOUNCE_MS = 400;

// How long typed text may sit unsaved before the page stops being subtle about it.
// A status line in the corner is enough for a hiccup and nowhere near enough for a dead server:
// an hour of answering that saved must never look like an hour of answering that did not.
const ALARM_AFTER_MS = 4000;
const RETRY_EVERY_MS = 3000;

const el = (id) => document.getElementById(id);

// ---- fetching ---------------------------------------------------------------

// A rejected fetch -- server gone, network dropped -- must not throw past the caller.
// An uncaught rejection there is the silent failure this page cannot afford: the save just never happens and nothing says so.
async function getJSON(url) {
  try {
    const response = await fetch(url, { headers: { Accept: "application/json" } });
    return { ok: response.ok, status: response.status, body: await response.json() };
  } catch (e) {
    return { ok: false, status: 0, unreachable: true, body: { error: "the server did not answer" } };
  }
}

async function postJSON(url, payload) {
  try {
    const response = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    return { ok: response.ok, status: response.status, body: await response.json() };
  } catch (e) {
    return { ok: false, status: 0, unreachable: true, body: { error: "the server did not answer" } };
  }
}

// ---- navigation -------------------------------------------------------------

function entryMark(row) {
  if (row.error) return { text: "!", cls: "bad" };
  if (!row.asks) return { text: "", cls: "" };
  if (row.answered === row.asks) return { text: "✓", cls: "ok" };
  if (row.answered > 0) return { text: "◑", cls: "partial" };
  return { text: "○", cls: "" };
}

function renderNav() {
  const data = state.data;
  el("review-name").textContent = data.title || data.name;
  el("review-range").textContent = data.range;
  el("review-goals").textContent = data.goals.join(" + ") + " · round " + data.round;

  const progress = data.progress;
  const share = progress.asks ? (progress.answered / progress.asks) * 100 : 0;
  el("bar-asks").style.width = share + "%";
  el("progress-text").textContent =
    `${progress.answered}/${progress.asks} answered · ` +
    `${progress.discharged}/${progress.changes} changes discharged`;

  const list = el("nav-list");
  list.textContent = "";

  for (const row of navRows()) {
    const item = document.createElement("div");
    item.className = "nav-entry";
    if (row.slug === state.current) item.classList.add("current");
    if (row.state !== "open") item.classList.add("obsolete");
    if (row.error) item.classList.add("broken");
    if (row.asks && row.answered === row.asks) item.classList.add("done");

    const id = document.createElement("span");
    id.className = "nav-id";
    id.textContent = row.id;
    const title = document.createElement("span");
    title.className = "nav-title";
    title.textContent = row.title;
    const group = document.createElement("span");
    group.className = "nav-group-tag";
    group.textContent = row.group;
    const mark = entryMark(row);
    const flag = document.createElement("span");
    flag.className = "nav-mark " + mark.cls;
    flag.textContent = mark.text;

    item.append(id, title, group, flag);
    item.addEventListener("click", () => selectEntry(row.slug));
    list.appendChild(item);
  }
}

// ---- entry ------------------------------------------------------------------

async function selectEntry(slug, { push = true } = {}) {
  if (!slug) return;
  const result = await getJSON("/api/entry/" + encodeURIComponent(slug));
  if (!result.ok) {
    // Returning silently here makes a server-side render failure look like a nav item that does not respond to clicks.
    setSaveState(`${slug}: ${result.body.error || "could not be rendered"}`, "bad");
    return;
  }

  // Re-rendering the entry you are already on must not move you; only actually navigating goes back to the top.
  const staying = slug === state.current;
  const scroll = staying ? window.scrollY : 0;
  const focused = staying ? (document.querySelector(".ask.focus") || {}).id : "";

  state.current = slug;
  state.staleContent = false;
  el("content").innerHTML = result.body.html;
  if (push) history.replaceState(null, "", "#" + slug);
  wireForms();
  renderNav();

  window.scrollTo(0, scroll);
  if (focused) {
    // The focus class is what `1`-`9` aims at, so losing it across a refresh silently retargets the number keys.
    const ask = document.getElementById(focused);
    if (ask) ask.classList.add("focus");
  }
}

// The one reading order — the agent's own numbering, which is the order it wants the review read in.
// Grouping the nav by group would reshuffle that sequence, and then j/k could not agree with what is on screen.
function navRows() {
  if (!state.data) return [];
  const filter = el("filter").value.trim().toLowerCase();
  if (!filter) return state.data.entries;
  return state.data.entries.filter(
    (r) => r.title.toLowerCase().includes(filter) || r.id.includes(filter) || r.group.includes(filter)
  );
}

function orderedSlugs() {
  return navRows().map((r) => r.slug);
}

function step(delta) {
  const slugs = orderedSlugs();
  const at = slugs.indexOf(state.current);
  const next = slugs[Math.min(Math.max(at + delta, 0), slugs.length - 1)];
  if (next && next !== state.current) selectEntry(next);
}

function nextUnanswered() {
  const rows = navRows();
  const at = rows.findIndex((r) => r.slug === state.current);
  const pending = (r) => r.state === "open" && (r.error || (r.asks && r.answered < r.asks));
  const after = rows.slice(at + 1).find(pending) || rows.find(pending);
  if (after) selectEntry(after.slug);
  else setSaveState("everything is answered");
}

// ---- when the window can no longer be trusted --------------------------------

function markStale(reason) {
  // A round that was handed back is finalized, so the forms on screen still look answerable and are not.
  // Nothing here is recoverable by retrying, which is why the window is sealed rather than nagged about.
  if (state.stale) return;
  state.stale = true;
  state.pending.forEach((timer) => clearTimeout(timer));
  state.pending.clear();
  el("stale-reason").textContent = reason;
  el("stale-veil").hidden = false;
  document.body.classList.add("is-stale");
}

function alarmText() {
  const seconds = Math.round((Date.now() - state.failingSince) / 1000);
  const n = state.unsaved.size;
  return n + " answer" + (n === 1 ? "" : "s") + " not saved for " + seconds + "s — still retrying; keep this tab open";
}

function noteUnsaved(form, key) {
  state.unsaved.set(key, form);
  state.dirty.add(key);
  if (!state.failingSince) state.failingSince = Date.now();
}

function noteSaved(key) {
  state.unsaved.delete(key);
  if (state.unsaved.size) return;
  state.failingSince = 0;
  document.body.classList.remove("save-alarm");
  el("save-alarm-bar").hidden = true;
}

function alarmTick() {
  if (state.stale || !state.unsaved.size) return;
  if (Date.now() - state.failingSince < ALARM_AFTER_MS) return;
  document.body.classList.add("save-alarm");
  el("save-alarm-bar").hidden = false;
  el("save-alarm-text").textContent = alarmText();
}

function retryUnsaved() {
  if (state.stale) return;
  state.unsaved.forEach((form) => saveForm(form));
}

// ---- answering --------------------------------------------------------------

function setSaveState(text, cls = "") {
  const node = el("save-state");
  node.textContent = text;
  node.className = cls;
}

function formPayload(form) {
  const selected = [...form.querySelectorAll("input:checked")].map((i) => i.value);
  const text = form.querySelector(".freeform").value;
  return {
    entry: form.dataset.entry,
    ask: form.dataset.ask,
    hash: form.dataset.hash,
    round: state.round,
    selected,
    text,
  };
}

async function saveForm(form) {
  if (state.stale) return;
  const key = form.dataset.entry + "::" + form.dataset.ask;
  const status = form.querySelector(".ask-status");
  const result = await postJSON("/api/answer", formPayload(form));
  state.dirty.delete(key);

  // Unreachable or broken is the one case where the text on screen is the only copy of it,
  // so the key stays dirty and the form goes on being retried until it lands.
  if (result.unreachable || result.status >= 500) {
    noteUnsaved(form, key);
    status.className = "ask-status bad";
    status.textContent = "not saved — retrying";
    return;
  }
  if (result.body && result.body.stale) {
    markStale(result.body.error);
    return;
  }

  if (result.status === 410) {
    noteSaved(key);
    status.className = "ask-status bad";
    status.textContent = result.body.error;
    return;
  }
  if (result.status === 409) {
    noteSaved(key);
    status.className = "ask-status warn";
    status.textContent = result.body.error + " — your text is saved against the new wording";
    if (result.body.hash) form.dataset.hash = result.body.hash;
    return;
  }
  if (!result.ok) {
    noteUnsaved(form, key);
    status.className = "ask-status bad";
    status.textContent = result.body.error || "could not save";
    return;
  }
  noteSaved(key);
  status.className = "ask-status";
  status.textContent = "saved";
  setSaveState("saved " + new Date().toLocaleTimeString());
  if (result.body.digest) state.selfDigests.add(result.body.digest);
  refreshState();
}

function scheduleSave(form) {
  if (state.stale) return;
  const key = form.dataset.entry + "::" + form.dataset.ask;
  state.dirty.add(key);
  clearTimeout(state.pending.get(key));
  state.pending.set(key, setTimeout(() => saveForm(form), SAVE_DEBOUNCE_MS));
}

function flushSave(form) {
  const key = form.dataset.entry + "::" + form.dataset.ask;
  if (!state.dirty.has(key)) return;
  clearTimeout(state.pending.get(key));
  saveForm(form);
}

function wireForms() {
  for (const form of document.querySelectorAll(".ask-form")) {
    form.addEventListener("change", () => scheduleSave(form));
    const area = form.querySelector(".freeform");
    area.addEventListener("input", () => scheduleSave(form));
    area.addEventListener("blur", () => flushSave(form));
    form.addEventListener("focusin", () => {
      for (const ask of document.querySelectorAll(".ask")) ask.classList.remove("focus");
      form.closest(".ask").classList.add("focus");
    });

    form.querySelectorAll(".opt").forEach((opt, index) => {
      if (index > 8) return;
      const key = document.createElement("span");
      key.className = "opt-key";
      key.textContent = String(index + 1);
      opt.insertBefore(key, opt.firstChild);
    });
  }
}

function pickOption(number) {
  const form = document.querySelector(".ask.focus .ask-form") || document.querySelector(".ask-form");
  if (!form) return;
  const options = form.querySelectorAll(".opt input");
  const target = options[number - 1];
  if (!target) return;
  target.checked = target.type === "checkbox" ? !target.checked : true;
  scheduleSave(form);
}

// ---- rounds -----------------------------------------------------------------

// Sending is the one irreversible act on this page: it freezes the round, moves the watermark and unblocks the agent.
// So it is armed first and sent second, and a single stray keystroke cannot do it.
const SEND_ARM_MS = 4000;
let sendArmTimer = null;

// Handing a round back is irreversible from the page, so the button opens the round rather than sending it.
// An ask is "answered" as soon as one option is picked, which leaves an untaken checkbox indistinguishable from one nobody read —
// this is the last place the person who would know can still tell the difference.
async function requestSend() {
  for (const form of document.querySelectorAll(".ask-form")) flushSave(form);

  const result = await getJSON("/api/summary");
  if (!result.ok) {
    setSaveState(result.body.error || "could not read the round back", "bad");
    return;
  }
  renderSendSummary(result.body);
  el("send-veil").hidden = false;
  el("send-confirm").focus();
}

function summaryRow(ask) {
  const chosen = ask.selected.length ? `<ul class="send-chosen">${ask.selected.map((c) => `<li>${_esc(c)}</li>`).join("")}</ul>` : "";
  const text = ask.text ? `<div class="send-text">${_esc(ask.text)}</div>` : "";

  let extras = "";
  if (ask.checks_offered) {
    const untaken = ask.checks_offered - ask.checks_taken;
    extras = untaken
      ? `<div class="send-flag warn">${untaken} of ${ask.checks_offered} optional item${ask.checks_offered === 1 ? "" : "s"} not taken</div>`
      : `<div class="send-flag ok">all ${ask.checks_offered} optional items taken</div>`;
  }

  const flag = ask.answered ? "" : '<div class="send-flag bad">nothing answered here</div>';
  const stamp = ask.finalized ? '<span class="send-old">answered in an earlier round</span>' : "";
  return `<div class="send-ask"><div class="send-ask-head"><code>${_esc(ask.name)}</code>${stamp}</div>${flag}${chosen}${text}${extras}</div>`;
}

function renderSendSummary(data) {
  el("send-title").textContent = `Hand round ${data.round} back?`;

  const fresh = [];
  for (const row of data.entries) {
    const asks = row.asks.filter((a) => !a.finalized);
    if (!asks.length) continue;
    fresh.push(
      `<section class="send-entry"><h3>${_esc(row.entry)} ${_esc(row.title)}</h3>${asks.map(summaryRow).join("")}</section>`
    );
  }

  el("send-body").innerHTML = fresh.length
    ? fresh.join("")
    : '<p class="send-empty">Nothing new since the last round. Sending hands back an empty round.</p>';
}

function closeSendSummary() {
  el("send-veil").hidden = true;
}

function _esc(text) {
  const d = document.createElement("div");
  d.textContent = text == null ? "" : String(text);
  return d.innerHTML;
}

// Closing the server ends the session for every tab, so it arms first exactly as sending does.
async function requestShutdown() {
  const button = el("shutdown");
  if (!state.shutdownArmed) {
    state.shutdownArmed = true;
    button.classList.add("armed");
    setSaveState("click Close server again to shut it down", "warn");
    setTimeout(() => {
      state.shutdownArmed = false;
      button.classList.remove("armed");
    }, SEND_ARM_MS);
    return;
  }

  state.shutdownArmed = false;
  button.classList.remove("armed");
  for (const form of document.querySelectorAll(".ask-form")) flushSave(form);

  // The server stops answering the moment it acknowledges, so a dropped connection here is success, not failure.
  try {
    await postJSON("/api/shutdown", {});
  } catch (e) {
    // ignored on purpose
  }
  state.stopped = true;
  el("stopped-veil").hidden = false;
}

async function signal(action) {
  for (const form of document.querySelectorAll(".ask-form")) flushSave(form);
  const result = await postJSON("/api/signal", { action });
  if (result.ok) {
    setSaveState(action === "send" ? "handed back — you can close this tab" : "paused");
  } else {
    setSaveState(result.body.error || "could not signal", "bad");
  }
}

// ---- live updates -----------------------------------------------------------

function typing() {
  const active = document.activeElement;
  return active && el("content").contains(active) && active.tagName === "TEXTAREA";
}

async function refreshState() {
  const result = await getJSON("/api/state");
  if (!result.ok) return;
  state.data = result.body;
  if (state.round === null) state.round = result.body.round;
  else if (result.body.round !== state.round) {
    markStale("This window is showing round " + state.round + ", which has been handed back. The review is on round " + result.body.round + " now.");
  }
  renderNav();
}

function listen() {
  const events = new EventSource("/events");
  events.addEventListener("reload", async (event) => {
    await refreshState();
    if (state.selfDigests.has(event.data)) {
      // Our own answer coming back. The nav counters are refreshed above, and the entry on screen is already right.
      state.selfDigests.delete(event.data);
      return;
    }
    if (state.dirty.size || typing()) {
      state.staleContent = true;
      setSaveState("this entry changed — it will refresh when you stop typing", "warn");
      return;
    }
    if (state.current) await selectEntry(state.current, { push: false });
  });
  events.onerror = () => {
    if (state.stopped || state.stale) { events.close(); return; }
    setSaveState("live updates disconnected", "warn");
  };

  setInterval(() => {
    if (state.staleContent && !state.dirty.size && !typing()) {
      state.staleContent = false;
      selectEntry(state.current, { push: false });
    }
  }, 1500);

  setInterval(alarmTick, 1000);
  setInterval(retryUnsaved, RETRY_EVERY_MS);
  // The server decides which round is current, and a dead EventSource is exactly when a tab drifts without noticing.
  setInterval(refreshState, 5000);
}

// ---- keyboard ---------------------------------------------------------------

function shortcuts(event) {
  if (event.key === "Escape") {
    el("help").hidden = true;
    closeSendSummary();
    if (document.activeElement) document.activeElement.blur();
    return;
  }
  const typingNow = ["TEXTAREA", "INPUT"].includes(document.activeElement.tagName);
  if (typingNow) {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
      const form = document.activeElement.closest(".ask-form");
      if (form) flushSave(form);
      nextUnanswered();
      event.preventDefault();
    }
    return;
  }

  if (event.key === "j") step(1);
  else if (event.key === "k") step(-1);
  else if (event.key === "n") nextUnanswered();
  else if (event.key === "s") requestSend();
  else if (event.key === "p") signal("pause");
  else if (event.key === "?") el("help").hidden = !el("help").hidden;
  else if (event.key === "/") { el("filter").focus(); event.preventDefault(); }
  else if (/^[1-9]$/.test(event.key)) pickOption(Number(event.key));
  else return;
  event.preventDefault();
}

// ---- start ------------------------------------------------------------------

async function main() {
  await refreshState();
  const wanted = location.hash.slice(1) || (state.data.entries[0] && state.data.entries[0].slug);
  if (wanted) await selectEntry(wanted, { push: false });

  el("prev").addEventListener("click", () => step(-1));
  el("next").addEventListener("click", () => step(1));
  el("next-open").addEventListener("click", nextUnanswered);
  el("send").addEventListener("click", requestSend);
  el("send-confirm").addEventListener("click", () => { closeSendSummary(); signal("send"); });
  el("send-cancel").addEventListener("click", closeSendSummary);
  el("send-veil").addEventListener("click", (e) => { if (e.target === el("send-veil")) closeSendSummary(); });
  el("pause").addEventListener("click", () => signal("pause"));
  el("shutdown").addEventListener("click", requestShutdown);
  el("help-toggle").addEventListener("click", () => { el("help").hidden = !el("help").hidden; });
  el("help").addEventListener("click", () => { el("help").hidden = true; });
  el("filter").addEventListener("input", renderNav);
  el("filter").addEventListener("keydown", (e) => { if (e.key === "Escape") { el("filter").value = ""; el("filter").blur(); renderNav(); } });
  document.addEventListener("keydown", shortcuts);
  window.addEventListener("beforeunload", () => {
    for (const form of document.querySelectorAll(".ask-form")) flushSave(form);
  });

  listen();
}

main();
