"""Measure what the compiler spends its time on, per header and per translation unit.

Two questions, one engine:

- **header cost** — compile a synthetic TU whose entire body is `#include <that header>`.
  This deliberately measures the header's *transitive closure*.
  A trivial header that pulls in the world is exactly what it is meant to expose.
- **TU cost** — compile the real TU, and separately a copy reduced to its preprocessor directives.
  The two wall clocks then bracket how much of a file is its includes.
- **PCH value** — compile the real TU with the precompiled header its target is configured for, and again without it.
  The ratio is what that target's tier is currently worth.

Everything is end-to-end wall clock of a real `clang-cl` / `cl` / `clang` invocation with the target's real flags, because that is the number a build actually pays.
`-ftime-trace` runs alongside and its frontend / backend / source split is folded into each record, but it never replaces the wall clock.
It costs ~8 % to collect, and it cannot separate work done in a TU's own body from work done in its headers.

Four rules this module exists to enforce, each of which silently corrupts the measurement when broken:

- **Flags are inserted before the `--` separator.** CMake ends a compile command with `-c -- <source>`, and anything appended lands past it and is read as an input file.
- **The target's precompiled-header flags are stripped.** Otherwise a header is measured against a closure that is already deserialized, and every header reads as ~0.03 s.
- **Objects are written to a scratch directory.** Reusing the entry's `/Fo` overwrites the real build's objects.
- **`-fsyntax-only` is not used.**
  Headers do real backend work through inline functions, template instantiations and static initializers.
  Dropping codegen would under-report exactly the headers worth finding.

Public API:
    measure_headers(headers, ...) -> list[Record]
    measure_tus(sources, ...)     -> list[Record]
    measure_pch(sources, ...)     -> list[Record]
    baseline(entry, ...)          -> Record
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from ..core import console
from ..project import compdb

# A directive line, possibly indented.
# Continuation lines (trailing backslash) are joined onto it.
_DIRECTIVE = re.compile(r"^\s*#")

# A raw string literal, delimiter and all.
# Test fixtures hold OBJ and markdown content whose lines start with '#', and those are not directives.
# Scanning without removing raw strings first picks up '# a single triangle' and hands it to the compiler.
_RAW_STRING = re.compile(r'R"([^()\\\s]{0,16})\(.*?\)\1"', re.DOTALL)
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)

# Pragmas are dropped rather than kept: none of them changes which headers get included, and one that
# must attach to a statement (#pragma clang loop ...) is a syntax error standing on its own.
_PRAGMA = re.compile(r"^\s*#\s*pragma\b")


@dataclass
class Record:
    """One measured compile.

    `kind` is "header", "tu-full", "tu-includes" or "baseline", and `path` is the file it describes — the real header or source, never the synthetic TU.
    `runs` holds every repeat's wall clock; `wall_s` is their minimum, which is the low-noise estimator for a compiler benchmark.
    `trace` is the -ftime-trace summary, empty when tracing was off or the trace could not be read.
    """

    path: str
    kind: str
    target: str = ""
    flags_from: str = ""
    ok: bool = True
    error: str = ""
    runs: list[float] = field(default_factory=list)
    trace: dict = field(default_factory=dict)

    @property
    def wall_s(self) -> float:
        return min(self.runs) if self.runs else 0.0

    def as_dict(self) -> dict:
        d = {
            "path": self.path,
            "kind": self.kind,
            "target": self.target,
            "flags_from": self.flags_from,
            "ok": self.ok,
            "wall_s": round(self.wall_s, 4),
            "runs": [round(r, 4) for r in self.runs],
        }
        if self.trace:
            d["trace"] = self.trace
        if not self.ok:
            d["error"] = self.error
        return d


# ---------------------------------------------------------------------------
# Command construction
# ---------------------------------------------------------------------------

def retarget(entry: dict, source: Path, obj: Path, extra: list[str], *, keep_pch: bool = False) -> list[str]:
    """The entry's compile command, rewritten to compile `source` into `obj`, with `extra` inserted safely.

    `extra` goes before the `--` separator, since everything after it is an input file rather than a flag.
    A command with no separator gets `extra` appended, which is equivalent there.

    The target's precompiled-header flags are stripped first, and that is a correctness requirement rather than tidiness.
    Left in, a header measurement compiles a synthetic TU whose whole transitive closure is already deserialized and reports ~0.03 s for every header — a plausible number, not an error.
    So `headers` and `tu` always measure parsing from source, whether or not the preset builds with a PCH.
    `keep_pch` is the one exception, for the `pch` mode, whose entire question is what those flags are worth.
    """
    argv = compdb.split_command(entry)
    if not keep_pch:
        argv = compdb.strip_pch_flags(argv)
    out: list[str] = []
    placed = False
    for tok in argv:
        low = tok.lower()
        if low.startswith("/fo") or low.startswith("-fo"):
            out.append(tok[:3] + str(obj))
        elif low.startswith("/fd"):
            out.append(tok[:3] + str(obj.with_suffix(".pdb")))
        elif tok == "-o":
            out.append(tok)  # the value is replaced when we reach it below
        elif tok == "--":
            out.extend(extra)
            placed = True
            out.append(tok)
        elif low.endswith((".cc", ".cpp", ".cxx", ".c")):
            out.append(str(source))
        else:
            out.append(tok)
    # GCC-style `-o <path>`: the token after -o is the object, and the loop above left it alone.
    for i, tok in enumerate(out):
        if tok == "-o" and i + 1 < len(out):
            out[i + 1] = str(obj)
    if not placed:
        out.extend(extra)
    return out


# ---------------------------------------------------------------------------
# -ftime-trace
# ---------------------------------------------------------------------------

def _trace_flags(family: str) -> list[str]:
    """The pass-through form of -ftime-trace for this compiler family, empty when unsupported.

    clang-cl needs the `/clang:` prefix; MSVC has no equivalent, so its records simply carry no trace.
    """
    if family == "msvc":
        return []
    return ["/clang:-ftime-trace"] if os.name == "nt" and family == "clang" else ["-ftime-trace"]


# Per-header self time below this is dropped from `headers`, with the total kept as `headers_omitted_s`.
# A top-N cap would be the obvious alternative and is the wrong one: aggregating cost across every TU is the
# whole point of the per-header table, and a cap silently truncates the long tail that aggregation sums up.
_HEADER_FLOOR_S = 0.0005


def _summarize_trace(path: Path, floor_s: float = _HEADER_FLOOR_S) -> dict:
    """Reduce a -ftime-trace capture to the fields worth keeping per record.

    Source spans are async begin/end pairs keyed by id and nested by inclusion, so self time is inclusive minus directly-nested children.
    `ours_source_s` versus `system_source_s` is the split that decides whether include hygiene on our own headers can pay at all.
    """
    try:
        events = json.loads(path.read_text(encoding="utf-8"))["traceEvents"]
    except (OSError, ValueError, KeyError):
        return {}

    totals: dict[str, float] = {}
    for e in events:
        name = e.get("name", "")
        if name.startswith("Total ") or name == "ExecuteCompiler":
            totals[name] = e.get("dur", 0) / 1e6

    # Pair the Source spans, then compute each file's self time.
    stacks: dict[int, list[tuple[str, int]]] = {}
    spans: list[tuple[str, int, int]] = []
    for e in sorted((x for x in events if x.get("cat") == "Source"), key=lambda x: x["ts"]):
        if e["ph"] == "b":
            stacks.setdefault(e["id"], []).append((e["args"]["detail"], e["ts"]))
        elif e["ph"] == "e" and stacks.get(e["id"]):
            name, start = stacks[e["id"]].pop()
            spans.append((name, start, e["ts"]))

    spans.sort(key=lambda s: (s[1], -s[2]))
    self_s: dict[str, float] = {}
    for i, (name, start, end) in enumerate(spans):
        covered, cursor = 0, start
        for _, c_start, c_end in spans[i + 1:]:
            if c_start >= end:
                break
            if c_start >= cursor:
                covered += c_end - c_start
                cursor = c_end
        self_s[name] = self_s.get(name, 0.0) + (end - start - covered) / 1e6

    def is_system(p: str) -> bool:
        low = p.lower()
        return "program files" in low or "windows kits" in low or "/usr/" in low or "\\vc\\tools\\" in low

    kept = {k: v for k, v in self_s.items() if v >= floor_s}
    omitted = [v for v in self_s.values() if v < floor_s]

    return {
        "execute_compiler_s": round(totals.get("ExecuteCompiler", 0.0), 4),
        "frontend_s": round(totals.get("Total Frontend", 0.0), 4),
        "backend_s": round(totals.get("Total Backend", 0.0), 4),
        "source_s": round(totals.get("Total Source", 0.0), 4),
        "instantiate_s": round(totals.get("Total InstantiateFunction", 0.0)
                               + totals.get("Total InstantiateClass", 0.0), 4),
        "include_count": len(self_s),
        "ours_source_s": round(sum(v for k, v in self_s.items() if not is_system(k)), 4),
        "system_source_s": round(sum(v for k, v in self_s.items() if is_system(k)), 4),
        "headers_omitted": len(omitted),
        "headers_omitted_s": round(sum(omitted), 4),
        "headers": [
            {"file": k.replace("\\", "/"), "self_s": round(v, 5), "system": is_system(k)}
            for k, v in sorted(kept.items(), key=lambda kv: -kv[1])
        ],
    }


# ---------------------------------------------------------------------------
# TU synthesis
# ---------------------------------------------------------------------------

def header_tu(header: Path) -> str:
    """A TU whose whole body is one include, so its cost is that header's transitive closure."""
    return f'#include "{header.as_posix()}"\n'


