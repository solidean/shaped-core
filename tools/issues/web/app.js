// The issue tracker's web UI. Every edit is a POST that rewrites one issue file; nothing commits until "Commit & push".
"use strict";

const TYPES = ["epic", "bug", "feat", "perf", "debt", "chore", "question"];
const PRIORITY_RANK = { next: 0, normal: 1, someday: 2 };

const S = {
  data: null,
  byQid: new Map(),
  byPath: new Map(),
  pending: [],
  dirty: new Set(),
  markers: null,
  selected: null,
  checked: new Set(),
  view: "list",
  types: new Set(TYPES),
  version: null,
  cy: null,
};

const $ = (sel) => document.querySelector(sel);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
// A title is plain text but for `code` spans, which read badly as raw backticks.
const inline = (s) => esc(s).replace(/`([^`]+)`/g, "<code>$1</code>");
const vscode = (path, line) => `vscode://file/${encodeURI(path)}${line ? `:${line}` : ""}`;

function load(key, fallback) {
  try { return JSON.parse(localStorage.getItem(`issues.${key}`)) ?? fallback; } catch { return fallback; }
}
function save(key, value) {
  try { localStorage.setItem(`issues.${key}`, JSON.stringify(value)); } catch { /* storage unavailable */ }
}

// --- server ---------------------------------------------------------------------------------------------------------

async function api(path, body) {
  const init = body === undefined ? {} : {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Issues-Token": window.ISSUES_TOKEN },
    body: JSON.stringify(body),
  };
  const res = await fetch(path, init);
  const json = await res.json().catch(() => ({ error: `${res.status} ${res.statusText}` }));
  if (!res.ok) throw new Error(json.error || `${res.status}`);
  return json;
}

async function refresh() {
  const [data, pending] = await Promise.all([api("/api/issues"), api("/api/pending")]);
  S.data = data;
  S.version = data.version;
  S.byQid = new Map(data.issues.map((i) => [i.qid, i]));
  S.byPath = new Map(data.issues.map((i) => [i.path.toLowerCase(), i]));
  setPending(pending);
  $("#repo-name").textContent = data.repo;
  fillFilterOptions();
  render();
}

function setPending(pending) {
  S.pending = pending;
  S.dirty = new Set(pending.map((p) => p.path.toLowerCase()));
  const btn = $("#pending-btn");
  btn.hidden = pending.length === 0;
  btn.textContent = `${pending.length} outstanding edit${pending.length === 1 ? "" : "s"}`;
}

async function edit(qid, fields, notes) {
  try {
    await api("/api/edit", { qid, fields, notes });
    await refresh();
  } catch (e) {
    toast(e.message, true);
    await refresh();
  }
}

async function poll() {
  try {
    const st = await api("/api/status");
    if (st.version !== S.version) await refresh();
    else if (st.pending !== S.pending.length) setPending(await api("/api/pending")), render();
  } catch { /* the server is gone or restarting; try again next tick */ }
  setTimeout(poll, 2500);
}

function toast(text, error = false) {
  const t = $("#toast");
  t.textContent = text;
  t.className = error ? "error" : "";
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (t.hidden = true), error ? 9000 : 3000);
}

// --- filtering ------------------------------------------------------------------------------------------------------

function fillFilterOptions() {
  const issues = S.data.issues;
  const scopes = [...new Set(issues.map((i) => i.scope))].sort();
  const keepScope = $("#f-scope").value;
  $("#f-scope").innerHTML = `<option value="">all</option>` + scopes.map((s) => `<option>${esc(s)}</option>`).join("");
  $("#f-scope").value = scopes.includes(keepScope) ? keepScope : "";

  const tags = allTags();
  const keepTag = $("#f-tag").value;
  $("#f-tag").innerHTML = `<option value="">any</option>` + tags.map(([t]) => `<option>${esc(t)}</option>`).join("");
  $("#f-tag").value = tags.some(([t]) => t === keepTag) ? keepTag : "";
  $("#tag-list").innerHTML = tags.map(([t]) => `<option value="${esc(t)}">`).join("");
  $("#id-list").innerHTML = issues.filter((i) => !i.closed).map((i) => `<option value="${esc(i.qid)}">${esc(i.title)}</option>`).join("");

  $("#f-types").innerHTML = TYPES.map((t) =>
    `<button class="type-chip badge t-${t} ${S.types.has(t) ? "" : "off"}" data-type="${t}">${t}</button>`).join("");
}

