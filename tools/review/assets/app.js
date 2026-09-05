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

// The nav row preview is off.
//
// It works, and the whole entry in a popover is genuinely useful, but a full render is slow enough to open that
// the hover feels laggy, and it survives a pointer moving into the main content by a path the leave handler
// does not see — so it sits over what you were trying to read.
// Both are fixable with a hover delay and a pointer-position check rather than a leave event, and neither is
// worth doing blind. Flip this to re-enable; everything behind it is intact.
const PREVIEW_ENTRIES = false;

function renderNav() {
  entryPeekCache.clear();
  const data = state.data;
  const shown = data.title || data.name;
  el("review-name").textContent = shown;
  // The tab is how you find a review among several, so the title is the review rather than the tool.
  document.title = shown;
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
    if (PREVIEW_ENTRIES && !row.error) {
      hoverPopover(item, () => peekEntry(item, row.slug));
    } else {
      // The title only. Opened beside the row rather than under it, so the list you are scanning stays visible.
      hoverPopover(item, () => peekNavTitle(item, title, row));
    }
    list.appendChild(item);
  }
}

// ---- entry ------------------------------------------------------------------

// Where a navigation's time went, printed when `?timings` is in the URL.
//
// The server reports its own halves at /api/timings; this is the other side of the same question, because a
// click that feels slow is fetch + inject + annotate and only the first of those is the server's.
// `annotate` is the one worth watching: it walks every text node in the entry against every token, so it
// grows with the entry rather than with the change.
const SHOW_TIMINGS = new URLSearchParams(location.search).has("timings");

function timed(label, fn) {
  if (!SHOW_TIMINGS) return fn();
  const started = performance.now();
  const out = fn();
  console.log(`[timing] ${label.padEnd(18)} ${(performance.now() - started).toFixed(1)} ms`);
  return out;
}