def includes_only_tu(source: Path) -> str:
    """`source` reduced to its preprocessor directives, everything else dropped.

    Keeping every directive rather than only `#include` lines is what makes conditional includes resolve the same way.
    An include guarded by `#if` needs its `#if`, and the `#define` the condition reads.
    Continuation lines are joined so a multi-line macro survives intact.
    Raw string literals and block comments are removed first, since a fixture holding OBJ or markdown content has lines that open with `#` and are not directives at all.
    This is a heuristic.
    A directive whose expansion referenced dropped code still compiles, since nothing expands it, and a file that fails this way is reported rather than silently skipped.
    """
    text = source.read_text(encoding="utf-8", errors="replace")
    # Blanked to the same line count, so a directive after a multi-line literal still stands on its own line.
    text = _RAW_STRING.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = _BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    lines = text.splitlines()
    kept: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if _DIRECTIVE.match(line):
            keep = not _PRAGMA.match(line)
            if keep:
                kept.append(line)
            while line.rstrip().endswith("\\") and i + 1 < len(lines):
                i += 1
                line = lines[i]
                if keep:
                    kept.append(line)
        i += 1
    return "\n".join(kept) + "\n"


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def _run_once(argv: list[str], cwd: str, env: dict[str, str] | None) -> tuple[float, int, str]:
    start = time.perf_counter()
    proc = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
    return time.perf_counter() - start, proc.returncode, proc.stderr[-4000:]


