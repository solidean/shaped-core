"""`assembly` — search symbols and disassemble functions from built object files.

A local godbolt over the object code the current preset produced:

    assembly search <pattern>   list matching symbols (mangled + demangled), grouped by target
    assembly show <symbol>      disassemble one function (Intel syntax, labeled branches)
    assembly trace <symbol>     record what a function ACTUALLY executed at run time

search/show answer the static question — what the code might do. Objects, not the linked
binary, are the source of truth there: a release .exe is stripped of its symbol table, while
every .obj keeps a full one and holds the real inlined codegen of a function. See
tools/dev/lib/toolchain/disasm.py for the rationale and docs/guides/disassembly.md.

trace answers the dynamic one — which branch was taken, where an indirect call landed, how many
instructions an invocation really retired. It drives tools/instruction-tracer (Windows x64 only);
see that tool's readme.md.
"""

from __future__ import annotations

import argparse
import platform
import re
import subprocess
import sys
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.toolchain import disasm

from . import args as a
from .context import Context

NAME = "assembly"

# Built by tools/instruction-tracer/CMakeLists.txt; only exists on Windows with SC_BUILD_TOOLS.
TRACER_TARGET = "instruction-tracer"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Search symbols and disassemble functions (local godbolt over .obj)")
    asm_sub = p.add_subparsers(dest="assembly_cmd", required=True)

    s = asm_sub.add_parser("search", help="List symbols matching a pattern (mangled + demangled)")
    a.preset(s)
    s.add_argument("pattern", help="Substring (case-insensitive) matched against mangled and demangled names")
    s.add_argument("--target", action="append",
                   help="Restrict to target(s): comma-list, repeatable, wildcards (default: all)")
    s.add_argument("--regex", action="store_true", help="Treat pattern as a regular expression")
    s.add_argument("--all", action="store_true",
                   help="Include data symbols too (default: only text/function symbols)")
    s.add_argument("--limit", type=int, default=100, metavar="N",
                   help="Max match rows to print (default: 100)")

    d = asm_sub.add_parser("show", help="Disassemble one function by exact symbol name")
    a.preset(d)
    d.add_argument("symbol", help="Exact mangled or demangled symbol name (quote demangled names)")
    d.add_argument("--target", action="append", help="Restrict the search scope to target(s)")
    d.add_argument("--source", action="store_true",
                   help="Interleave source lines (best-effort; needs debug info, i.e. a relwithdebinfo preset)")
    d.add_argument("--bytes", action="store_true", help="Show raw instruction bytes")
    d.add_argument("--att", action="store_true", help="AT&T syntax instead of Intel")

    t = asm_sub.add_parser(
        "trace",
        help="Record the instructions a function actually retired at run time (Windows x64)",
        description="Run --target under the debugger, break on a symbol, and print what one "
                    "invocation actually executed. Everything after `--` is passed to the traced "
                    "binary (e.g. a nexus test-name filter).",
    )
    a.preset(t)
    t.add_argument("--target", required=True, metavar="TARGET",
                   help="Executable target to trace, e.g. clean-core-test (built automatically)")

    # Exactly one, mirroring the tracer. `--spec` rather than `--target` because --target already
    # means a build target across this command.
    where = t.add_mutually_exclusive_group(required=True)
    where.add_argument("--symbol", help="Break on a symbol; a unique substring is enough")
    where.add_argument("--address", help="Break on an absolute runtime address, e.g. 0x7ff611203410")
    where.add_argument("--spec", help="Target spec: foo::bar | 0x7ff6... | mod.exe!foo::bar | mod.exe+0x3410")

    t.add_argument("--skip", type=int, metavar="N", help="Ignore the first N entry hits (default: 0)")
    t.add_argument("--traces", type=int, metavar="N", help="Record N invocations (default: 1)")
    t.add_argument("--instructions", type=int, metavar="N",
                   help="Max retired instructions per trace (default: 100)")

    t.add_argument("--sections", metavar="LIST",
                   help="Comma-separated output sections, all from one capture: trace, stats, memory, "
                        "cachelines, memory-stats (default: trace). Any non-trace section raises the "
                        "--instructions default to 100000")
    t.add_argument("--memory-regions", metavar="LIST",
                   help="Comma-separated address regions the memory sections show: heap, frame, stack, "
                        "instructions (default: heap,stack)")

    # BooleanOptionalAction gives each its --no- form; default None means "don't pass it, let the
    # tracer's own default stand" — so the defaults live in one place, not two.
    for flag, help_text in (
        ("until-return", "Stop once the entry frame returns (default: on)"),
        ("stop-at-syscall", "Stop before executing a syscall (default: on)"),
        ("stack", "Print the stack at entry (default: on)"),
        ("source", "Annotate with source file/line and text (default: on)"),
        ("register-diffs", "Show the registers each instruction changed (default: off)"),
        ("terminate-after-traces", "Kill the debuggee once done (default: on)"),
        ("stats", "Shortcut for --sections stats (default: off)"),
        ("memory-instruction-addresses",
         "Annotate the memory and cacheline views with the accessing instruction (default: off)"),
    ):
        t.add_argument(f"--{flag}", action=argparse.BooleanOptionalAction, default=None, help=help_text)

    t.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    t.add_argument("debuggee_args", nargs="*", metavar="-- ARGS...",
                   help="Args passed verbatim to the traced binary")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.assembly_cmd:
        case "search":
            _search(args, ctx)
        case "show":
            _show(args, ctx)
        case "trace":
            _trace(args, ctx)
        case _:  # argparse required=True should prevent this
            ctx.die(f"unknown assembly subcommand {args.assembly_cmd!r}")