function allTags() {
  const counts = new Map();
  for (const i of S.data.issues) for (const t of i.tags) {
    const c = counts.get(t) || { open: 0, all: 0 };
    c.all++;
    if (!i.closed) c.open++;
    counts.set(t, c);
  }
  return [...counts.entries()].sort((a, b) => a[0].localeCompare(b[0]));
}

function matches(i) {
  const state = $("#f-state").value;
  if (state === "open" && i.closed || state === "closed" && !i.closed) return false;
  if (!S.types.has(i.type)) return false;
  const scope = $("#f-scope").value;
  if (scope && i.scope !== scope) return false;
  const pr = $("#f-priority").value;
  if (pr && i.priority !== pr) return false;
  const tag = $("#f-tag").value;
  if (tag && !i.tags.includes(tag)) return false;
  const claim = $("#f-claim").value;
  if (claim === "claimed" && !i.claimed_by || claim === "unclaimed" && i.claimed_by) return false;
  if (claim === "mine" && !(i.claimed_by || "").toLowerCase().includes((S.data.user || "\0").toLowerCase())) return false;
  if ($("#f-blocked").checked && !i.blocked) return false;
  const q = $("#search").value.trim().toLowerCase();
  if (q && !`${i.qid}\n${i.title}\n${i.tags.join(" ")}\n${i.body}\n${i.notes}`.toLowerCase().includes(q)) return false;
  return true;
}

const byRank = (a, b) =>
  (PRIORITY_RANK[a.priority] ?? 1) - (PRIORITY_RANK[b.priority] ?? 1) ||
  TYPES.indexOf(a.type) - TYPES.indexOf(b.type) || a.id.localeCompare(b.id);

// --- rendering ------------------------------------------------------------------------------------------------------

function render() {
  if (!S.data) return;
  const shown = S.data.issues.filter(matches);
  $("#count").textContent = `${shown.length} of ${S.data.issues.length}`;
  for (const v of ["list", "graph", "tags"]) $(`#view-${v}`).hidden = S.view !== v;
  document.querySelectorAll("#views button").forEach((b) => b.classList.toggle("active", b.dataset.view === S.view));
  if (S.view === "list") renderList(shown);
  if (S.view === "graph") renderGraph(shown);
  if (S.view === "tags") renderTags();
  renderPanel();
  renderBulk();
}

function chipsFor(i) {
  const out = [];
  if (S.dirty.has(i.path.toLowerCase())) out.push(`<span class="chip dirty" title="edited, not committed">edited</span>`);
  if (i.problems.length) out.push(`<span class="chip problem" title="${esc(i.problems.join("\n"))}">check fails</span>`);
  if (i.priority !== "normal") out.push(`<span class="chip ${i.priority}">${i.priority}</span>`);
  if (i.blocked) out.push(`<span class="chip blocked">blocked</span>`);
  if (i.claimed_by) out.push(`<span class="chip claim">@${esc(i.claimed_by)}</span>`);
  for (const t of i.tags) out.push(`<span class="chip">${esc(t)}</span>`);
  if (i.closed) out.push(`<span class="chip">${esc(i.resolution || "closed")} ${esc(i.closed_date || "")}</span>`);
  return out.join("");
}

