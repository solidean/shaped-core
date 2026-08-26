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
  annotate(el("content"), result.body.tokens);
  wireForms();
  wireComments();
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

// ---- annotation -------------------------------------------------------------
//
// One pass with a provider per kind of reference, rather than four render passes that would each grow their own
// idea of what a code block is.
//
// The page decides nothing. Python found and resolved every reference over the entry source -- fences included --
// and handed down a table of literal tokens; this walks text nodes and wraps the ones it finds. A text node is by
// construction inside one element, so a match can never cross a boundary, which is what makes annotating inside
// highlighted code work at all.

// Which regions each provider's tokens may be decorated in. Server-side defaults, togglable here without a
// re-render, which is what "configurable even if the option is not exposed" means.
const ANNOTATE = { file: true, glossary: true, commit: true, symbol: true };

function regionOf(node) {
  if (node.closest("pre, code, .difflines")) return node.closest(".difflines") ? "diff" : "code";
  return "prose";
}

function annotate(root, tokens) {
  // The overview's tree emits its own links: a row shows a basename while the link needs the whole path, so the
  // pass has no literal to match on. They still want the same peek.
  for (const el of root.querySelectorAll("a.annot[data-path]")) {
    el.addEventListener("mouseenter", () => peek(el));
    el.addEventListener("mouseleave", schedulePopoverClose);
  }
  if (!tokens || !tokens.length) return;
  const live = tokens.filter((t) => ANNOTATE[t.kind] !== false);
  if (!live.length) return;

  // Longest first, so `lib/render/markdown.py` wins over `markdown.py` at the same position.
  const byLength = [...live].sort((a, b) => b.text.length - a.text.length);
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const targets = [];
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    if (node.parentElement.closest("a, mark, .annot, textarea, .comment-composer")) continue;
    targets.push(node);
  }

  // A glossary term is drawn once per block. A term used eleven times in a paragraph becomes eleven dotted
  // underlines, which reads as a rash rather than as help.
  const drawnTerms = new Map();

  for (const node of targets) {
    const region = regionOf(node.parentElement);
    const block = node.parentElement.closest(".block") || root;
    if (!drawnTerms.has(block)) drawnTerms.set(block, new Set());
    const done = drawnTerms.get(block);
    const usable = byLength.filter((t) => {
      if (!t.regions.includes(region)) return false;
      return t.kind !== "glossary" || !done.has(t.text.toLowerCase());
    });
    if (usable.length) {
      for (const hit of wrapNode(node, usable)) {
        if (hit.kind === "glossary") done.add(hit.text.toLowerCase());
      }
    }
  }
}

function wrapNode(node, tokens) {
  const text = node.nodeValue;
  const hits = [];
  for (const token of tokens) {
    let from = 0;
    for (;;) {
      const at = text.indexOf(token.text, from);
      if (at < 0) break;
      // No nesting and no overlap: the first provider to claim a span owns it.
      if (!hits.some((h) => at < h.end && at + token.text.length > h.start)) {
        hits.push({ start: at, end: at + token.text.length, token });
      }
      from = at + token.text.length;
    }
  }
  if (!hits.length) return [];
  hits.sort((a, b) => a.start - b.start);

  const fragment = document.createDocumentFragment();
  let cursor = 0;
  for (const hit of hits) {
    if (hit.start > cursor) fragment.append(text.slice(cursor, hit.start));
    fragment.append(decorate(hit.token));
    cursor = hit.end;
  }
  if (cursor < text.length) fragment.append(text.slice(cursor));
  node.parentNode.replaceChild(fragment, node);
  return hits.map((h) => h.token);
}

function decorate(token) {
  const el = document.createElement(token.href ? "a" : "span");
  el.className = "annot " + (token.css || token.kind);
  el.textContent = token.label || token.text;
  el.dataset.kind = token.kind;
  if (token.href) {
    el.href = token.href;
    el.target = "_blank";
    el.rel = "noopener";
  }
  if (token.path) {
    el.dataset.path = token.path;
    el.dataset.line = String(token.line || 0);
    el.addEventListener("mouseenter", () => peek(el));
    el.addEventListener("mouseleave", schedulePopoverClose);
  }
  if (token.kind === "commit") {
    el.dataset.sha = token.text;
    el.addEventListener("mouseenter", () => peekCommit(el));
    el.addEventListener("mouseleave", schedulePopoverClose);
  }
  if (token.note) {
    el.title = "";
    el.addEventListener("mouseenter", () => showPopover(el, `<div class="pop-def">${mdInline(token.note)}</div>`));
    el.addEventListener("mouseleave", schedulePopoverClose);
  }
  // Clicking a term goes to the entry that defines it, for the reader who wants more than a sentence.
  if (token.target) {
    el.style.cursor = "pointer";
    el.addEventListener("click", (e) => { e.preventDefault(); selectEntry(token.target); });
  }
  return el;
}

// The definition is one line of markdown, and only its bold lead ever matters.
function mdInline(text) {
  return _esc(text).replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>").replace(/`([^`]+)`/g, "<code>$1</code>");
}

// ---- the popover ------------------------------------------------------------
//
// One popover shared by every provider. A second one would be a second set of positioning bugs.

let popoverTimer = 0;

function popover() {
  let box = document.getElementById("popover");
  if (!box) {
    box = document.createElement("div");
    box.id = "popover";
    box.hidden = true;
    box.addEventListener("mouseenter", () => clearTimeout(popoverTimer));
    box.addEventListener("mouseleave", schedulePopoverClose);
    document.body.appendChild(box);
  }
  return box;
}