# --- shared scope resolution --------------------------------------------------

def _target_patterns(specs: list[str] | None) -> list[str]:
    patterns: list[str] = []
    for spec in specs or []:
        patterns.extend(s.strip() for s in spec.split(",") if s.strip())
    return patterns


def _scan(args: argparse.Namespace, ctx: Context):
    """Resolve the preset, enumerate symbols across its objects, and return the pieces."""
    preset = ctx.resolve_presets(args.preset)[0]
    if not preset.build_dir.exists():
        ctx.die(f"no build at {ctx.rel(preset.build_dir)} - run: uv run dev.py build --preset {preset.name}")

    nm = disasm.find_tool("llvm-nm", preset.build_dir)
    objdump = disasm.find_tool("llvm-objdump", preset.build_dir)

    by_target = disasm.discover_objects(preset.build_dir, _target_patterns(args.target))
    if not by_target:
        scope = f" matching --target {', '.join(_target_patterns(args.target))}" if args.target else ""
        ctx.die(f"no object files found under {ctx.rel(preset.build_dir)}{scope}")

    symbols = disasm.enumerate_symbols(nm, by_target)
    return preset, objdump, by_target, symbols


def _target_meta(ctx: Context, preset) -> dict[str, tuple[str, int]]:
    """Best-effort map target -> (kind, artifact_size_bytes) from CMake discovery; {} on failure."""
    try:
        meta: dict[str, tuple[str, int]] = {}
        for t in ctx.discover(preset):
            size = t.artifact.stat().st_size if t.artifact and t.artifact.exists() else 0
            meta[t.name] = (t.kind, size)
        return meta
    except Exception:
        return {}


def _human_bytes(n: int) -> str:
    if n >= 1 << 20:
        return f"{n / (1 << 20):.2f} MB"
    if n >= 1 << 10:
        return f"{n / (1 << 10):.1f} kB"
    return f"{n} B"


# --- search -------------------------------------------------------------------