function renderList(shown) {
  const listed = new Set(shown.map((i) => i.qid));
  const children = new Map();
  const roots = [];
  for (const i of shown) {
    if (i.parent_qid && listed.has(i.parent_qid) && i.parent_qid !== i.qid) {
      if (!children.has(i.parent_qid)) children.set(i.parent_qid, []);
      children.get(i.parent_qid).push(i);
    } else roots.push(i);
  }
  const groups = new Map();
  for (const i of roots) {
    const key = i.repo === S.data.repo ? i.scope : `${i.repo}: ${i.scope}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(i);
  }
  const html = [];
  const row = (i, depth) => {
    html.push(`<div class="row ${i.closed ? "closed" : ""} ${S.selected === i.qid ? "selected" : ""}" data-qid="${esc(i.qid)}"
      style="padding-left:${16 + depth * 22}px">
      <input type="checkbox" class="pick" ${S.checked.has(i.qid) ? "checked" : ""}>
      <span class="badge t-${i.type}">${i.type}</span>
      <span class="title">${inline(i.title)}</span>
      <span class="chips">${chipsFor(i)}</span>
      <span class="id">${esc(i.qid)}</span></div>`);
    for (const c of (children.get(i.qid) || []).sort(byRank)) row(c, depth + 1);
  };
  for (const key of [...groups.keys()].sort()) {
    const items = groups.get(key).sort(byRank);
    html.push(`<div class="scope-head">${esc(key)} · ${items.length}</div>`);
    for (const i of items) row(i, 0);
  }
  $("#view-list").innerHTML = html.join("") || `<p class="graph-empty">No issue matches.</p>`;
}

function renderTags() {
  const rows = allTags().map(([t, c]) => `<tr>
    <td><a href="#" class="tag-filter" data-tag="${esc(t)}">${esc(t)}</a></td><td>${c.open}</td><td>${c.all}</td>
    <td><button class="tag-rename" data-tag="${esc(t)}">Rename…</button> <button class="tag-drop quiet" data-tag="${esc(t)}">Remove</button></td></tr>`);
  $("#view-tags").innerHTML = `<table class="tags-table"><tr><th>Tag</th><th>Open</th><th>All</th><th></th></tr>${rows.join("")}</table>`;
}

// --- graph ----------------------------------------------------------------------------------------------------------

function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function renderGraph(shown) {
  const host = $("#graph");
  if (!window.cytoscape) {
    host.innerHTML = `<p class="graph-empty">The graph library did not load; it comes from cdn.jsdelivr.net.</p>`;
    return;
  }
  const withParent = $("#g-parent").checked;
  const outside = $("#g-outside").checked;
  const inFilter = new Set(shown.map((i) => i.qid));
  const edges = [];
  for (const i of S.data.issues) {
    for (const b of i.blocked_by_qids) edges.push({ source: b, target: i.qid, kind: "block" });
    if (withParent && i.parent_qid) edges.push({ source: i.parent_qid, target: i.qid, kind: "parent" });
  }
  const used = edges.filter((e) => outside
    ? inFilter.has(e.source) || inFilter.has(e.target)
    : inFilter.has(e.source) && inFilter.has(e.target));
  let nodes = new Set(used.flatMap((e) => [e.source, e.target]));
  if ($("#g-focus").checked && S.selected && nodes.has(S.selected)) nodes = component(S.selected, used);
  const key = JSON.stringify([...nodes].sort()) + withParent;
  if (S.cy && S.graphKey === key) {
    S.cy.nodes().removeClass("sel");
    if (S.selected) S.cy.getElementById(S.selected).addClass("sel");
    return;
  }
  S.graphKey = key;
  if (!nodes.size) {
    if (S.cy) S.cy.destroy(), (S.cy = null);
    host.innerHTML = `<p class="graph-empty">None of the shown issues blocks, or is blocked by, another${withParent ? ", or belongs to an epic" : ""}.</p>`;
    return;
  }
  host.innerHTML = "";
  const short = (t) => (t.length > 46 ? `${t.slice(0, 44)}…` : t);
  const elements = [
    ...[...nodes].map((q) => {
      const i = S.byQid.get(q);
      return { data: { id: q, label: `${i.id}\n${short(i.title)}`, type: i.type },
        classes: [i.closed ? "closed" : "", i.blocked ? "blocked" : "", inFilter.has(q) ? "" : "outside", S.selected === q ? "sel" : ""].join(" ") };
    }),
    ...used.filter((e) => nodes.has(e.source) && nodes.has(e.target))
      .map((e, n) => ({ data: { id: `e${n}`, source: e.source, target: e.target }, classes: e.kind })),
  ];
  if (S.cy) S.cy.destroy();
  S.cy = cytoscape({
    container: host,
    elements,
    wheelSensitivity: 0.3,
    style: [
      { selector: "node", style: {
        shape: "round-rectangle", label: "data(label)", "text-wrap": "wrap", "text-valign": "center", "font-size": 10,
        width: "label", height: "label", padding: "8px", color: css("--text"), "background-color": css("--surface"),
        "border-width": 2, "border-color": css("--muted") } },
      ...TYPES.map((t) => ({ selector: `node[type = "${t}"]`, style: { "border-color": css(`--t-${t}`) } })),
      { selector: "node.closed", style: { opacity: 0.45 } },
      { selector: "node.outside", style: { "border-style": "dashed", opacity: 0.6 } },
      { selector: "node.blocked", style: { "background-color": css("--danger-soft") } },
      { selector: "node.sel", style: { "border-width": 4, "background-color": css("--accent-soft") } },
      { selector: "edge", style: { width: 1.5, "curve-style": "bezier", "target-arrow-shape": "triangle", "arrow-scale": 0.9 } },
      { selector: "edge.block", style: { "line-color": css("--danger"), "target-arrow-color": css("--danger") } },
      { selector: "edge.parent", style: { "line-color": css("--muted"), "target-arrow-color": css("--muted"), "line-style": "dashed" } },
    ],
    layout: { name: "preset" },
  });
  packComponents(S.cy, host);
  S.cy.on("tap", "node", (ev) => select(ev.target.id()));
}

function component(start, edges) {
  const seen = new Set([start]);
  for (let grew = true; grew;) {
    grew = false;
    for (const e of edges) {
      if (seen.has(e.source) !== seen.has(e.target)) seen.add(e.source), seen.add(e.target), (grew = true);
    }
  }
  return seen;
}

// One dagre layout per connected cluster, the clusters then shelf-packed to the view's aspect, largest first:
// a single layout over all of them stacks every small cluster into one unreadable column.
function packComponents(cy, host) {
  const layout = window.cytoscapeDagre ? { name: "dagre", rankDir: "TB", nodeSep: 14, rankSep: 50 } : { name: "breadthfirst", directed: true };
  const comps = cy.elements().components().sort((a, b) => b.length - a.length);
  const gap = 60;
  let area = 0;
  for (const c of comps) {
    c.layout({ ...layout, fit: false, animate: false }).run();
    const bb = c.boundingBox();
    area += (bb.w + gap) * (bb.h + gap);
  }
  const width = Math.max(comps[0].boundingBox().w, Math.sqrt(area * (host.clientWidth / Math.max(1, host.clientHeight))));
  let x = 0, y = 0, rowHeight = 0;
  for (const c of comps) {
    const bb = c.boundingBox();
    if (x > 0 && x + bb.w > width) x = 0, (y += rowHeight + gap), (rowHeight = 0);
    c.nodes().shift({ x: x - bb.x1, y: y - bb.y1 });
    x += bb.w + gap;
    rowHeight = Math.max(rowHeight, bb.h);
  }
  cy.fit(undefined, 30);
}

// --- panel ----------------------------------------------------------------------------------------------------------

function issueLink(qid) {
  const i = S.byQid.get(qid);
  if (!i) return `<span class="chip problem">${esc(qid)}</span>`;
  return `<span class="issue-link ${i.closed ? "closed" : ""}" data-qid="${esc(qid)}" title="${esc(i.title)}">${esc(qid)}</span>`;
}

function chipEditor(field, values, list, render = esc) {
  return `<div class="chips-edit" data-field="${field}">
    ${values.map((v) => `<span class="chip">${render(v)}<button data-remove="${esc(v)}" title="remove">×</button></span>`).join("")}
    <input list="${list}" placeholder="add…" data-add="${field}"></div>`;
}

function renderMarkdown(md, issue) {
  const html = window.marked && window.DOMPurify
    ? DOMPurify.sanitize(marked.parse(md || ""))
    : `<pre>${esc(md)}</pre>`;
  const box = document.createElement("div");
  box.innerHTML = html;
  const dir = issue.path.slice(0, issue.path.lastIndexOf("/"));
  for (const a of box.querySelectorAll("a[href]")) {
    const href = a.getAttribute("href");
    if (/^[a-z][a-z0-9+.-]*:/i.test(href) || href.startsWith("#")) {
      a.target = "_blank";
      continue;
    }
    const [target, anchor] = href.split("#");
    const abs = resolvePath(dir, decodeURI(target));
    const linked = S.byPath.get(abs.toLowerCase());
    if (linked) {
      a.removeAttribute("href");
      a.className = "issue-link";
      a.dataset.qid = linked.qid;
    } else {
      const line = /^L(\d+)/.exec(anchor || "");
      a.href = vscode(abs, line && line[1]);
    }
  }
  return box.innerHTML;
}

function resolvePath(dir, rel) {
  const parts = dir.split("/");
  for (const p of rel.split("/")) {
    if (p === "..") parts.pop();
    else if (p && p !== ".") parts.push(p);
  }
  return parts.join("/");
}

function renderPanel() {
  const panel = $("#panel");
  const i = S.selected && S.byQid.get(S.selected);
  panel.hidden = !i;
  $("#splitter").hidden = !i;
  if (!i) return;
  const epics = S.data.issues.filter((e) => e.type === "epic" && e.repo === i.repo && e.qid !== i.qid);
  const blocks = S.data.issues.filter((o) => o.blocked_by_qids.includes(i.qid));
  const kids = S.data.issues.filter((o) => o.parent_qid === i.qid);
  const done = kids.filter((k) => k.closed).length;
  const markers = S.markers ? S.markers[i.qid] || [] : null;
  const editingNotes = panel.dataset.editing === i.qid;

  panel.innerHTML = `
    <div class="panel-head">
      <span class="badge t-${i.type}">${i.type}</span><span class="id">${esc(i.qid)}</span>
      ${S.dirty.has(i.path.toLowerCase()) ? `<span class="chip dirty">edited</span>` : ""}
      <span class="spacer"></span>
      <button class="quiet" id="panel-close" title="close (Esc)">✕</button>
    </div>
    <h2>${inline(i.title)}</h2>
    <a class="path" href="${vscode(i.path)}" title="open in VS Code">${esc(i.rel)}</a>
    ${i.problems.length ? `<div class="problems"><b>issues check</b><ul>${i.problems.map((p) => `<li>${esc(p)}</li>`).join("")}</ul></div>` : ""}
    <div class="fields">
      <span class="k">Priority</span>
      <span><select data-set="priority">${["next", "normal", "someday"].map((p) => `<option ${p === i.priority ? "selected" : ""}>${p}</option>`).join("")}</select></span>
      <span class="k">Tags</span>${chipEditor("tags", i.tags, "tag-list")}
      ${i.type === "epic" ? "" : `<span class="k">Epic</span><span><select data-set="parent">
        <option value="">—</option>${epics.map((e) => `<option value="${esc(e.id)}" ${e.qid === i.parent_qid ? "selected" : ""}>${esc(e.qid)} · ${esc(e.title)}</option>`).join("")}
      </select></span>`}
      <span class="k">Blocked by</span>${chipEditor("blocked-by", i.blocked_by, "id-list",
        (v) => issueLink(S.byQid.has(v) || i.repo === S.data.repo ? v : `${i.repo}:${v}`))}
      ${blocks.length ? `<span class="k">Blocks</span><span class="link-list">${blocks.map((b) => issueLink(b.qid)).join("")}</span>` : ""}
      ${kids.length ? `<span class="k">Children</span><span><span class="progress">${done} of ${kids.length} closed</span>
        <span class="link-list">${kids.sort(byRank).map((k) => issueLink(k.qid)).join("")}</span></span>` : ""}
      <span class="k">Claimed</span><span>${i.claimed_by
        ? `${esc(i.claimed_by)} <span class="progress">${esc(i.claimed_at || "")}</span> <button id="release">Release</button>`
        : i.closed ? "—" : `<button id="claim">Claim as ${esc(S.data.user || "me")}</button>`}</span>
      <span class="k">Created</span><span>${esc(i.created || "—")}${i.closed ? ` · closed ${esc(i.closed_date || "")} as ${esc(i.resolution || "?")}` : ""}</span>
    </div>
    <div class="markdown">${renderMarkdown(i.body, i)}</div>
    <div class="section-title">Notes</div>
    <div class="notes">${editingNotes
      ? `<textarea id="notes-text">${esc(i.notes)}</textarea>
         <div class="notes-actions"><button id="notes-cancel" class="quiet">Cancel</button><button id="notes-save" class="primary">Save notes</button></div>`
      : `${i.notes ? `<div class="markdown">${renderMarkdown(i.notes, i)}</div>` : `<span class="empty">No notes.</span>`}
         <div class="notes-actions"><button id="notes-edit">${i.notes ? "Edit notes" : "Add notes"}</button></div>`}
    </div>
    ${i.refs.length ? `<div class="section-title">Refs</div><ul class="refs">${i.refs.map(([k, v]) => `<li>${k}: ${refLink(k, v, i)}</li>`).join("")}</ul>` : ""}
    <div class="section-title">Markers in code</div>
    ${markers === null ? `<p class="progress">scanning…</p>`
      : markers.length ? `<ul class="refs">${markers.map((m) => `<li><a href="${vscode(m.path, m.line)}">${esc(m.rel)}:${m.line}</a></li>`).join("")}</ul>`
      : `<p class="progress">none</p>`}
  `;
  if (S.markers === null) loadMarkers();
  if (editingNotes) $("#notes-text").focus();
}

function refLink(kind, value, issue) {
  if (kind === "issue") return issueLink(S.byQid.has(value) ? value : (issue.repo === S.data.repo ? value : `${issue.repo}:${value}`));
  if (kind === "file") {
    const m = /^(.*?):(\d+)(?:-\d+)?$/.exec(value);
    const root = issue.path.slice(0, issue.path.length - issue.rel.length - 1);
    return `<a href="${vscode(`${root}/${m ? m[1] : value}`, m && m[2])}">${esc(value)}</a>`;
  }
  return `<code>${esc(value)}</code>`;
}

let markersLoading = false;
async function loadMarkers() {
  if (markersLoading) return;
  markersLoading = true;
  try {
    S.markers = await api("/api/markers");
    renderPanel();
  } catch (e) {
    toast(e.message, true);
  } finally {
    markersLoading = false;
  }
}

function select(qid) {
  S.selected = qid;
  history.replaceState(null, "", qid ? `#${encodeURIComponent(qid)}` : location.pathname);
  $("#panel").dataset.editing = "";
  $("#panel").scrollTop = 0;
  render();
}

// --- bulk -----------------------------------------------------------------------------------------------------------

function renderBulk() {
  for (const q of [...S.checked]) if (!S.byQid.has(q)) S.checked.delete(q);
  $("#bulk-bar").hidden = S.checked.size === 0;
  $("#bulk-count").textContent = `${S.checked.size} selected`;
}

async function bulk(body) {
  try {
    await api("/api/bulk", { qids: [...S.checked], ...body });
  } catch (e) {
    toast(e.message, true);
  }
  await refresh();
}

// --- pending edits --------------------------------------------------------------------------------------------------

async function openPending() {
  setPending(await api("/api/pending"));
  $("#publish-log").hidden = true;
  $("#pending-diff").innerHTML = "";
  $("#pending-list").innerHTML = S.pending.map((p, n) => `<div class="pending-row">
    <input type="checkbox" data-n="${n}" ${p.touched ? "checked" : ""}>
    <span class="st">${esc(p.status)}</span>
    <span class="rel" data-n="${n}">${esc(p.repo === S.data.repo ? p.rel : `${p.repo}: ${p.rel}`)}</span>
    ${p.touched ? "" : `<span class="outside">changed outside this page</span>`}</div>`).join("") || `<p class="hint">Nothing outstanding.</p>`;
  $("#pending-dialog").showModal();
}

async function showDiff(n) {
  const { diff } = await api(`/api/diff?path=${encodeURIComponent(S.pending[n].path)}`);
  $("#pending-diff").innerHTML = diff.split("\n").map((l) =>
    `<span class="${l.startsWith("+") && !l.startsWith("+++") ? "add" : l.startsWith("-") && !l.startsWith("---") ? "del" : ""}">${esc(l)}</span>`).join("\n");
}

async function publish() {
  const paths = [...document.querySelectorAll("#pending-list input:checked")].map((c) => S.pending[+c.dataset.n].path);
  if (!paths.length) return toast("Check the files to commit.", true);
  const btn = $("#publish-btn");
  btn.disabled = true;
  btn.textContent = "Pushing…";
  try {
    const { log } = await api("/api/publish", { paths, message: $("#commit-message").value });
    $("#publish-log").textContent = log;
    $("#publish-log").hidden = false;
    toast(`Pushed ${paths.length} file${paths.length === 1 ? "" : "s"}.`);
    await refresh();
    if (!S.pending.length) $("#pending-dialog").close();
    else openPending();
  } catch (e) {
    $("#publish-log").textContent = e.message;
    $("#publish-log").hidden = false;
  } finally {
    btn.disabled = false;
    btn.textContent = "Commit & push";
  }
}

// --- events ---------------------------------------------------------------------------------------------------------

function wire() {
  for (const id of ["f-scope", "f-state", "f-priority", "f-tag", "f-claim", "f-blocked", "g-parent", "g-outside", "g-focus"]) {
    $(`#${id}`).addEventListener("change", () => (saveFilters(), render()));
  }
  $("#search").addEventListener("input", () => (saveFilters(), render()));
  $("#f-types").addEventListener("click", (ev) => {
    const t = ev.target.dataset.type;
    if (!t) return;
    // A plain click shows that type alone, or all again; a ctrl-click toggles it.
    if (ev.ctrlKey || ev.metaKey) S.types.has(t) ? S.types.delete(t) : S.types.add(t);
    else S.types = S.types.size === 1 && S.types.has(t) ? new Set(TYPES) : new Set([t]);
    saveFilters();
    fillFilterOptions();
    render();
  });
  $("#views").addEventListener("click", (ev) => {
    if (!ev.target.dataset.view) return;
    S.view = ev.target.dataset.view;
    save("view", S.view);
    render();
  });

  $("#view-list").addEventListener("click", (ev) => {
    const row = ev.target.closest(".row");
    if (!row) return;
    if (ev.target.classList.contains("pick")) {
      ev.target.checked ? S.checked.add(row.dataset.qid) : S.checked.delete(row.dataset.qid);
      renderBulk();
      return;
    }
    select(row.dataset.qid);
  });

  $("#view-tags").addEventListener("click", async (ev) => {
    const tag = ev.target.dataset.tag;
    if (!tag) return;
    ev.preventDefault();
    if (ev.target.classList.contains("tag-filter")) {
      $("#f-tag").value = tag;
      S.view = "list";
      render();
      return;
    }
    const drop = ev.target.classList.contains("tag-drop");
    const to = drop ? "" : prompt(`Rename the tag "${tag}" on every issue carrying it to:`, tag);
    if (to === null || to === tag || drop && !confirm(`Remove the tag "${tag}" from every issue carrying it?`)) return;
    try {
      const { renamed } = await api("/api/rename-tag", { from: tag, to });
      toast(`${drop ? "Removed from" : "Renamed on"} ${renamed} issue${renamed === 1 ? "" : "s"}.`);
    } catch (e) {
      toast(e.message, true);
    }
    await refresh();
  });

  const panel = $("#panel");
  document.body.addEventListener("click", (ev) => {
    const link = ev.target.closest(".issue-link");
    if (link && link.dataset.qid) {
      ev.preventDefault();
      select(link.dataset.qid);
    }
  });
  panel.addEventListener("change", (ev) => {
    const key = ev.target.dataset.set;
    if (key) edit(S.selected, { [key]: ev.target.value });
  });
  panel.addEventListener("click", (ev) => {
    const i = S.byQid.get(S.selected);
    const t = ev.target;
    if (t.id === "panel-close") return select(null);
    if (t.id === "claim") return edit(i.qid, { "claimed-by": S.data.user });
    if (t.id === "release") return edit(i.qid, { "claimed-by": "" });
    if (t.id === "notes-edit") return (panel.dataset.editing = i.qid), renderPanel();
    if (t.id === "notes-cancel") return (panel.dataset.editing = ""), renderPanel();
    if (t.id === "notes-save") {
      const notes = $("#notes-text").value;
      panel.dataset.editing = "";
      return edit(i.qid, {}, notes);
    }
    if (t.dataset.remove !== undefined) {
      const field = t.closest(".chips-edit").dataset.field;
      const values = field === "tags" ? i.tags : i.blocked_by;
      return edit(i.qid, { [field]: values.filter((v) => v !== t.dataset.remove) });
    }
  });
  panel.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter" && ev.target.dataset.add) {
      const i = S.byQid.get(S.selected);
      const field = ev.target.dataset.add;
      const value = ev.target.value.trim();
      if (!value) return;
      const values = field === "tags" ? i.tags : i.blocked_by;
      edit(i.qid, { [field]: [...values, value] });
    }
    if (ev.key === "Enter" && (ev.ctrlKey || ev.metaKey) && ev.target.id === "notes-text") $("#notes-save").click();
  });

  $("#bulk-add").addEventListener("click", () => $("#bulk-tag").value.trim() && bulk({ add_tags: [$("#bulk-tag").value.trim()] }));
  $("#bulk-remove").addEventListener("click", () => $("#bulk-tag").value.trim() && bulk({ remove_tags: [$("#bulk-tag").value.trim()] }));
  $("#bulk-priority").addEventListener("change", (ev) => {
    if (ev.target.value) bulk({ priority: ev.target.value });
    ev.target.value = "";
  });
  $("#bulk-clear").addEventListener("click", () => (S.checked.clear(), render()));

  $("#pending-btn").addEventListener("click", openPending);
  $("#pending-list").addEventListener("click", (ev) => {
    if (ev.target.classList.contains("rel")) showDiff(+ev.target.dataset.n);
  });
  $("#publish-btn").addEventListener("click", publish);

  document.addEventListener("keydown", (ev) => {
    const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
    if (ev.key === "/" && !typing) {
      ev.preventDefault();
      $("#search").focus();
    } else if (ev.key === "Escape" && !$("#pending-dialog").open) {
      if (typing) document.activeElement.blur();
      else select(null);
    } else if ((ev.key === "j" || ev.key === "k") && !typing && S.view === "list") {
      const rows = [...document.querySelectorAll("#view-list .row")].map((r) => r.dataset.qid);
      const at = rows.indexOf(S.selected);
      const next = rows[Math.max(0, Math.min(rows.length - 1, at + (ev.key === "j" ? 1 : -1)))];
      if (next) {
        select(next);
        document.querySelector(`#view-list .row.selected`)?.scrollIntoView({ block: "nearest" });
      }
    }
  });

  const splitter = $("#splitter");
  splitter.addEventListener("pointerdown", (ev) => {
    splitter.setPointerCapture(ev.pointerId);
    const move = (e) => {
      const width = Math.max(320, Math.min(window.innerWidth - 300, window.innerWidth - e.clientX));
      panel.style.width = `${width}px`;
    };
    splitter.addEventListener("pointermove", move);
    splitter.addEventListener("pointerup", () => {
      splitter.removeEventListener("pointermove", move);
      save("panelWidth", panel.style.width);
      if (S.cy) S.cy.resize();
    }, { once: true });
  });
}