async function selectEntry(slug, { push = true } = {}) {
  if (!slug) return;
  const fetchStarted = performance.now();
  const result = await getJSON("/api/entry/" + encodeURIComponent(slug));
  if (SHOW_TIMINGS) console.log(`[timing] ${"fetch".padEnd(18)} ${(performance.now() - fetchStarted).toFixed(1)} ms`);
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
  timed("inject", () => { el("content").innerHTML = result.body.html; });
  if (push) history.replaceState(null, "", "#" + slug);
  timed("annotate", () => annotate(el("content"), result.body.tokens));
  timed("wire", () => { wireForms(); wireComments(); });
  timed("nav", () => renderNav());

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
const ANNOTATE = { file: true, dir: true, glossary: true, commit: true, symbol: true };

function regionOf(node) {
  if (node.closest("pre, code, .difflines")) return node.closest(".difflines") ? "diff" : "code";
  return "prose";
}

function annotate(root, tokens) {
  // The overview's tree emits its own links: a row shows a basename while the link needs the whole path, so the
  // pass has no literal to match on. They still want the same peek.
  for (const el of root.querySelectorAll("a.annot[data-path]")) {
    hoverPopover(el, () => peek(el));
  }
  if (!tokens || !tokens.length) return;
  const live = tokens.filter((t) => ANNOTATE[t.kind] !== false);
  if (!live.length) return;

  // Longest first, so `lib/render/markdown.py` wins over `markdown.py` at the same position.
  const byLength = [...live].sort((a, b) => b.text.length - a.text.length);
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const targets = [];
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    // `code.raw` is the author's opt-out: a span that looks like a reference and is not.
    if (node.parentElement.closest("a, mark, .annot, textarea, .comment-composer, code.raw")) continue;
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

// A term was matched whole-word on the Python side, and the literal it emitted carries no record of that.
// So `round` would be found inside `around` here, and — since a term is drawn once per block — the real
// occurrence further along would then get nothing. Worse than not matching at all, which is the outcome
// the glossary is explicitly trying to avoid.
//
// Paths and shas keep matching as substrings: a filename inside a longer run of word characters is a
// different situation, and the current behaviour there is right.
const WORD = /[A-Za-z0-9_]/;

function isWholeWord(text, at, length) {
  const before = at > 0 ? text[at - 1] : "";
  const after = at + length < text.length ? text[at + length] : "";
  return !WORD.test(before) && !WORD.test(after);
}

function wrapNode(node, tokens) {
  const text = node.nodeValue;
  const hits = [];
  for (const token of tokens) {
    let from = 0;
    for (;;) {
      const at = text.indexOf(token.text, from);
      if (at < 0) break;
      from = at + token.text.length;
      if (token.kind === "glossary" && !isWholeWord(text, at, token.text.length)) continue;
      // No nesting and no overlap: the first provider to claim a span owns it.
      if (!hits.some((h) => at < h.end && at + token.text.length > h.start)) {
        hits.push({ start: at, end: at + token.text.length, token });
      }
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
    hoverPopover(el, () => peek(el));
  }
  if (token.kind === "dir") {
    el.dataset.dir = token.path;
    hoverPopover(el, () => peekTree(el));
  }
  if (token.kind === "commit") {
    el.dataset.sha = token.text;
    hoverPopover(el, () => peekCommit(el));
  }
  if (token.note) {
    el.title = "";
    hoverPopover(el, () => showPopover(el, `<div class="pop-def">${mdInline(token.note)}</div>`));
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
//
// The box is driven by an OWNER rather than by a pair of enter/leave events, and that is the whole design.
// `popoverOwner` is the element the box belongs to, and every open re-checks it before painting.
//
// Three things go wrong without it, and all three were visible as a box that stuck to the screen:
//
//   - **A peek is asynchronous.** `mouseleave` schedules the close, the fetch resolves after it has run, and
//     the box reopens for an element the pointer left — now with no close scheduled behind it. Nothing but
//     hovering some other reference took it down again, which is exactly the report.
//   - **`mouseleave` is a hint, not the authority.** An element re-rendered under the cursor never sends one,
//     and a pointer leaving through a corner or out of the window can miss it too. A capture-phase
//     `pointermove` is what actually decides whether the pointer is still over the anchor or the box.
//   - **A sweep is not a hover.** Crossing a paragraph passes over a dozen references without meaning any of
//     them. Opening waits for the pointer to settle, and nothing is fetched until it has.
//
// The box is `position: fixed` and placed from a `getBoundingClientRect`, so any scroll leaves it pointing at
// nothing. It closes rather than trying to follow.

// Popover timing, in milliseconds. Change them here; nothing else reads a literal.
//
// Not exposed in the UI, deliberately — for now. They are a pair rather than two settings: `close` must stay
// above `open` (see below), so a control for one without the other would let a reader build a configuration
// that blinks. Worth exposing both together if it ever comes up.
const POPOVER_TIMING = {
  // Hover intent. Nothing opens and nothing is fetched until the pointer has settled this long on a
  // reference, so sweeping across a paragraph costs no requests and opens no boxes.
  open: 110,

  // How long you have to reach the box after leaving the anchor.
  //
  // **Armed once, when the pointer leaves, and never restarted by further movement.** Restarting it on each
  // move is the obvious reading of "close when the pointer is elsewhere" and it is wrong: a reader who keeps
  // the mouse moving never reaches the deadline, so the box stays up for as long as they keep moving.
  //
  // Must stay above `open`, so moving to a neighbouring reference hands the box over before this fires.
  close: 300,
};

let popoverOwner = null;
let openTimer = 0;
let closeTimer = 0;

function popover() {
  let box = document.getElementById("popover");
  if (!box) {
    box = document.createElement("div");
    box.id = "popover";
    box.hidden = true;
    box.addEventListener("mouseenter", cancelPopoverClose);
    box.addEventListener("mouseleave", schedulePopoverClose);
    document.body.appendChild(box);
  }
  return box;
}

// Wires one reference: hover intent in, ownership taken at the moment the box is actually wanted.
//
// `open` may be asynchronous and is only ever called once the pointer has settled, so a peek's fetch is work
// the reader asked for rather than work a sweep triggered.
function hoverPopover(el, open) {
  el.addEventListener("mouseenter", () => {
    clearTimeout(openTimer);
    openTimer = setTimeout(() => {
      openTimer = 0;
      popoverOwner = el;
      // The close armed by leaving the previous anchor is not about this one, and an `open` that fetches can
      // easily outlive it — which would close the box the reader is now pointing at.
      cancelPopoverClose();
      open();
    }, POPOVER_TIMING.open);
  });
  el.addEventListener("mouseleave", () => {
    clearTimeout(openTimer);
    openTimer = 0;
    schedulePopoverClose();
  });
}

// `beside` puts the box past the anchor's right edge instead of under it, and aligns its top with the anchor.
//
// A nav row is the case: opening downwards covers the rows below the one being hovered, so the list you are
// scanning disappears under the preview of the thing you were scanning it for.
function showPopover(anchor, html, { beside = false } = {}) {
  // An open that lost its anchor while it was fetching paints nothing, and says so.
  //
  // Strict equality rather than "not somebody else's": `closePopover` clears the owner, so a fetch that
  // resolves after the pointer left everything would otherwise still find a vacant box and fill it. That is
  // the same stuck box by a slower route, and it is the one this whole owner is for.
  if (popoverOwner !== anchor) return false;

  const box = popover();
  cancelPopoverClose();
  box.innerHTML = html;
  box.hidden = false;
  const rect = anchor.getBoundingClientRect();

  if (beside) {
    const room = window.innerWidth - rect.right - 16;
    box.style.maxWidth = Math.max(320, room) + "px";
    box.style.left = Math.min(rect.right + 10, window.innerWidth - box.offsetWidth - 8) + "px";
    box.style.top = Math.max(8, Math.min(rect.top, window.innerHeight - box.offsetHeight - 8)) + "px";
    return true;
  }

  box.style.maxWidth = "";
  box.style.left = Math.max(8, Math.min(rect.left, window.innerWidth - box.offsetWidth - 8)) + "px";
  const below = rect.bottom + 6;
  box.style.top = (below + box.offsetHeight > window.innerHeight ? rect.top - box.offsetHeight - 6 : below) + "px";
  return true;
}

function closePopover() {
  clearTimeout(openTimer);
  openTimer = 0;
  cancelPopoverClose();
  popoverOwner = null;
  const box = document.getElementById("popover");
  if (box) box.hidden = true;
}

// Every cancellation goes through here, because clearing the handle without zeroing it would leave the guard
// in `schedulePopoverClose` believing a close is still armed, and no later one would ever be.
function cancelPopoverClose() {
  clearTimeout(closeTimer);
  closeTimer = 0;
}

// Arms the deadline, or leaves the running one alone.
//
// The idempotence is the point rather than an optimization: the deadline runs from the moment the pointer
// LEFT the anchor, not from its last twitch, so a reader still moving is still on the clock.
function schedulePopoverClose() {
  if (closeTimer) return;
  closeTimer = setTimeout(closePopover, POPOVER_TIMING.close);
}

// The safety net, and what actually fixes a box that stuck.
//
// Capture phase, so it sees the move wherever it lands — inside a diff, inside the box, over an element that
// stops propagation. It only does work while a box is open.
document.addEventListener("pointermove", (event) => {
  const box = document.getElementById("popover");
  if (!box || box.hidden) return;
  if (box.contains(event.target)) {
    cancelPopoverClose();
    return;
  }
  // `contains` on an owner that a re-render detached is false for every live target, which is the case a
  // `mouseleave` never arrived for.
  if (popoverOwner && popoverOwner.isConnected && popoverOwner.contains(event.target)) {
    cancelPopoverClose();
    return;
  }
  schedulePopoverClose();
}, true);

// A fixed box placed from a rect is wrong the moment anything scrolls, and the pointer leaving the window
// never sends a move at all. Both close immediately rather than after the travel grace.
document.addEventListener("scroll", (event) => {
  const box = document.getElementById("popover");
  if (!box || box.hidden || box.contains(event.target)) return;
  closePopover();
}, true);
window.addEventListener("blur", closePopover);
document.addEventListener("pointerleave", closePopover);
document.addEventListener("keydown", (event) => { if (event.key === "Escape") closePopover(); });

// Line numbers are added here rather than server-side: the highlighter emits one blob, and a gutter is a
// reading aid rather than part of the content.
// Splitting the serialized HTML on newlines is safe because pygments closes its spans at every line end,
// so no element ever straddles the split.
// Joined with nothing rather than with a newline: `.src-line` is `display: block` and breaks the line itself,
// while a `pre` renders the newline between two blocks as a second break. That doubled every line's height,
// which reads exactly like vertical padding on the gutter and is not.
function withLineNumbers(html, start) {
  return html.split("\n")
    .map((text, index) => `<span class="src-line" data-line="${start + index}">` +
      `<span class="src-no">${start + index}</span>${text}</span>`)
    .join("");
}

// The whole file, every time, bounded and scrollable — rather than a window computed around the line.
//
// A window has to guess how much you wanted, and it guessed with a rule about comment blocks that was right
// often enough to be worth having and wrong often enough to be noticed. Scrolling costs the reader nothing
// and never hides the line above or below the one they asked about.
// Cached per path rather than per path-and-line, since there is now only one response per file.
const peekCache = new Map();

async function peek(el) {
  const path = el.dataset.path;
  const line = Number(el.dataset.line || 0);
  if (!peekCache.has(path)) {
    const result = await getJSON(`/api/file?path=${encodeURIComponent(path)}&line=0&whole=1`);
    if (!result.ok) return;
    peekCache.set(path, result.body);
  }
  const body = peekCache.get(path);
  // The fetch may have outlived the hover, in which case the box is someone else's and the scroll below
  // would drag it to a line from a file the reader is no longer pointing at.
  if (!showPopover(el, `<div class="pop-head">${_esc(body.path)}<span>${peekMeta(body, line)}</span></div>` + peekBody(body))) return;
  if (body.kind === "text") focusPopoverLine(line);
}

// What the head line says about the file, which is the only thing a reader gets before an image paints.
function peekMeta(body, line) {
  if (body.kind === "image") return [body.dimensions, body.size].filter(Boolean).join(" · ");
  if (body.kind === "binary") return body.size;
  return `${line ? `line ${line} of ` : ""}${body.lines} lines`;
}

// One viewer per kind of file.
//
// Everything here used to assume text, so a reference to a committed image was highlighted as a screen of
// replacement characters — slow, and telling the reader nothing about the picture they were pointing at.
// A binary that is not an image has nothing to draw, so it says what it is rather than pretending.
function peekBody(body) {
  if (body.kind === "image") {
    return `<div class="pop-image"><img src="${_esc(body.src)}" alt="${_esc(body.path)}" loading="lazy"></div>`;
  }
  if (body.kind === "binary") {
    return `<div class="pop-binary">binary file — nothing to show here</div>`;
  }
  return `<pre class="pg pop-scroll"><code>${withLineNumbers(body.html, body.start)}</code></pre>`;
}

// Top when no line was given, and the line itself when one was — a third of the way down, so the lines
// leading up to it are on screen too.
function focusPopoverLine(line) {
  const scroller = popover().querySelector(".pop-scroll");
  if (!scroller) return;
  if (!line) {
    scroller.scrollTop = 0;
    return;
  }
  const target = scroller.querySelector(`.src-line[data-line="${line}"]`);
  if (!target) return;
  target.classList.add("src-focus");
  scroller.scrollTop = Math.max(0, target.offsetTop - scroller.clientHeight / 3);
}

// The full title of a row the nav had to cut short.
//
// Nothing opens when the title fits: a popover repeating a line the reader can already read in full is noise,
// and the nav is where noise costs the most, since the pointer crosses every row on the way to one of them.
// `scrollWidth > clientWidth` is the browser's own answer to "did this ellipse", which beats measuring text.
function peekNavTitle(anchor, titleEl, row) {
  if (titleEl.scrollWidth <= titleEl.clientWidth) return;
  showPopover(anchor,
    `<div class="pop-navtitle"><span class="pop-navid">${_esc(row.id)}</span>${_esc(row.title)}</div>`,
    { beside: true });
}

// The whole entry, in the popover the nav row opens.
//
// Reading is what the nav is for, and until now the only way to see whether an entry was the one you meant
// was to navigate into it and lose your place. Non-interactive on purpose: an ask answered by accident from
// a hover is worse than no preview at all, so the copy is inert and the real one is a click away.
const entryPeekCache = new Map();

async function peekEntry(anchor, slug) {
  if (!entryPeekCache.has(slug)) {
    const result = await getJSON("/api/entry/" + encodeURIComponent(slug));
    if (!result.ok || result.body.broken) return;
    entryPeekCache.set(slug, result.body.html);
  }
  if (!showPopover(anchor,
    `<div class="pop-entry pop-scroll" aria-hidden="true">${entryPeekCache.get(slug)}</div>`,
    { beside: true })) return;
  const scroller = popover().querySelector(".pop-scroll");
  if (scroller) scroller.scrollTop = 0;
}

// A folder's popover: what is under it, as the same tree the overview draws.
// The full tree with the overview's own row cap, rather than one level — a folder reference is asking what is
// in here, and a listing that stops at the first directory answers that for almost no folder in this repo.
const treeCache = new Map();

async function peekTree(el) {
  const path = el.dataset.dir;
  if (!treeCache.has(path)) {
    const result = await getJSON("/api/tree?path=" + encodeURIComponent(path));
    if (!result.ok) return;
    treeCache.set(path, result.body);
  }
  const body = treeCache.get(path);
  showPopover(el,
    `<div class="pop-head">${_esc(body.path)}/<span>${body.files} files</span></div>` +
    `<div class="pop-scroll pop-tree">${body.html}</div>`);
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