def _search(args: argparse.Namespace, ctx: Context) -> None:
    preset, _objdump, by_target, symbols = _scan(args, ctx)
    matches = disasm.match_symbols(symbols, args.pattern, regex=args.regex, text_only=not args.all)
    meta = _target_meta(ctx, preset)

    kind = "all symbols" if args.all else "functions"
    print(console.bold(f"'{args.pattern}' in {preset.name}") + console.dim(f"  ({kind})"))

    # Group matches by target, preserving discovery order.
    by_group: dict[str, list[disasm.Symbol]] = {}
    for s in matches:
        by_group.setdefault(s.target, []).append(s)
    total_in_target = {t: sum(1 for s in symbols if s.target == t) for t in by_target}

    printed = 0
    truncated = False
    for tgt in by_target:
        hits = by_group.get(tgt, [])
        if not hits:
            continue
        knd, art_size = meta.get(tgt, ("", 0))
        size = art_size or sum(o.stat().st_size for o in by_target[tgt] if o.exists())
        head = f"\n{tgt}"
        if knd:
            head += console.dim(f"  [{knd}]")
        head += f"  {_human_bytes(size)}" + console.dim(f"  ({total_in_target.get(tgt, 0)} symbols)")
        print(head)
        for s in hits:
            if printed >= args.limit:
                truncated = True
                break
            marker = "" if s.is_text else console.dim(" [data]")
            demangled = s.demangled if s.demangled != s.mangled else ""
            line = f"  {s.mangled}"
            if demangled:
                line += console.dim(f"  ({demangled})")
            print(line + marker)
            printed += 1
        if truncated:
            break

    obj_count = sum(len(o) for o in by_target.values())
    obj_bytes = sum(o.stat().st_size for objs in by_target.values() for o in objs if o.exists())
    summary = (
        f"\n{len(matches)} match(es)"
        + (f", showing {printed}" if truncated else "")
        + f"  |  searched {len(symbols)} symbols across {len(by_target)} target(s), "
        f"{obj_count} object files, {_human_bytes(obj_bytes)}"
    )
    print(console.dim(summary))
    if truncated:
        print(console.yellow(f"output limited to {args.limit} rows; raise --limit to see the rest"))


# --- show ---------------------------------------------------------------------

def _resolve_symbol(symbols: list[disasm.Symbol], query: str, ctx: Context) -> disasm.Symbol:
    """Find the single symbol to disassemble: exact match first, then a unique substring."""
    exact = [s for s in symbols if s.mangled == query or s.demangled == query]
    if exact:
        chosen = exact[0]
        copies = [s for s in exact if s.obj != chosen.obj]
        if copies:
            print(console.dim(f"note: '{query}' is defined in {len(exact)} objects (COMDAT); "
                              f"showing {ctx.rel(chosen.obj)}"))
        return chosen

    sub = [s for s in symbols if query.lower() in s.demangled.lower() or query.lower() in s.mangled.lower()]
    uniq = {s.mangled: s for s in sub}
    if len(uniq) == 1:
        only = next(iter(uniq.values()))
        print(console.dim(f"note: no exact match; using unique substring hit '{only.demangled}'"))
        return only
    if not sub:
        ctx.die(f"no symbol matches {query!r}. Try: uv run dev.py assembly search {query!r}")
    names = sorted({s.demangled for s in sub})[:12]
    ctx.die(f"{query!r} is ambiguous ({len(uniq)} symbols). Candidates:\n  " + "\n  ".join(names))


def _show(args: argparse.Namespace, ctx: Context) -> None:
    preset, objdump, _by_target, symbols = _scan(args, ctx)
    if not symbols:
        ctx.die("no symbols found to disassemble")
    sym = _resolve_symbol(symbols, args.symbol, ctx)

    raw = disasm.disassemble(
        objdump, sym.obj, sym.mangled,
        intel=not args.att, source=args.source, show_bytes=args.bytes,
    )

    print(console.bold(sym.demangled))
    meta = console.dim(f"  {sym.target}  --  {ctx.rel(sym.obj)}  --  {sym.mangled}")
    print(meta)
    if not args.att:  # label annotation is written for Intel operand order
        print(_format_intel(raw))
    else:
        print(raw.rstrip("\n"))
    if not args.source:
        print(console.dim("(cross-object calls are unresolved placeholders in object code; "
                          "pass --source on a relwithdebinfo preset to interleave source)"))


