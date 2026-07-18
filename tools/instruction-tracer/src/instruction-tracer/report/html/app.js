// instruction-tracer HTML export — renderer.
//
// Renders the embedded TRACE_DATA (see report/html_export.cc for the schema). Vanilla JS, no
// framework, no external requests. All DOM is built with createElement + textContent — never
// innerHTML with data — since symbols and source text can contain '<' and '&'.
(function () {
  "use strict";

  const D = TRACE_DATA;
  const body = document.body;

  // ---- small DOM helpers -------------------------------------------------

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function span(cls, text) { return el("span", cls, text); }
  function frag() { return document.createDocumentFragment(); }

  // ---- address helpers (addresses arrive as "hi`lo" hex strings) ---------

  function parseAddr(s) { return BigInt("0x" + s.replace(/`/g, "")); }
  function fmtAddr(n) {
    const h = n.toString(16).padStart(16, "0");
    return h.slice(0, 8) + "`" + h.slice(8);
  }
  function popcount(n) {
    let c = 0;
    while (n > 0n) { c += Number(n & 1n); n >>= 1n; }
    return c;
  }
  function baseName(p) {
    const i = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"));
    return i >= 0 ? p.slice(i + 1) : p;
  }
  // "instructions" is too wide for the memory columns; every other region name is already short.
  function regionLabel(r) { return r === "instructions" ? "instr" : r; }

  // Light highlighter for disassembly operands: registers/identifiers and number literals get the
  // same colors the source view uses; everything else stays default. `text` is the operand tail
  // (the mnemonic is coloured separately).
  const ASM_TOKEN = /([A-Za-z_.$][A-Za-z0-9_.$]*)|(0[xX][0-9a-fA-F]+|\d+)|([^A-Za-z0-9_.$]+)/g;
  function highlightAsm(parent, text) {
    let m;
    ASM_TOKEN.lastIndex = 0;
    while ((m = ASM_TOKEN.exec(text)) !== null) {
      if (m[1]) parent.appendChild(span("tok-ident", m[1]));
      else if (m[2]) parent.appendChild(span("tok-number", m[2]));
      else parent.appendChild(span("operands", m[3]));
    }
  }

  // ---- region filter state (drives inline memory + the 3 aggregate views) ----

  const regionState = {
    heap: D.meta.regions.heap,
    stack: D.meta.regions.stack,
    frame: D.meta.regions.frame,
    instructions: D.meta.regions.instructions,
  };
  const memoryRebuilders = []; // () => void, one per trace panel; called on a region change

  function applyRegionClasses() {
    for (const r of ["heap", "stack", "frame", "instructions"])
      body.classList.toggle("rgn-" + r, regionState[r]);
  }

  // ---- felixcloutier.com mnemonic links ----------------------------------

  const JCC = new Set([
    "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle", "jna", "jnae", "jnb", "jnbe",
    "jnc", "jne", "jng", "jnge", "jnl", "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo",
    "js", "jz",
  ]);
  function felixUrl(mn) {
    mn = mn.toLowerCase();
    if (JCC.has(mn)) return "https://www.felixcloutier.com/x86/jcc";
    if (mn.startsWith("set") && mn.length > 3) return "https://www.felixcloutier.com/x86/setcc";
    if (mn.startsWith("cmov")) return "https://www.felixcloutier.com/x86/cmovcc";
    if (mn.startsWith("loop")) return "https://www.felixcloutier.com/x86/loop:loopcc";
    return "https://www.felixcloutier.com/x86/" + encodeURIComponent(mn);
  }

  // ---- header ------------------------------------------------------------

  function renderHeader() {
    const m = D.meta;
    const header = el("header");
    header.appendChild(el("h1", null, "instruction trace — " + (m.target || "")));

    const grid = el("div", "meta-grid");
    const row = (k, v) => {
      if (v == null || v === "") return;
      grid.appendChild(span("meta-k", k));
      grid.appendChild(span("meta-v", String(v)));
    };
    const sizeKb = m.exeSizeBytes ? (Number(m.exeSizeBytes) / 1024).toFixed(1) + " kB" : "";
    row("generated", m.generatedAt);
    row("os", m.osVersion);
    row("binary", m.exePath + (sizeKb ? "  (" + sizeKb + ")" : ""));
    row("target", m.target);
    row("traces", m.traces);
    row("skip", m.skip);
    row("instructions", m.instructions);
    row("until-return", m.untilReturn ? "on" : "off");
    row("stop-at-syscall", m.stopAtSyscall ? "on" : "off");
    header.appendChild(grid);

    if (m.commandLine) {
      const cl = el("div", "cmdline");
      cl.appendChild(span("cmdline-label", "command line"));
      cl.appendChild(el("code", null, m.commandLine));
      header.appendChild(cl);
    }
    return header;
  }

  // ---- toggle + region controls ------------------------------------------

  function renderControls() {
    const bar = el("div", "controls");

    const toggle = (label, cls, initial) => {
      body.classList.toggle(cls, initial);
      const lbl = el("label", "toggle");
      const cb = el("input");
      cb.type = "checkbox";
      cb.checked = initial;
      cb.addEventListener("change", () => body.classList.toggle(cls, cb.checked));
      lbl.appendChild(cb);
      lbl.appendChild(span(null, label));
      bar.appendChild(lbl);
    };
    toggle("source", "show-source", true);
    toggle("registers", "show-registers", false);
    toggle("memory", "show-memory", false);

    bar.appendChild(span("controls-sep", "regions"));
    for (const r of ["heap", "stack", "frame", "instructions"]) {
      const lbl = el("label", "toggle");
      const cb = el("input");
      cb.type = "checkbox";
      cb.checked = regionState[r];
      cb.addEventListener("change", () => {
        regionState[r] = cb.checked;
        applyRegionClasses();
        for (const rebuild of memoryRebuilders) rebuild();
      });
      lbl.appendChild(cb);
      lbl.appendChild(span("rgn-dot rgn-dot-" + r, ""));
      lbl.appendChild(span(null, r));
      bar.appendChild(lbl);
    }
    return bar;
  }

  // ---- tabs --------------------------------------------------------------

  function renderTabs(panels) {
    const bar = el("div", "tabbar");
    bar.setAttribute("role", "tablist");
    const buttons = [];
    const select = (i) => {
      panels.forEach((p, j) => p.classList.toggle("active", i === j));
      buttons.forEach((b, j) => b.setAttribute("aria-selected", String(i === j)));
    };
    D.traces.forEach((t, i) => {
      const b = el("button", "tab", "trace " + t.index + "/" + t.total);
      b.setAttribute("role", "tab");
      b.addEventListener("click", () => select(i));
      buttons.push(b);
      bar.appendChild(b);
    });
    select(0);
    return bar;
  }

  // ---- left column: the trace --------------------------------------------

  // (fileId + ":" + line) -> raw source text, so an inline heading can show the mapped line.
  function sourceIndex(trace) {
    const idx = new Map();
    for (const f of trace.source.files)
      for (const r of f.ranges)
        for (const ln of r.lines)
          idx.set(f.fileId + ":" + ln.number, ln.text);
    return idx;
  }

  function renderInstruction(insn) {
    const row = el("div", "insn");
    if (insn.fileId >= 0 && insn.line > 0) row.dataset.loc = insn.fileId + ":" + insn.line;
    if (insn.isAtomic) row.classList.add("atomic");

    row.appendChild(span("addr", insn.addr));

    const code = span("code");
    if (insn.mnemonic) {
      const a = el("a", "mnemonic", insn.mnemonic);
      a.href = felixUrl(insn.mnemonic);
      a.target = "_blank";
      a.rel = "noopener";
      code.appendChild(a);
      highlightAsm(code, insn.text.slice(insn.mnemonic.length));
    } else {
      code.appendChild(span("operands", insn.text));
    }
    row.appendChild(code);

    // Branch annotation, mirroring the terminal's semantics.
    const note = branchNote(insn);
    if (note) row.appendChild(note);

    // Inline register diff (shown under the registers toggle).
    if (insn.regdiff && insn.regdiff.length) {
      const rd = span("regdiff");
      rd.appendChild(span(null, "  ; "));
      rd.appendChild(span("regdiff-body", insn.regdiff.map((d) => d.name + "=" + d.value).join(" ")));
      row.appendChild(rd);
    }

    // Inline memory accesses (shown under the memory toggle, filtered live by region).
    for (const acc of insn.mem) {
      const mr = el("div", "mem-row");
      mr.dataset.region = acc.region;
      mr.appendChild(span("mem-addr", acc.addr));
      mr.appendChild(span("mem-size", String(acc.size)));
      mr.appendChild(span("mem-rw", (acc.isRead ? "R" : "") + (acc.isWrite ? "W" : "")));
      mr.appendChild(span("mem-region rgn-fg-" + acc.region, regionLabel(acc.region)));
      if (acc.symbol) mr.appendChild(span("mem-sym", acc.symbol));
      row.appendChild(mr);
    }
    return row;
  }

  function branchNote(insn) {
    const s = span("branch");
    if (insn.category === "conditional_branch") {
      if (insn.diverged) {
        s.appendChild(span("taken", "  ; taken"));
        if (insn.target) s.appendChild(span("target", " -> " + insn.target));
      } else {
        s.appendChild(span("nottaken", "  ; not taken"));
      }
      return s;
    }
    if (insn.category === "call" || insn.category === "unconditional_branch" || insn.category === "ret") {
      if (insn.target) {
        s.appendChild(span("dim", "  ; "));
        s.appendChild(span("target", "-> " + insn.target));
        return s;
      }
    }
    return null;
  }

  function renderTraceColumn(trace) {
    const col = el("div", "trace-col");

    const head = el("div", "col-head");
    head.appendChild(span("col-title", "trace"));
    head.appendChild(span("col-sub",
      "thread " + trace.threadId + " · hit " + trace.hit + " · " + trace.instructionCount + " instructions"));
    col.appendChild(head);

    const bodyEl = el("div", "trace-body");

    // entry -> return
    const entry = el("div", "entryinfo");
    entry.appendChild(span("k", "entry"));
    entry.appendChild(span("sym", trace.entrySymbol));
    if (trace.returnSymbol) {
      entry.appendChild(span("k", "return"));
      entry.appendChild(span("sym", trace.returnSymbol));
    }
    bodyEl.appendChild(entry);

    // stack at entry
    if (trace.stack && trace.stack.length) {
      const st = el("div", "stack");
      st.appendChild(span("k", "stack"));
      for (const f of trace.stack) {
        const r = el("div", "stack-frame");
        r.appendChild(span("sym", f.symbol || f.addr));
        if (f.file) {
          const loc = span("loc", baseName(f.file) + ":" + f.line);
          loc.title = f.file + ":" + f.line;
          r.appendChild(loc);
        }
        st.appendChild(r);
      }
      bodyEl.appendChild(st);
    }

    // entry register dump (under the registers toggle)
    if (trace.entryRegisters) bodyEl.appendChild(renderRegisterDump(trace.entryRegisters));

    // instructions, grouped under source headings
    const srcIdx = sourceIndex(trace);
    let curKey = null;
    for (const insn of trace.instructions) {
      if (insn.line > 0) {
        const key = insn.fileId + ":" + insn.line;
        if (key !== curKey) {
          curKey = key;
          const heading = el("div", "src-heading");
          if (insn.fileId >= 0 && insn.line > 0) heading.dataset.loc = key;
          const loc = span("src-loc", baseName(insn.file) + ":" + insn.line);
          loc.title = insn.file + ":" + insn.line;
          heading.appendChild(loc);
          const text = srcIdx.get(key);
          if (text) heading.appendChild(span("src-text", text));
          bodyEl.appendChild(heading);
        }
      }
      bodyEl.appendChild(renderInstruction(insn));
    }

    const footer = el("div", "trace-footer");
    footer.appendChild(span("dim", "trace ended: " + trace.reason));
    if (trace.truncated) footer.appendChild(span("warn", "  (truncated — the aggregates below are incomplete)"));
    bodyEl.appendChild(footer);

    col.appendChild(bodyEl);
    return col;
  }

  function renderRegisterDump(regs) {
    const box = el("div", "entry-registers");
    box.appendChild(span("k", "registers"));
    const grid = el("div", "reg-grid");
    for (const g of regs.gpr) {
      grid.appendChild(span("reg-name", g.name));
      grid.appendChild(span("reg-val", g.value));
    }
    box.appendChild(grid);
    const fl = el("div", "reg-flags");
    fl.appendChild(span("reg-name", "rflags"));
    fl.appendChild(span("reg-val", regs.rflags));
    if (regs.flags && regs.flags.length) fl.appendChild(span("dim", "[" + regs.flags.join(" ") + "]"));
    box.appendChild(fl);
    return box;
  }

  // ---- right column: collapsible views -----------------------------------

  function collapsible(title, open) {
    const d = el("details", "view");
    if (open) d.open = true;
    const s = el("summary", null, title);
    d.appendChild(s);
    const bodyEl = el("div", "view-body");
    d.appendChild(bodyEl);
    return { details: d, body: bodyEl };
  }

  function renderStatsTable(stats) {
    const table = el("table", "stats");
    const header = ["self", "atomics", "slow", "calls d/i", "mem r/w", "br (taken)", "symbol"];
    const thead = el("tr", "head");
    header.forEach((h, i) => thead.appendChild(el(i === header.length - 1 ? "th" : "th", i === 6 ? "sym" : "num", h)));
    table.appendChild(thead);

    const tot = { i: 0, a: 0, s: 0, dc: 0, ic: 0, mr: 0, mw: 0, br: 0, brt: 0 };
    for (const r of stats.rows) {
      const tr = el("tr");
      const cells = [
        String(r.instructions), String(r.atomics), String(r.slow),
        r.directCalls + "/" + r.indirectCalls, r.memoryReads + "/" + r.memoryWrites,
        r.branches + " (" + r.branchesTaken + ")",
      ];
      cells.forEach((c) => tr.appendChild(el("td", "num", c)));
      tr.appendChild(el("td", "sym", r.symbol));
      table.appendChild(tr);
      tot.i += r.instructions; tot.a += r.atomics; tot.s += r.slow;
      tot.dc += r.directCalls; tot.ic += r.indirectCalls;
      tot.mr += r.memoryReads; tot.mw += r.memoryWrites;
      tot.br += r.branches; tot.brt += r.branchesTaken;
    }
    const trt = el("tr", "total");
    [String(tot.i), String(tot.a), String(tot.s), tot.dc + "/" + tot.ic, tot.mr + "/" + tot.mw,
     tot.br + " (" + tot.brt + ")"].forEach((c) => trt.appendChild(el("td", "num", c)));
    trt.appendChild(el("td", "sym", "total (" + stats.traces + (stats.traces === 1 ? " trace)" : " traces)")));
    table.appendChild(trt);

    const wrap = el("div", "table-wrap");
    wrap.appendChild(table);

    if (stats.slowOps && stats.slowOps.length) {
      const so = el("div", "slow-ops");
      so.appendChild(span("slow-head", "slow ops (tens of cycles each — the instruction count does not show these)"));
      for (const s of stats.slowOps) {
        const line = el("div", "slow-row");
        line.appendChild(span("slow-mn", s.mnemonic));
        line.appendChild(span("slow-count", "x" + s.count));
        line.appendChild(span("slow-sym", s.symbol));
        so.appendChild(line);
      }
      wrap.appendChild(so);
    }
    if (stats.truncated)
      wrap.appendChild(el("div", "warn", "TRUNCATED: a trace hit --instructions; these counts are incomplete."));
    return wrap;
  }

  // Aggregate memory computations, reimplemented from the C++ formatters (memory-test.cc is the spec).

  function computeRaw(insns) {
    const out = [];
    for (const insn of insns)
      for (const acc of insn.mem)
        if (regionState[acc.region]) out.push(acc);
    return out;
  }

  function computeCachelines(insns) {
    const buckets = new Map();
    for (const insn of insns)
      for (const acc of insn.mem) {
        if (!regionState[acc.region] || acc.size === 0) continue;
        const addr = parseAddr(acc.addr);
        const size = BigInt(acc.size);
        const first = addr >> 6n;
        const last = (addr + size - 1n) >> 6n;
        for (let line = first; line <= last; line++) {
          const key = line.toString();
          let b = buckets.get(key);
          if (!b) { b = { line, accesses: 0, mask: 0n, reads: false, writes: false, regions: [], symbols: [] }; buckets.set(key, b); }
          const base = line * 64n;
          const lo = (addr > base ? addr : base) - base;
          const end = addr + size;
          const lineEnd = base + 64n;
          const hi = (end < lineEnd ? end : lineEnd) - base;
          for (let bit = lo; bit < hi; bit++) b.mask |= 1n << bit;
          b.accesses++;
          b.reads = b.reads || acc.isRead;
          b.writes = b.writes || acc.isWrite;
          if (!b.regions.includes(acc.region)) b.regions.push(acc.region);
          if (acc.symbol && !b.symbols.includes(acc.symbol)) b.symbols.push(acc.symbol);
        }
      }
    return [...buckets.values()].sort((a, b) => (a.line < b.line ? -1 : a.line > b.line ? 1 : 0));
  }

  function computeMemStats(insns) {
    const rows = new Map();
    const total = { symbol: "total", accesses: 0, reads: 0, writes: 0, bytes: 0, lines: new Set() };
    for (const insn of insns)
      for (const acc of insn.mem) {
        if (!regionState[acc.region]) continue;
        const owner = insn.owner || "(unknown)";
        let r = rows.get(owner);
        if (!r) { r = { symbol: owner, accesses: 0, reads: 0, writes: 0, bytes: 0, lines: new Set() }; rows.set(owner, r); }
        r.accesses++; total.accesses++;
        if (acc.isRead) { r.reads++; total.reads++; }
        if (acc.isWrite) { r.writes++; total.writes++; }
        r.bytes += acc.size; total.bytes += acc.size;
        if (acc.size > 0) {
          const addr = parseAddr(acc.addr);
          const first = addr >> 6n;
          const last = (addr + BigInt(acc.size) - 1n) >> 6n;
          for (let l = first; l <= last; l++) { r.lines.add(l.toString()); total.lines.add(l.toString()); }
        }
      }
    const arr = [...rows.values()].sort((a, b) => b.accesses - a.accesses || (a.symbol < b.symbol ? -1 : 1));
    return { rows: arr, total };
  }

  function joinSyms(syms) {
    let out = "";
    for (const s of syms) {
      const sep = out ? ", " : "";
      if (out.length + sep.length + s.length > 100) { out += sep + "…"; break; }
      out += sep + s;
    }
    return out;
  }

  function renderRawView(insns) {
    const accs = computeRaw(insns);
    if (!accs.length) return el("div", "empty", "(no accesses in the selected regions)");
    const box = el("div", "mono-list");
    for (const acc of accs) {
      const r = el("div", "mem-row");
      r.appendChild(span("mem-addr", acc.addr));
      r.appendChild(span("mem-size", String(acc.size)));
      r.appendChild(span("mem-rw", (acc.isRead ? "R" : "") + (acc.isWrite ? "W" : "")));
      r.appendChild(span("mem-region rgn-fg-" + acc.region, regionLabel(acc.region)));
      if (acc.symbol) r.appendChild(span("mem-sym", acc.symbol));
      box.appendChild(r);
    }
    return box;
  }

  function renderCachelineView(insns) {
    const buckets = computeCachelines(insns);
    if (!buckets.length) return el("div", "empty", "(no accesses in the selected regions)");
    const box = el("div", "mono-list");
    let prev = null;
    for (const b of buckets) {
      if (prev !== null && b.line !== prev + 1n) box.appendChild(el("div", "gap", ""));
      prev = b.line;
      const r = el("div", "cl-row");
      r.appendChild(span("mem-addr", fmtAddr(b.line * 64n)));
      r.appendChild(span("cl-acc", b.accesses + " acc"));
      r.appendChild(span("cl-fp", popcount(b.mask) + "/64"));
      r.appendChild(span("mem-rw", (b.reads ? "R" : "") + (b.writes ? "W" : "")));
      r.appendChild(span("mem-region rgn-fg-" + b.regions[0], b.regions.map(regionLabel).join("/")));
      const names = joinSyms(b.symbols);
      if (names) r.appendChild(span("mem-sym", names));
      box.appendChild(r);
    }
    return box;
  }

  function renderMemStatsView(insns) {
    const { rows, total } = computeMemStats(insns);
    if (!rows.length) return el("div", "empty", "(no accesses in the selected regions)");
    const table = el("table", "stats");
    const thead = el("tr", "head");
    ["acc", "r/w", "lines", "bytes", "executing symbol"].forEach((h, i) => thead.appendChild(el("th", i === 4 ? "sym" : "num", h)));
    table.appendChild(thead);
    const rowOf = (r, cls) => {
      const tr = el("tr", cls);
      [String(r.accesses), r.reads + "/" + r.writes, String(r.lines.size), String(r.bytes)]
        .forEach((c) => tr.appendChild(el("td", "num", c)));
      tr.appendChild(el("td", "sym", r.symbol));
      return tr;
    };
    for (const r of rows) table.appendChild(rowOf(r, null));
    table.appendChild(rowOf(total, "total"));
    const wrap = el("div", "table-wrap");
    wrap.appendChild(table);
    return wrap;
  }

  function renderViewsColumn(trace) {
    const col = el("div", "views-col");

    const stats = collapsible("instruction stats", true);
    stats.body.appendChild(renderStatsTable(trace.stats));
    col.appendChild(stats.details);

    const raw = collapsible("memory accesses", true);
    const cl = collapsible("cacheline accesses", true);
    const ms = collapsible("memory stats", true);
    col.appendChild(raw.details);
    col.appendChild(cl.details);
    col.appendChild(ms.details);

    const rebuild = () => {
      raw.body.replaceChildren(renderRawView(trace.instructions));
      cl.body.replaceChildren(renderCachelineView(trace.instructions));
      ms.body.replaceChildren(renderMemStatsView(trace.instructions));
    };
    rebuild();
    memoryRebuilders.push(rebuild);

    const src = collapsible("source", true);
    src.body.appendChild(renderSourceView(trace));
    col.appendChild(src.details);

    return col;
  }

  // ---- source view (godbolt-style) ---------------------------------------
  // See renderSourceView below and the syntax highlighter.

  function renderTrace(trace) {
    const panel = el("div", "trace-panel");
    panel.setAttribute("role", "tabpanel");
    const grid = el("div", "trace-grid");
    grid.appendChild(renderTraceColumn(trace));
    grid.appendChild(renderViewsColumn(trace));
    panel.appendChild(grid);
    return panel;
  }

  // ---- hover-linking: source line <-> the instructions that ran it -------

  function installHoverLinking(root) {
    let active = null;
    const clear = () => {
      if (!active) return;
      root.querySelectorAll(".loc-hi").forEach((e) => e.classList.remove("loc-hi"));
      active = null;
    };
    root.addEventListener("mouseover", (ev) => {
      const t = ev.target.closest("[data-loc]");
      if (!t) return;
      if (t.dataset.loc === active) return;
      clear();
      active = t.dataset.loc;
      root.querySelectorAll('[data-loc="' + cssEscape(active) + '"]').forEach((e) => e.classList.add("loc-hi"));
    });
    root.addEventListener("mouseout", (ev) => {
      const t = ev.target.closest("[data-loc]");
      if (t && !ev.relatedTarget?.closest?.('[data-loc="' + cssEscape(active) + '"]')) clear();
    });
  }
  function cssEscape(s) { return s.replace(/["\\]/g, "\\$&"); }

  // ==== source view + highlighter =========================================

  function renderSourceView(trace) {
    const wrap = el("div", "source-view");
    if (!trace.source.files.length) return el("div", "empty", "(no source resolved for this trace)");
    for (const f of trace.source.files) {
      const fileBox = el("details", "src-file");
      fileBox.open = true;
      fileBox.appendChild(el("summary", "src-file-head", f.displayName));
      for (const r of f.ranges) {
        const rng = el("details", "src-range");
        rng.open = true;
        rng.appendChild(el("summary", "src-range-head", f.displayName + ":" + r.start + "-" + r.end));
        const code = el("div", "src-code");
        const state = { inComment: false, depth: 0 };
        for (const ln of r.lines) {
          const line = el("div", "src-line");
          if (ln.executed) {
            line.classList.add("executed");
            line.dataset.loc = f.fileId + ":" + ln.number;
          }
          line.appendChild(span("src-gutter", String(ln.number)));
          const bar = span("src-bar", "");
          line.appendChild(bar);
          const content = span("src-content");
          highlightInto(content, ln.text, state);
          line.appendChild(content);
          code.appendChild(line);
        }
        rng.appendChild(code);
        fileBox.appendChild(rng);
      }
      wrap.appendChild(fileBox);
    }
    return wrap;
  }

  const KEYWORDS = new Set([
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "char8_t",
    "char16_t", "char32_t", "class", "co_await", "co_return", "co_yield", "concept", "const",
    "consteval", "constexpr", "constinit", "const_cast", "continue", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
    "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "nullptr", "operator", "or", "override", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "wchar_t", "while", "final",
    // clean-core primitive aliases, commonly seen in this codebase
    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "isize", "usize",
  ]);
  const BRACKET_COLORS = 3;

  function isIdentStart(c) { return /[A-Za-z_]/.test(c); }
  function isIdent(c) { return /[A-Za-z0-9_]/.test(c); }
  function isDigit(c) { return c >= "0" && c <= "9"; }

  // Tokenize one line into colored spans, appending to `parent`. `state` carries block-comment and
  // bracket-nesting depth across the lines of a range.
  function highlightInto(parent, text, state) {
    let i = 0;
    const n = text.length;
    const push = (cls, s) => { if (s) parent.appendChild(span(cls, s)); };

    while (i < n) {
      if (state.inComment) {
        const endc = text.indexOf("*/", i);
        if (endc < 0) { push("tok-comment", text.slice(i)); i = n; }
        else { push("tok-comment", text.slice(i, endc + 2)); i = endc + 2; state.inComment = false; }
        continue;
      }
      const c = text[i];
      const c2 = text.slice(i, i + 2);

      if (c2 === "//") { push("tok-comment", text.slice(i)); break; }
      if (c2 === "/*") {
        const endc = text.indexOf("*/", i + 2);
        if (endc < 0) { push("tok-comment", text.slice(i)); state.inComment = true; i = n; }
        else { push("tok-comment", text.slice(i, endc + 2)); i = endc + 2; }
        continue;
      }
      if (c === '"' || c === "'") {
        let j = i + 1;
        while (j < n) { if (text[j] === "\\") j += 2; else if (text[j] === c) { j++; break; } else j++; }
        push("tok-string", text.slice(i, j));
        i = j;
        continue;
      }
      if (isDigit(c) || (c === "." && isDigit(text[i + 1] || ""))) {
        let j = i + 1;
        while (j < n && /[0-9a-fA-FxX.'pPeE+-]/.test(text[j])) {
          // Stop a trailing + / - that is not part of an exponent.
          if ((text[j] === "+" || text[j] === "-") && !/[eEpP]/.test(text[j - 1])) break;
          j++;
        }
        while (j < n && /[uUlLfF]/.test(text[j])) j++;
        push("tok-number", text.slice(i, j));
        i = j;
        continue;
      }
      if (isIdentStart(c)) {
        let j = i + 1;
        while (j < n && isIdent(text[j])) j++;
        const word = text.slice(i, j);
        push(KEYWORDS.has(word) ? "tok-keyword" : "tok-ident", word);
        i = j;
        continue;
      }
      if (c === "(" || c === "[" || c === "{") {
        push("tok-brk brk" + (state.depth % BRACKET_COLORS), c);
        state.depth++;
        i++;
        continue;
      }
      if (c === ")" || c === "]" || c === "}") {
        if (state.depth > 0) state.depth--;
        push("tok-brk brk" + (state.depth % BRACKET_COLORS), c);
        i++;
        continue;
      }
      push("tok-punct", c);
      i++;
    }
  }

  // ---- main (runs last, after every declaration above is initialized) -----

  const app = document.getElementById("app");
  app.appendChild(renderHeader());
  app.appendChild(renderControls());

  const panels = D.traces.map(renderTrace);
  app.appendChild(renderTabs(panels));
  const panelHost = el("div", "panels");
  panels.forEach((p) => panelHost.appendChild(p));
  app.appendChild(panelHost);

  applyRegionClasses();
  installHoverLinking(app);
})();