function showPopover(anchor, html) {
  const box = popover();
  clearTimeout(popoverTimer);
  box.innerHTML = html;
  box.hidden = false;
  const rect = anchor.getBoundingClientRect();
  box.style.left = Math.max(8, Math.min(rect.left, window.innerWidth - box.offsetWidth - 8)) + "px";
  const below = rect.bottom + 6;
  box.style.top = (below + box.offsetHeight > window.innerHeight ? rect.top - box.offsetHeight - 6 : below) + "px";
}

function schedulePopoverClose() {
  clearTimeout(popoverTimer);
  popoverTimer = setTimeout(() => { popover().hidden = true; }, 180);
}

const peekCache = new Map();

async function peek(el) {
  const key = el.dataset.path + "#" + el.dataset.line;
  if (!peekCache.has(key)) {
    const result = await getJSON(
      `/api/file?path=${encodeURIComponent(el.dataset.path)}&line=${el.dataset.line}`);
    if (!result.ok) return;
    peekCache.set(key, result.body);
  }
  const body = peekCache.get(key);
  showPopover(el,
    `<div class="pop-head">${_esc(body.path)}  <span>lines ${body.start}-${body.end} of ${body.lines}</span></div>` +
    `<pre class="pg"><code>${body.html}</code></pre>`);
}

const commitCache = new Map();

async function peekCommit(el) {
  const sha = el.dataset.sha;
  if (!commitCache.has(sha)) {
    const result = await getJSON("/api/commit?sha=" + encodeURIComponent(sha));
    if (!result.ok) return;
    commitCache.set(sha, result.body);
  }
  const c = commitCache.get(sha);
  // The forge link lives in the popover rather than on the sha: `upstream` is not always there, and a popover
  // can lose a line without the reference losing its link.
  const forge = c.forge ? `<a href="${_esc(c.forge)}" target="_blank" rel="noopener">open on the forge</a>` : "";
  showPopover(el,
    `<div class="pop-head"><code>${_esc(c.short)}</code><span>${_esc(c.author)} · ${_esc(c.date)}</span></div>` +
    `<div class="pop-subject">${_esc(c.subject)}</div>` +
    (c.body ? `<pre class="pop-body">${_esc(c.body)}</pre>` : "") +
    (c.stat ? `<div class="pop-stat">${_esc(c.stat)}</div>` : "") +
    (forge ? `<div class="pop-link">${forge}</div>` : ""));
}

// ---- comments ---------------------------------------------------------------
//
// A comment is a remark, never a tracked question: the agent answers one by appending a block that names it.
// So there is nothing to track here -- the composer writes, the server stores, and the round carries it over.

async function saveComment(anchor, text, { id = "", change = "", offset = -1 } = {}) {
  const result = await postJSON("/api/comment", {
    entry: state.current, id, text, block: change ? "" : anchor, change, offset,
  });
  if (!result.ok) {
    setSaveState(result.body.error || "the comment did not save", "bad");
    return false;
  }
  if (result.body.digest) state.selfDigests.add(result.body.digest);
  setSaveState(text.trim() ? "comment saved" : "comment removed", "ok");
  await selectEntry(state.current, { push: false });
  return true;
}

// The composer is one element reused wherever it is opened, so there is never a second half-typed box on screen.
function openComposer(host, { anchor, id = "", text = "", change = "", offset = -1 }) {
  closeComposer();
  const box = document.createElement("div");
  box.className = "comment-composer";
  const area = document.createElement("textarea");
  area.rows = 3;
  area.value = text;
  area.placeholder = change ? "a remark on this line" : "a remark on this block";
  const actions = document.createElement("div");
  actions.className = "comment-actions";
  const save = document.createElement("button");
  save.type = "button";
  save.textContent = id ? "save" : "comment";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.className = "ghost";
  cancel.textContent = "cancel";

  save.addEventListener("click", () => saveComment(anchor, area.value, { id, change, offset }));
  cancel.addEventListener("click", closeComposer);
  area.addEventListener("keydown", (e) => {
    if (e.key === "Escape") { e.stopPropagation(); closeComposer(); }
    if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) saveComment(anchor, area.value, { id, change, offset });
  });

  actions.append(save, cancel);
  if (id) {
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "ghost danger";
    remove.textContent = "delete";
    remove.addEventListener("click", () => saveComment(anchor, "", { id, change, offset }));
    actions.append(remove);
  }
  box.append(area, actions);
  host.appendChild(box);
  area.focus();
}

function closeComposer() {
  for (const box of document.querySelectorAll(".comment-composer")) box.remove();
}

function wireComments() {
  for (const button of document.querySelectorAll(".comment-add")) {
    button.addEventListener("click", () => {
      const slot = button.closest(".comment-slot");
      openComposer(slot, { anchor: slot.dataset.anchor });
    });
  }

  for (const card of document.querySelectorAll(".comment-card")) {
    if (card.querySelector(".comment-state").textContent.startsWith("sent")) continue;
    card.querySelector(".comment-text").addEventListener("click", () => {
      const slot = card.closest(".comment-slot") || card.parentElement;
      openComposer(slot, {
        anchor: slot.dataset ? slot.dataset.anchor || "" : "",
        id: card.dataset.comment,
        text: card.querySelector(".comment-text").textContent,
      });
    });
  }

  // A line comment anchors on the change id plus the offset into that change's diff, which is stable
  // for exactly as long as the change id is.
  for (const table of document.querySelectorAll(".change .difflines")) {
    const change = table.closest(".change").querySelector(".change-head code").textContent;
    for (const row of table.querySelectorAll("tr[data-off]")) {
      row.querySelector(".dl-src").addEventListener("dblclick", () => {
        const host = table.closest(".change");
        openComposer(host, { anchor: "", change, offset: Number(row.dataset.off) });
      });
    }
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