def _measure(
    entry: dict,
    source: Path,
    *,
    path: str,
    kind: str,
    target: str,
    scratch: Path,
    repeat: int,
    time_trace: bool,
    family: str,
    env: dict[str, str] | None,
    extra_flags: list[str] | None = None,
    keep_pch: bool = False,
) -> Record:
    """Compile `source` `repeat` times with `entry`'s flags, keeping the fastest.

    The trace is collected on the LAST run only: it costs ~8 % and would otherwise inflate every sample.
    """
    rec = Record(path=path, kind=kind, target=target, flags_from=entry["file"].replace("\\", "/"))
    # Keyed on kind too: a TU's full and includes-only measurements would otherwise share an object,
    # and so share the trace file clang names after it.
    obj = scratch / f"{kind}_{abs(hash(path)):x}.obj"

    for i in range(repeat):
        last = i == repeat - 1
        extra = list(extra_flags or [])
        if time_trace and last:
            extra += _trace_flags(family)
        argv = retarget(entry, source, obj, extra, keep_pch=keep_pch)
        elapsed, code, err = _run_once(argv, entry["directory"], env)
        if code != 0:
            rec.ok = False
            rec.error = err
            return rec
        rec.runs.append(elapsed)

    if time_trace:
        # clang names the trace after the object, ignoring -ftime-trace-file in clang-cl mode.
        trace_path = obj.with_suffix(".json")
        if trace_path.is_file():
            rec.trace = _summarize_trace(trace_path)

    # Scratch is cleared per measurement, not at the end.
    # A full sweep is ~1000 compiles, and letting their objects and multi-megabyte traces pile up costs
    # several GB of file churn that the filesystem and any on-access scanner charge back to later compiles:
    # left in place, the same TU measures ~3x slower at the end of a 483-file sweep than on its own.
    for leftover in (obj, obj.with_suffix(".pdb"), obj.with_suffix(".json")):
        leftover.unlink(missing_ok=True)
    return rec


