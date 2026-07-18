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

  // Instruction prefixes that lead the disassembly text: colored distinctly (like a keyword), with
  // the real mnemonic — the token after the prefix — linked and highlighted like any other.
  const ASM_PREFIXES = new Set(["lock", "rep", "repe", "repz", "repne", "repnz", "bnd", "notrack"]);

  // Render one instruction's disassembly (mnemonic link + colored operands) into a `.code` span.
  // Shared by the trace column and the waterfall so both read identically.
  function renderCode(insn) {
    const code = span("code");
    if (!insn.mnemonic) {
      code.appendChild(span("operands", insn.text));
      return code;
    }
    let text = insn.text;
    let mnem = insn.mnemonic;
    if (ASM_PREFIXES.has(mnem.toLowerCase())) {
      code.appendChild(span("asm-prefix", mnem));
      code.appendChild(span(null, " "));
      text = text.slice(mnem.length).replace(/^\s+/, "");
      const sp = text.indexOf(" ");
      mnem = sp < 0 ? text : text.slice(0, sp);
    }
    const a = el("a", "mnemonic", mnem);
    a.href = felixUrl(mnem);
    a.target = "_blank";
    a.rel = "noopener";
    code.appendChild(a);
    highlightAsm(code, text.slice(mnem.length));
    return code;
  }

  // ---- region filter state (drives inline memory + the 3 aggregate views) ----

  const regionState = {
    heap: D.meta.regions.heap,
    stack: D.meta.regions.stack,
    frame: D.meta.regions.frame,
    instructions: D.meta.regions.instructions,
  };
  const memoryRebuilders = []; // () => void, one per trace panel; called on a region change
  const allViews = []; // every <details class="view">, so collapse-all can reach them (the DOM shim
                       // stubs querySelectorAll, so we keep an explicit list rather than query)

  function applyRegionClasses() {
    for (const r of ["heap", "stack", "frame", "instructions"])
      body.classList.toggle("rgn-" + r, regionState[r]);
  }

  // ---- cross-highlight registry (source <-> instructions <-> memory views) ----
  //
  // The relationships are many-to-many — one instruction touches several addresses, one address is
  // touched by several instructions, a cacheline aggregates many accesses. So instead of a single
  // data-* key we tag each element with a set of *links* {key, rw} and, on hover, light every element
  // that shares any key. Keys: "loc:<fileId>:<line>", "addr:<hi`lo>", "cl:<cacheline index>". The
  // read/write direction rides on each link so a matched element can tint by its own access.
  const linkKeyMap = new Map(); // key -> Set<element>
  const linkedEls = [];         // elements carrying `_links`, for teardown/debug

  function registerLinks(elm, links) {
    if (!links || !links.length) return;
    elm._links = links;
    elm.dataset.linked = "1";
    linkedEls.push(elm);
    for (const l of links) {
      let set = linkKeyMap.get(l.key);
      if (!set) { set = new Set(); linkKeyMap.set(l.key, set); }
      set.add(elm);
    }
  }

  function rwOf(acc) { return acc.isRead && acc.isWrite ? "rw" : acc.isWrite ? "w" : "r"; }
  function cachelineKey(addrStr) { return (parseAddr(addrStr) >> 6n).toString(); }

  // The links one memory access contributes: its address, its cacheline, and (so the source lights up
  // too) the source location of the instruction that made it.
  function accessLinks(acc, loc) {
    const rw = rwOf(acc);
    const links = [{ key: "addr:" + acc.addr, rw }, { key: "cl:" + cachelineKey(acc.addr), rw }];
    if (loc) links.push({ key: "loc:" + loc, rw: null });
    return links;
  }

  function mergeRw(a, b) {
    if (a === "w" || b === "w" || a === "rw" || b === "rw") return "w";
    if (a === "r" || b === "r") return "r";
    return null;
  }
  function dedupLinks(links) {
    const map = new Map(); // key -> merged rw
    for (const l of links) map.set(l.key, map.has(l.key) ? mergeRw(map.get(l.key), l.rw) : l.rw);
    return [...map].map(([key, rw]) => ({ key, rw }));
  }

  let activeHi = [];
  function clearHighlight() {
    for (const e of activeHi) e.classList.remove("link-hi", "link-hi-r", "link-hi-w");
    activeHi = [];
  }
  function highlightFrom(elm) {
    clearHighlight();
    if (!elm || !elm._links) return;
    const keys = new Set(elm._links.map((l) => l.key));
    const matched = new Set();
    for (const k of keys) {
      const set = linkKeyMap.get(k);
      if (set) for (const m of set) matched.add(m);
    }
    for (const m of matched) {
      let rw = null; // strongest read/write among m's links that matched (write wins, then read)
      for (const l of m._links) {
        if (!keys.has(l.key)) continue;
        if (l.rw === "w" || l.rw === "rw") { rw = "w"; break; }
        if (l.rw === "r") rw = rw || "r";
      }
      m.classList.add(rw === "w" ? "link-hi-w" : rw === "r" ? "link-hi-r" : "link-hi");
      activeHi.push(m);
    }
  }

  function installCrossHighlighting(root) {
    root.addEventListener("mouseover", (ev) => {
      const t = ev.target.closest ? ev.target.closest("[data-linked]") : null;
      if (t) highlightFrom(t);
    });
    root.addEventListener("mouseout", (ev) => {
      const t = ev.target.closest ? ev.target.closest("[data-linked]") : null;
      if (t) clearHighlight();
    });
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
    if (D.traces.some((t) => t.mca && t.mca.available)) toggle("timing", "show-timing", false);

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

    // Collapse / expand every right-column view at once (the column grew ~6 collapsibles).
    bar.appendChild(span("controls-sep", "views"));
    let allOpen = true;
    const collapseBtn = el("button", "ctl-btn", "collapse all");
    collapseBtn.addEventListener("click", () => {
      allOpen = !allOpen;
      for (const d of allViews) d.open = allOpen;
      collapseBtn.textContent = allOpen ? "collapse all" : "expand all";
    });
    bar.appendChild(collapseBtn);
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

  function fmtNum(x) { return (Math.round(x * 100) / 100).toString(); }

  function renderInstruction(insn, mi) {
    const row = el("div", "insn");
    const loc = insn.fileId >= 0 && insn.line > 0 ? insn.fileId + ":" + insn.line : null;
    if (insn.isAtomic) row.classList.add("atomic");

    row.appendChild(span("addr", insn.addr));
    row.appendChild(renderCode(insn));

    // Branch annotation, mirroring the terminal's semantics.
    const note = branchNote(insn);
    if (note) row.appendChild(note);

    // llvm-mca timing: the @retire cycle is shown by default (dim, like the address); the µops /
    // latency / throughput detail is gated behind the `timing` toggle.
    if (mi && mi.valid) {
      if (mi.hasTimeline) row.appendChild(span("retire", " @" + mi.cRetired));
      let detail = " " + mi.uops + "µ " + mi.latency + "c rthr" + fmtNum(mi.rthroughput);
      if (mi.mayLoad) detail += " ld";
      if (mi.mayStore) detail += " st";
      row.appendChild(span("timing", detail));
    }

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
      registerLinks(mr, accessLinks(acc, loc));
      row.appendChild(mr);
    }

    // The row itself links to its source line and every address it touches, so hovering the
    // instruction lights up the matching rows in the memory views (and vice versa).
    const rowLinks = [];
    if (loc) rowLinks.push({ key: "loc:" + loc, rw: null });
    for (const acc of insn.mem) {
      const rw = rwOf(acc);
      rowLinks.push({ key: "addr:" + acc.addr, rw });
      rowLinks.push({ key: "cl:" + cachelineKey(acc.addr), rw });
    }
    registerLinks(row, rowLinks);
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
    const mca = trace.mca && trace.mca.available && trace.mca.perInstructionValid ? trace.mca : null;

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
    for (let i = 0; i < trace.instructions.length; i++) {
      const insn = trace.instructions[i];
      if (insn.line > 0) {
        const key = insn.fileId + ":" + insn.line;
        if (key !== curKey) {
          curKey = key;
          const heading = el("div", "src-heading");
          if (insn.fileId >= 0 && insn.line > 0) registerLinks(heading, [{ key: "loc:" + key, rw: null }]);
          const loc = span("src-loc", baseName(insn.file) + ":" + insn.line);
          loc.title = insn.file + ":" + insn.line;
          heading.appendChild(loc);
          const text = srcIdx.get(key);
          if (text) heading.appendChild(span("src-text", text));
          bodyEl.appendChild(heading);
        }
      }
      bodyEl.appendChild(renderInstruction(insn, mca ? mca.instructions[i] : null));
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

  function collapsible(title, open, info) {
    const d = el("details", "view");
    if (open) d.open = true;
    const s = el("summary");
    s.appendChild(span("view-title", title));
    if (info) {
      const icon = span("info-icon", "ⓘ");
      icon.dataset.tip = info;
      s.appendChild(icon);
    }
    d.appendChild(s);
    const bodyEl = el("div", "view-body");
    d.appendChild(bodyEl);
    allViews.push(d);
    return { details: d, body: bodyEl };
  }

  // A stable color per resource index, so a port keeps its hue across the heatmap and the waterfall.
  const PORT_HUES = [200, 30, 145, 280, 0, 55, 320, 95, 255, 175, 15, 220];
  function portColor(idx) { return "hsl(" + PORT_HUES[idx % PORT_HUES.length] + " 60% 55%)"; }

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
    for (const insn of insns) {
      const loc = insn.fileId >= 0 && insn.line > 0 ? insn.fileId + ":" + insn.line : null;
      for (const acc of insn.mem)
        if (regionState[acc.region]) out.push({ acc, loc });
    }
    return out;
  }

  function computeCachelines(insns) {
    const buckets = new Map();
    for (const insn of insns) {
      const loc = insn.fileId >= 0 && insn.line > 0 ? insn.fileId + ":" + insn.line : null;
      for (const acc of insn.mem) {
        if (!regionState[acc.region] || acc.size === 0) continue;
        const addr = parseAddr(acc.addr);
        const size = BigInt(acc.size);
        const first = addr >> 6n;
        const last = (addr + size - 1n) >> 6n;
        for (let line = first; line <= last; line++) {
          const key = line.toString();
          let b = buckets.get(key);
          if (!b) { b = { line, accesses: 0, mask: 0n, reads: false, writes: false, regions: [], symbols: [], members: [] }; buckets.set(key, b); }
          b.members.push({ acc, loc });
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
    for (const { acc, loc } of accs) {
      const r = el("div", "mem-row");
      r.appendChild(span("mem-addr", acc.addr));
      r.appendChild(span("mem-size", String(acc.size)));
      r.appendChild(span("mem-rw", (acc.isRead ? "R" : "") + (acc.isWrite ? "W" : "")));
      r.appendChild(span("mem-region rgn-fg-" + acc.region, regionLabel(acc.region)));
      if (acc.symbol) r.appendChild(span("mem-sym", acc.symbol));
      registerLinks(r, accessLinks(acc, loc));
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
      // Hovering a cacheline lights every access, instruction and source line that landed on it.
      const links = b.members.flatMap((m) => accessLinks(m.acc, m.loc));
      links.push({ key: "cl:" + b.line.toString(), rw: null });
      registerLinks(r, dedupLinks(links));
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

  // ---- llvm-mca timing views ---------------------------------------------

  // Rate a value in [0,1] of its ideal into a good/mid/low class for the color-coded cells.
  function rateFraction(frac) { return frac >= 0.66 ? "good" : frac >= 0.33 ? "mid" : "low"; }

  function renderBlockSummary(mca) {
    const s = mca.summary;
    const table = el("table", "stats mca-summary");
    const row = (k, v, tip, cls) => {
      const tr = el("tr");
      const kc = el("td", "mca-k", k);
      if (tip) kc.dataset.tip = tip;
      tr.appendChild(kc);
      tr.appendChild(el("td", "mca-v" + (cls ? " " + cls : ""), v));
      table.appendChild(tr);
    };
    // IPC and µops/cycle are rated against the dispatch width — the hardware ceiling for both.
    const width = s.dispatchWidth || 6;
    row("micro-arch", mca.cpu || "?", "The CPU model llvm-mca simulated (host by default, via -mcpu=native).");
    row("IPC", fmtNum(s.ipc),
      "Instructions retired per cycle. Higher is better; the ceiling is the dispatch width (" + width + "). Low IPC means the block is latency- or resource-bound, not that it is slow in wall-clock.",
      rateFraction(s.ipc / width));
    row("block RThroughput", fmtNum(s.blockRThroughput),
      "Reciprocal throughput: the cycles one iteration of the block needs at peak, ignoring latency. A lower bound on cost — lower is better.");
    row("total cycles", String(s.totalCycles),
      "Modeled cycles to run all " + s.iterations + " iterations of the block back to back.");
    row("total µops", String(s.totalUops),
      "Total micro-ops issued. Instructions decode into one or more µops; this is the real work the scheduler sees.");
    row("µops / cycle", fmtNum(s.uopsPerCycle),
      "Micro-ops retired per cycle. Higher is better; the ceiling is the dispatch width (" + width + ").",
      rateFraction(s.uopsPerCycle / width));
    row("dispatch width", String(s.dispatchWidth),
      "How many µops the front-end can dispatch per cycle — the hardware ceiling for IPC and µops/cycle.");
    row("iterations", String(s.iterations),
      "How many times llvm-mca ran the block to reach steady state (its default aggregate). The waterfall and @retire come from iteration 0 only.");
    const wrap = el("div", "table-wrap");
    wrap.appendChild(table);
    wrap.appendChild(el("div", "mca-note",
      "Static model over " + s.iterations + " steady-state iterations; assumes the block loops and is blind to cache misses."));
    return wrap;
  }

  function renderPortPressure(mca) {
    const wrap = el("div", "mca-ports");

    wrap.appendChild(el("div", "mca-subhead", "port pressure"));
    wrap.appendChild(el("div", "mca-note",
      "How the µops spread across execution ports. A tall, lopsided bar is a port the code leans on — a throughput ceiling. Evenly spread and short is healthier than one saturated port."));

    // Aggregate pressure per resource across every timed instruction — the "where do the µops go" heatmap.
    const totals = new Array(mca.resources.length).fill(0);
    for (const mi of mca.instructions)
      if (mi.valid && mi.portPressure)
        mi.portPressure.forEach((u, idx) => { totals[idx] += u; });
    let maxT = 0;
    for (const t of totals) maxT = Math.max(maxT, t);

    const heat = el("div", "port-heat");
    mca.resources.forEach((name, idx) => {
      if (totals[idx] <= 0) return;
      const r = el("div", "port-row");
      r.dataset.tip = name + ": " + fmtNum(totals[idx]) + " cycles of pressure across the block (sum over all instructions).";
      r.appendChild(span("port-name", name));
      const track = el("div", "port-track");
      const fill = el("div", "port-fill");
      fill.style.width = (maxT > 0 ? 100 * totals[idx] / maxT : 0) + "%";
      fill.style.background = portColor(idx);
      track.appendChild(fill);
      r.appendChild(track);
      r.appendChild(span("port-val", fmtNum(totals[idx])));
      heat.appendChild(r);
    });
    if (!heat.children.length) heat.appendChild(el("div", "empty", "(no port pressure reported)"));
    wrap.appendChild(heat);

    const b = mca.bottleneck;
    if (b && b.available && b.totalCycles > 0) {
      const sub = el("div", "mca-subhead", "bottleneck — of " + b.totalCycles + " cycles");
      sub.dataset.tip = "Where the cycles went. Dependency bars = the block waited on results (latency-bound). Resource pressure = it waited on a busy port (throughput-bound). The bars can overlap and need not sum to the total.";
      wrap.appendChild(sub);
      const bars = el("div", "btl-bars");
      const bar = (label, val, cls, tip) => {
        const r = el("div", "btl-row");
        if (tip) r.dataset.tip = tip;
        r.appendChild(span("btl-label", label));
        const track = el("div", "btl-track");
        const fill = el("div", "btl-fill " + cls);
        fill.style.width = (100 * val / b.totalCycles) + "%";
        track.appendChild(fill);
        r.appendChild(track);
        r.appendChild(span("btl-val", val + "c"));
        bars.appendChild(r);
      };
      bar("register deps", b.registerDependency, "btl-reg", "Cycles stalled waiting for a register result — a dependency chain. Shorten the chain or break the dependency to speed it up.");
      bar("data deps", b.dataDependency, "btl-data", "Cycles stalled on a data dependency (register or memory result feeding the next op).");
      bar("memory deps", b.memoryDependency, "btl-mem", "Cycles stalled on a memory dependency — a load waiting on an earlier store to the same address (store-to-load forwarding).");
      bar("resource pressure", b.resourcePressure, "btl-res", "Cycles lost because a needed execution port was busy — throughput-bound, not latency-bound. See the port pressure above.");
      wrap.appendChild(bars);
      if (b.topPorts && b.topPorts.length) {
        const lim = el("div", "btl-limit");
        lim.appendChild(span("btl-limit-label", "limited by "));
        lim.appendChild(span(null, b.topPorts.map((p) => p.resource + " (" + fmtNum(p.cycles) + "c)").join(", ")));
        wrap.appendChild(lim);
      }
    }
    return wrap;
  }

  // Full-width pipeline waterfall: rows = instructions, columns = cycles; each row a bar from the
  // dispatch cycle to the retire cycle, segmented by phase (wait -> execute -> retire).
  const WF_CELL = 13;      // px per cycle
  const WF_ROW_CAP = 512;  // long traces: cap rows and say so, rather than freeze the browser

  function renderWaterfall(trace) {
    const mca = trace.mca && trace.mca.available && trace.mca.perInstructionValid ? trace.mca : null;
    if (!mca) return el("div", "empty", "(no per-instruction timeline for this trace)");

    const rows = [];
    for (let i = 0; i < trace.instructions.length; i++) {
      const mi = mca.instructions[i];
      if (mi && mi.valid && mi.hasTimeline) rows.push({ insn: trace.instructions[i], mi });
    }
    if (!rows.length) return el("div", "empty", "(llvm-mca produced no timeline cycles)");

    let truncated = false;
    let shown = rows;
    if (rows.length > WF_ROW_CAP) { shown = rows.slice(0, WF_ROW_CAP); truncated = true; }

    let maxCycle = 0;
    for (const r of shown) maxCycle = Math.max(maxCycle, r.mi.cRetired);
    const numCols = maxCycle + 1;
    const trackW = numCols * WF_CELL;

    const wrap = el("div", "waterfall");

    // legend (each phase carries a tooltip explaining what it means)
    const legend = el("div", "wf-legend");
    const chip = (cls, label, tip) => {
      const c = span("wf-chip", "");
      if (tip) c.dataset.tip = tip;
      c.appendChild(span("wf-swatch " + cls, ""));
      c.appendChild(span(null, label));
      legend.appendChild(c);
    };
    chip("wf-wait", "wait (dispatch → issue)",
      "Cycles between being dispatched into the scheduler and issuing to a port — waiting for its input operands and a free execution port.");
    chip("wf-exec", "execute",
      "Cycles spent executing in a functional unit — the instruction's latency.");
    chip("wf-retire", "retire wait",
      "Finished executing but not yet retired. Instructions retire in program order, so a fast one waits for older ones ahead of it.");
    wrap.appendChild(legend);

    const scroll = el("div", "wf-scroll");
    const table = el("div", "wf-table");

    // cycle ruler
    const ruler = el("div", "wf-ruler");
    ruler.appendChild(span("wf-label", "cycle"));
    const rtrack = el("div", "wf-track");
    rtrack.style.width = trackW + "px";
    for (let cyc = 0; cyc < numCols; cyc += 5) {
      const tick = span("wf-tick", String(cyc));
      tick.style.left = (cyc * WF_CELL) + "px";
      rtrack.appendChild(tick);
    }
    ruler.appendChild(rtrack);
    table.appendChild(ruler);

    const seg = (a, b, cls, tip) => {
      const d = el("div", "wf-seg " + cls);
      d.style.left = (a * WF_CELL) + "px";
      d.style.width = (Math.max(1, b - a) * WF_CELL) + "px";
      if (tip) d.dataset.tip = tip;
      return d;
    };

    for (const { insn, mi } of shown) {
      const r = el("div", "wf-row");
      const loc = insn.fileId >= 0 && insn.line > 0 ? insn.fileId + ":" + insn.line : null;
      const label = span("wf-label");
      label.dataset.tip = "dispatched @" + mi.cDispatched + " · issued @" + mi.cIssued + " · executed @" + mi.cExecuted + " · retired @" + mi.cRetired;
      label.appendChild(span("wf-addr", insn.addr.slice(9))); // low half only, to save width
      label.appendChild(renderCode(insn));                    // same highlighting as the trace column
      r.appendChild(label);

      const track = el("div", "wf-track");
      track.style.width = trackW + "px";
      const issue = Math.max(mi.cIssued, mi.cDispatched);
      const exec = Math.max(mi.cExecuted, issue);
      if (issue > mi.cDispatched) track.appendChild(seg(mi.cDispatched, issue, "wf-wait", "waiting for operands / a free port (" + (issue - mi.cDispatched) + "c)"));
      if (exec > issue) track.appendChild(seg(issue, exec, "wf-exec", "executing (" + (exec - issue) + "c latency)"));
      if (mi.cRetired > exec) track.appendChild(seg(exec, mi.cRetired, "wf-retire", "waiting to retire in order (" + (mi.cRetired - exec) + "c)"));
      track.appendChild(seg(mi.cRetired, mi.cRetired + 1, "wf-tickmark", "retired at cycle " + mi.cRetired));
      r.appendChild(track);

      // Waterfall rows join the same cross-highlighting as the trace column.
      const links = [];
      if (loc) links.push({ key: "loc:" + loc, rw: null });
      for (const acc of insn.mem) {
        const rw = rwOf(acc);
        links.push({ key: "addr:" + acc.addr, rw });
        links.push({ key: "cl:" + cachelineKey(acc.addr), rw });
      }
      registerLinks(r, links);
      table.appendChild(r);
    }

    scroll.appendChild(table);
    wrap.appendChild(scroll);
    if (truncated)
      wrap.appendChild(el("div", "warn", "waterfall capped at " + WF_ROW_CAP + " of " + rows.length + " instructions"));
    return wrap;
  }

  function renderViewsColumn(trace) {
    const col = el("div", "views-col");

    const stats = collapsible("instruction stats", true,
      "Per-symbol tally of what ran: self instructions, atomics, slow ops, direct/indirect calls, memory reads/writes, and branches taken. A count, not a cost model — see block summary for timing.");
    stats.body.appendChild(renderStatsTable(trace.stats));
    col.appendChild(stats.details);

    const mca = trace.mca && trace.mca.available ? trace.mca : null;
    if (mca) {
      const bs = collapsible("block summary (llvm-mca)", true,
        "llvm-mca's static timing model of this exact instruction stream, aggregated over " + mca.summary.iterations + " steady-state iterations. IPC and cycles assume the block loops; it models the pipeline, not the memory system (blind to cache misses).");
      bs.body.appendChild(renderBlockSummary(mca));
      col.appendChild(bs.details);

      const pp = collapsible("port pressure & bottleneck", true,
        "Which execution ports the µops use, and where the cycles were lost: dependency stalls (latency-bound) versus resource pressure (throughput-bound). The limiting ports cap the block's throughput.");
      pp.body.appendChild(renderPortPressure(mca));
      col.appendChild(pp.details);
    }

    const raw = collapsible("memory accesses", true,
      "Every memory operand's runtime effective address, in execution order: address, size, read/write, region, and the symbol it hit. Hover a row to light the instructions and source that touch it.");
    const cl = collapsible("cacheline accesses", true,
      "The same accesses bucketed into 64-byte cachelines: how many accesses hit each line and how many of its bytes — the footprint (\"am I using the whole line or 8 bytes of it\"). Hover to light everything on the line.");
    const ms = collapsible("memory stats", true,
      "Per-symbol memory table, grouped by the function making the accesses: access count, reads/writes, distinct cachelines, and bytes moved.");
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

    const src = collapsible("source", true,
      "Every source line the trace touched, grown by context and syntax-highlighted, with an executed-line marker. Hover a line to light the instructions that came from it.");
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

    // Without a per-instruction timeline there is nothing to put in a waterfall, so keep the plain
    // single-view layout. With one, add a second tab level: trace view vs full-width waterfall.
    const hasWaterfall = trace.mca && trace.mca.available && trace.mca.perInstructionValid;
    if (!hasWaterfall) {
      panel.appendChild(grid);
      return panel;
    }

    const traceView = el("div", "subpanel");
    traceView.appendChild(grid);
    const waterfallView = el("div", "subpanel");
    waterfallView.appendChild(renderWaterfall(trace));
    const subpanels = [traceView, waterfallView];

    const subbar = el("div", "subtabbar");
    const buttons = [];
    const select = (i) => {
      subpanels.forEach((p, j) => p.classList.toggle("active", i === j));
      buttons.forEach((b, j) => b.setAttribute("aria-selected", String(i === j)));
    };
    ["trace view", "waterfall view"].forEach((name, i) => {
      const b = el("button", "subtab", name);
      b.setAttribute("role", "tab");
      b.addEventListener("click", () => select(i));
      buttons.push(b);
      subbar.appendChild(b);
    });
    select(0);

    panel.appendChild(subbar);
    const host = el("div", "subpanels");
    subpanels.forEach((p) => host.appendChild(p));
    panel.appendChild(host);
    return panel;
  }

  // ---- tooltips: explain metrics and phases for readers new to the model ----

  function installTooltips(root) {
    const tip = el("div", "tooltip");
    tip.style.display = "none";
    document.body.appendChild(tip);
    let current = null;
    const place = (t) => {
      const r = t.getBoundingClientRect();
      tip.style.left = Math.max(8, r.left) + "px";
      tip.style.top = (r.bottom + 6) + "px";
    };
    root.addEventListener("mouseover", (ev) => {
      const t = ev.target.closest ? ev.target.closest("[data-tip]") : null;
      if (!t || t === current) return;
      current = t;
      tip.textContent = t.dataset.tip;
      tip.style.display = "block";
      place(t);
    });
    root.addEventListener("mouseout", (ev) => {
      const t = ev.target.closest ? ev.target.closest("[data-tip]") : null;
      if (t && t === current) { tip.style.display = "none"; current = null; }
    });
  }

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
            registerLinks(line, [{ key: "loc:" + f.fileId + ":" + ln.number, rw: null }]);
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
  installCrossHighlighting(app);
  installTooltips(app);
})();