# --- Intel disassembly prettifier (labels + light color) ----------------------

_INSN = re.compile(r"^\s*([0-9a-fA-F]+):\s*\t?\s*(.*)$")
_RELOC = re.compile(r"REL|RELOC", re.IGNORECASE)
# The relocation type token, followed by the (demangled, via -C) target symbol.
_RELOC_TARGET = re.compile(r"(?:IMAGE_REL_\w+|R_[A-Z0-9_]+|ARM64\w*)\s+(.+?)\s*$")
# objdump framing we print our own header for: "file format", section banners, the symbol-address header.
_NOISE = re.compile(r"file format|Disassembly of section|^[0-9a-fA-F]+\s+<.*>:\s*$")
_BRANCH = re.compile(r"^(jmp|j[a-z]{1,3}|loop[a-z]*)$")
# Operand target like "0x40" possibly followed by "<sym+0x40>".
_TARGET = re.compile(r"\b0x([0-9a-fA-F]+)\b")


def _c(fn, s: str) -> str:
    return fn(s) if console.enabled() else s


def _reloc_target(line: str) -> str | None:
    m = _RELOC_TARGET.search(line)
    if not m:
        return None
    target = m.group(1)
    return re.sub(r"\s*[-+]\s*0x[0-9a-fA-F]+$", "", target)  # drop the addend (e.g. "-0x4")


def _format_intel(raw: str) -> str:
    """Add .L labels for local branches, fold relocations into their instruction, light color."""
    lines = raw.splitlines()

    # Pass 1: classify lines; collect instruction offsets and local branch targets.
    insns: list[tuple[int, str] | None] = []  # (offset, text) for instruction lines, else None
    offsets: set[int] = set()
    targets: set[int] = set()
    for line in lines:
        m = _INSN.match(line)
        if not m or _RELOC.search(line):
            insns.append(None)
            continue
        off = int(m.group(1), 16)
        text = m.group(2)
        offsets.add(off)
        insns.append((off, text))
        mnem = text.split(None, 1)[0] if text else ""
        if _BRANCH.match(mnem):
            tm = _TARGET.search(text)
            if tm:
                targets.add(int(tm.group(1), 16))

    local_targets = sorted(t for t in targets if t in offsets)
    label_of = {off: f".L{i}" for i, off in enumerate(local_targets)}

    # Pass 2: emit, inserting labels, folding a following relocation into the instruction.
    out: list[str] = []
    n = len(lines)
    i = 0
    while i < n:
        line, parsed = lines[i], insns[i]
        if parsed is None:
            if not _NOISE.search(line) and not _RELOC.search(line) and line.strip():
                out.append(line)  # orphan relocs are consumed by their instruction below
            i += 1
            continue

        off, text = parsed
        if off in label_of:
            out.append(_c(console.yellow, f"{label_of[off]}:"))

        # A relocation on the next line names this instruction's real cross-object target.
        reloc = None
        if i + 1 < n and insns[i + 1] is None and _RELOC.search(lines[i + 1]):
            reloc = _reloc_target(lines[i + 1])

        parts = text.split(None, 1)
        mnem = parts[0] if parts else ""
        rest = parts[1] if len(parts) > 1 else ""

        if _BRANCH.match(mnem):
            tm = _TARGET.search(rest)
            if tm and int(tm.group(1), 16) in label_of:
                rest = label_of[int(tm.group(1), 16)]
        elif reloc and mnem == "call":
            rest = reloc  # replace the bogus self-relative placeholder with the real callee
            reloc = None

        colored_mnem = mnem
        if console.enabled():
            if mnem == "lock" or mnem.startswith("lock"):
                colored_mnem = console.red(console.bold(mnem))
            elif _BRANCH.match(mnem) or mnem in ("call", "ret"):
                colored_mnem = console.yellow(mnem)
            else:
                colored_mnem = console.bold(mnem)
        if "#" in rest and console.enabled():  # dim a trailing "# ..." comment
            code, _, comment = rest.partition("#")
            rest = code + console.dim("#" + comment)
        if reloc:  # a non-call relocation (e.g. RIP-relative load of a global)
            rest = (rest + "  " if rest else "") + _c(console.dim, f"-> {reloc}")

        addr = _c(console.dim, f"{off:>6x}:")
        out.append(f"  {addr}  {colored_mnem}\t{rest}".rstrip())
        i += 2 if (i + 1 < n and insns[i + 1] is None and _RELOC.search(lines[i + 1])) else 1
    return "\n".join(out)