const FILTERS = ["f-scope", "f-state", "f-priority", "f-tag", "f-claim", "search"];
function saveFilters() {
  save("filters", Object.fromEntries(FILTERS.map((id) => [id, $(`#${id}`).value])));
  save("blocked", $("#f-blocked").checked);
  save("types", [...S.types]);
}
function restoreFilters() {
  const f = load("filters", {});
  for (const id of FILTERS) if (f[id] !== undefined) $(`#${id}`).value = f[id];
  $("#f-blocked").checked = load("blocked", false);
  S.types = new Set(load("types", TYPES));
  S.view = new URLSearchParams(location.search).get("view") || load("view", "list");
  const width = load("panelWidth", null);
  if (width) $("#panel").style.width = width;
}

async function main() {
  if (window.cytoscape && window.cytoscapeDagre) {
    try { cytoscape.use(cytoscapeDagre); } catch { /* registered already */ }
  }
  wire();
  restoreFilters();
  const pendingFilters = load("filters", {});
  await refresh();
  // The scope and tag options exist only once the issues are in.
  for (const id of ["f-scope", "f-tag"]) if (pendingFilters[id]) $(`#${id}`).value = pendingFilters[id];
  const linked = decodeURIComponent(location.hash.slice(1));
  if (S.byQid.has(linked)) S.selected = linked;
  render();
  setTimeout(poll, 2500);
}

main().catch((e) => toast(e.message, true));