def baseline(
    entry: dict, *, scratch: Path, repeat: int, time_trace: bool, family: str,
    env: dict[str, str] | None,
) -> Record:
    """Cost of an empty TU with the same flags — compiler startup, prelude and object writing.

    Recorded rather than subtracted: what counts as the floor depends on the question being asked, so the consumer decides.
    """
    empty = scratch / "_baseline.cc"
    empty.write_text("\n", encoding="utf-8")
    return _measure(entry, empty, path="<empty>", kind="baseline", target="",
                    scratch=scratch, repeat=repeat, time_trace=time_trace, family=family, env=env)


def measure_headers(
    headers: list[tuple[Path, dict, str]], *, scratch: Path, repeat: int, time_trace: bool,
    family: str, env: dict[str, str] | None, root: Path, progress: bool = True,
) -> list[Record]:
    """Measure each (header, borrowed compile entry, target) as its own synthetic TU."""
    out: list[Record] = []
    for n, (header, entry, target) in enumerate(headers, 1):
        tu = scratch / f"inc_{abs(hash(str(header))):x}.cc"
        tu.write_text(header_tu(header), encoding="utf-8")
        rel = _rel(header, root)
        if progress:
            print(console.dim(f"  [{n}/{len(headers)}] {rel}"), file=sys.stderr)
        out.append(_measure(entry, tu, path=rel, kind="header", target=target, scratch=scratch,
                            repeat=repeat, time_trace=time_trace, family=family, env=env))
    return out


def measure_tus(
    sources: list[tuple[Path, dict, str]], *, scratch: Path, repeat: int, time_trace: bool,
    family: str, env: dict[str, str] | None, root: Path, progress: bool = True,
) -> list[Record]:
    """Measure each TU twice: as-is, and reduced to its preprocessor directives.

    Both run back to back on the same machine state, which is the point.
    Comparing a full TU against an includes-only TU from two separate invocations would be swamped by the ~8 % run-to-run variance a build shows.
    """
    out: list[Record] = []
    for n, (source, entry, target) in enumerate(sources, 1):
        rel = _rel(source, root)
        if progress:
            print(console.dim(f"  [{n}/{len(sources)}] {rel}"), file=sys.stderr)
        out.append(_measure(entry, source, path=rel, kind="tu-full", target=target, scratch=scratch,
                            repeat=repeat, time_trace=time_trace, family=family, env=env))

        stripped = scratch / f"inc_{abs(hash(str(source))):x}.cc"
        stripped.write_text(includes_only_tu(source), encoding="utf-8")
        # A quoted include resolves against the directory of the file containing it, and the stripped
        # copy does not live there — so the original's directory has to join the search path explicitly.
        out.append(_measure(entry, stripped, path=rel, kind="tu-includes", target=target, scratch=scratch,
                            repeat=repeat, time_trace=time_trace, family=family, env=env,
                            extra_flags=[f"-I{source.parent}"]))
    return out


def measure_pch(
    sources: list[tuple[Path, dict, str]], *, scratch: Path, repeat: int, family: str,
    env: dict[str, str] | None, root: Path, progress: bool = True,
) -> list[Record]:
    """Measure each TU twice: with the precompiled header its target is configured for, and without it.

    What this answers is "is this target's tier paying off", not "which tier is best".
    The tiers themselves live in tools/cmake/PrecompiledHeaders.cmake and are deliberately not duplicated here — to compare two tiers, change the one in the CMake module and measure again.

    The preset must be BUILT, not merely configured: the flags name a `.pch` that the target's own build produces.
    A preset with CMAKE_DISABLE_PRECOMPILE_HEADERS=ON carries no PCH flags at all, so both halves measure the same thing and every ratio reads 1.00 — which is the expected answer there, not a bug.

    Both halves run back to back on the same machine state, for the reason measure_tus does it: a ratio taken across two invocations would be swamped by the ~8 % run-to-run variance a build shows.
    """
    out: list[Record] = []
    for n, (source, entry, target) in enumerate(sources, 1):
        rel = _rel(source, root)
        if progress:
            print(console.dim(f"  [{n}/{len(sources)}] {rel}"), file=sys.stderr)
        out.append(_measure(entry, source, path=rel, kind="tu-pch", target=target, scratch=scratch,
                            repeat=repeat, time_trace=False, family=family, env=env, keep_pch=True))
        out.append(_measure(entry, source, path=rel, kind="tu-nopch", target=target, scratch=scratch,
                            repeat=repeat, time_trace=False, family=family, env=env))
    return out


def _rel(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()