# --- trace --------------------------------------------------------------------

def _artifact_of(ctx: Context, preset, name: str) -> Path | None:
    """The built executable for target `name`, or None if it is not an executable target here."""
    for t in ctx.discover(preset):
        if t.name == name and t.kind == "EXECUTABLE" and t.artifact:
            return t.artifact
    return None


def _tracer_argv(args: argparse.Namespace, tracer: Path, exe: Path) -> list[str]:
    """Translate our flags into the tracer's CLI. Flags left at None are simply not passed, so the
    tracer's own defaults apply and we never restate them."""
    argv = [str(tracer), "--exe", str(exe)]

    if args.symbol:
        argv += ["--symbol", args.symbol]
    elif args.address:
        argv += ["--address", args.address]
    else:
        argv += ["--target", args.spec]

    for flag, value in (("--skip", args.skip), ("--traces", args.traces),
                        ("--instructions", args.instructions),
                        ("--sections", args.sections), ("--memory-regions", args.memory_regions)):
        if value is not None:
            argv += [flag, str(value)]

    for flag, value in (
        ("until-return", args.until_return),
        ("stop-at-syscall", args.stop_at_syscall),
        ("stack", args.stack),
        ("source", args.source),
        ("register-diffs", args.register_diffs),
        ("terminate-after-traces", args.terminate_after_traces),
        ("stats", args.stats),
        ("memory-instruction-addresses", args.memory_instruction_addresses),
    ):
        if value is not None:
            argv.append(f"--{flag}" if value else f"--no-{flag}")

    # dev.py already resolved the color question (--colored/--plain/auto); make its answer
    # authoritative rather than letting the child re-detect against an inherited terminal.
    argv.append("--colored" if console.enabled() else "--plain")

    if args.debuggee_args:
        argv += ["--"] + list(args.debuggee_args)

    return argv


def _trace(args: argparse.Namespace, ctx: Context) -> None:
    if platform.system() != "Windows":
        ctx.die("assembly trace needs the Win32 debug API — it is Windows-only")

    preset = ctx.resolve_presets(args.preset)[0]

    if not args.no_build:
        results = dev.build([preset], [TRACER_TARGET, args.target], root=ctx.root,
                            auto_configure=True, mirror=args.mirror_output, verbose=args.verbose)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])

    tracer = _artifact_of(ctx, preset, TRACER_TARGET)
    if tracer is None:
        ctx.die(f"{TRACER_TARGET} is not built for preset {preset.name!r} — it needs "
                f"SC_BUILD_TOOLS=ON, Windows, and the fetched Zydis "
                f"(uv run extern/zydis/fetch-zydis.py)")

    exe = _artifact_of(ctx, preset, args.target)
    if exe is None:
        ctx.die(f"no executable target named {args.target!r} in preset {preset.name!r} — "
                f"see: uv run dev.py list-targets")

    if "release" in preset.name:
        print(console.yellow(f"note: {preset.name} has no PDB; the trace will show raw addresses "
                             f"without symbols or source. Use a relwithdebinfo preset."), file=sys.stderr)

    argv = _tracer_argv(args, tracer, exe)
    if args.verbose:
        print(console.dim(f"  $ {' '.join(argv)}"), file=sys.stderr)

    # Streamed, not captured: a trace is the output the user came for, not a build log to file away.
    sys.exit(subprocess.run(argv, cwd=ctx.root).returncode)
