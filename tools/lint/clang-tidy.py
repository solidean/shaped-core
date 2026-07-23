#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///

"""clang-tidy gate runner for shaped-core.

The first tenant of the `tools/lint/` linting framework, and where the clang-tidy logic actually lives —
`dev.py lint clang-tidy` is thin wiring that shells out to this script (and `dev.py check` runs it dirty-only).
It is also usable standalone: `uv run tools/lint/clang-tidy.py --build-dir build/<preset>`.

It runs clang-tidy against the strict whitelist in tools/lint/clang-tidy-gates.yml — NOT the root .clang-tidy
(that one is the IDE incubator). That file is our own schema (`gates`, each with a `why`), which this script
translates into clang-tidy's `-*,<gates>` config (passed via `--config`, so the root .clang-tidy is never
auto-discovered). Every whitelisted check is a gate: `--warnings-as-errors=*` makes any firing check a
non-zero exit, so `check` can treat a green run as "safe to commit".

Only `.cc` translation units are linted; the compilation database has no entry for a bare header.
First-party headers still surface diagnostics via `--header-filter` (they are seen through the `.cc` that
includes them), but a dirty `.hh` with no dirty `.cc` in scope currently lints nothing.
Mapping a changed header back to the TUs that include it is future work.

Files are linted in parallel — one clang-tidy invocation per `.cc` across a thread pool (`-j`, default CPU
count), since the built-in single-process driver is strictly sequential. `--fix` forces one worker so two
TUs can't rewrite a shared header at once.

Output is bounded: diagnostics print verbatim while under `--limit` lines; past it they collapse into a
grouped-by-check digest so every failing check stays visible even when there are thousands of hits.
Progress prints as each file finishes.

Reuses tools/dev machinery (git dirty-file discovery, the compilation-database reader, LLVM-tool location)
by putting the repo root on sys.path.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import tools.dev as dev  # noqa: E402

GATES_CONFIG = ROOT / "tools" / "lint" / "clang-tidy-gates.yml"

# One clang-tidy diagnostic anchor line, e.g.
#   C:\path\file.cc:11:1: warning: use 'using' instead of 'typedef' [modernize-use-using,-warnings-as-errors]
# The leading path can itself contain a colon on Windows ("C:\..."), so the non-greedy capture stops at the
# first ":<line>:<col>:" — the real separator. Notes and the code/caret lines carry no `[check]`, so they
# don't match and are skipped. The check name is the first token inside the trailing bracket.
_DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s+(?:error|warning):\s+"
    r"(?P<msg>.*?)\s*\[(?P<check>[a-zA-Z0-9.\-]+)[^\]]*\]\s*$",
    re.MULTILINE,
)


def _load_gates() -> dict:
    """Parse the gates config, exiting with a clean message on a read/parse error."""
    try:
        with open(GATES_CONFIG, encoding="utf-8") as f:
            return yaml.safe_load(f) or {}
    except (OSError, yaml.YAMLError) as e:
        print(dev.console.red(f"ERROR: cannot read {dev.report.rel(GATES_CONFIG, ROOT)}: {e}"), file=sys.stderr)
        sys.exit(2)


def _enabled_entries(data: dict, *, include_incubator: bool) -> list[dict]:
    """The check entries that are active this run: `gates`, plus `incubator` when previewing."""
    entries = list(data.get("gates", []))
    if include_incubator:
        entries += data.get("incubator", [])
    return entries


def _rationales(data: dict, *, include_incubator: bool) -> dict[str, str]:
    """Map each active check to its `why` (rationale, often carrying a fix hint) for the grouped digest."""
    return {e["check"]: e.get("why", "") for e in _enabled_entries(data, include_incubator=include_incubator)
            if e.get("check")}


def _tidy_config_arg(data: dict, *, include_incubator: bool = False) -> str:
    """Translate our gates schema into clang-tidy's `--config=` argument.

    Only `gates` and `options` reach clang-tidy: the enabled checks become `-*,<check>,...` (a strict
    whitelist), the options become `CheckOptions`. With `include_incubator` the `incubator` candidates are
    added too — a preview of what promoting them would flag, not a gate. `excluded` is always documentation
    and ignored. Passing `--config` (rather than a config file) also stops clang-tidy from auto-discovering
    the root .clang-tidy.
    """
    checks = ["-*"] + [g["check"] for g in _enabled_entries(data, include_incubator=include_incubator)]
    options = []
    for o in data.get("options", []):
        v = o["value"]
        v = "true" if v is True else "false" if v is False else str(v)
        options.append({"key": o["key"], "value": v})

    config: dict = {"Checks": ",".join(checks)}
    if options:
        config["CheckOptions"] = options
    return "--config=" + json.dumps(config)


def _default_build_dir() -> Path | None:
    """The platform default preset's build dir, so the script runs without an explicit --build-dir.

    Reads dev.py's policy table rather than hard-coding a second copy of the preset names.
    Imported lazily: the common path (invoked by dev.py with an explicit --build-dir) never needs it.
    """
    import dev as dev_cli  # repo-root dev.py, for its default-preset policy table

    name = dev_cli.DEFAULT_BUILD_PRESETS.get(platform.system())
    if name is None:
        return None
    presets = dev.resolve_presets(ROOT, [name])
    return presets[0].build_dir if presets else None


def _header_filter_regex() -> str:
    """A `--header-filter` regex that matches first-party headers only.

    Built from the same source roots clang-format owns (libs/, tools/instruction-tracer/), so header
    diagnostics surface while extern/ and system headers stay quiet. Matches either path separator.
    """
    segments = []
    for r in dev.source_roots(ROOT):
        rel = r.relative_to(ROOT).as_posix()
        segments.append(rel.replace("/", r"[/\\]"))
    return r".*[/\\](?:" + "|".join(segments) + r")[/\\].*"


def _parse_diagnostics(text: str) -> dict[str, list[str]]:
    """Group clang-tidy's diagnostics by check name.

    Returns an insertion-ordered {check: ["relpath:line:col  message", ...]}, de-duplicated so a header
    diagnostic seen through many `.cc` collapses to one entry.
    """
    groups: dict[str, list[str]] = {}
    seen: set[tuple[str, str]] = set()
    for m in _DIAG_RE.finditer(text):
        check = m.group("check")
        rel = dev.report.rel(Path(m.group("file")), ROOT)
        loc = f"{rel}:{m.group('line')}:{m.group('col')}"
        key = (check, loc)
        if key in seen:
            continue
        seen.add(key)
        groups.setdefault(check, []).append(f"{loc}  {m.group('msg').strip()[:100]}")
    return groups


def _render_grouped(groups: dict[str, list[str]], limit: int, rationales: dict[str, str]) -> list[str]:
    """A bounded, grouped digest of the diagnostics — one section per check, kept within ~`limit` lines.

    Every failing check stays visible (at least one file each, even past the limit); the remaining line
    budget is spread across checks round-robin, so small checks show in full and the largest are truncated
    with a `... N more` marker. Checks are ordered by failure count, descending. Each section leads with the
    check's `why` from the gates config (the rationale, often with a fix hint) so it's clear how to resolve.
    """
    ordered = sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    n_checks = len(ordered)
    # Headers cost one line each; reserve another line per check for a possible `... more` marker. The floor
    # keeps the >=1-file-per-check guarantee even when there are more checks than the limit allows.
    files_budget = max(n_checks, limit - 2 * n_checks)

    alloc = {check: 0 for check, _ in ordered}
    remaining = files_budget
    progressed = True
    while remaining > 0 and progressed:
        progressed = False
        for check, items in ordered:
            if alloc[check] < len(items):
                alloc[check] += 1
                remaining -= 1
                progressed = True
                if remaining == 0:
                    break

    lines: list[str] = []
    for check, items in ordered:
        lines.append(dev.console.bold(f"[{check}]") + dev.console.dim(f"  ({len(items)})"))
        why = rationales.get(check)
        if why:
            lines.append(dev.console.dim(f"  why: {why}"))
        shown = alloc[check]
        lines.extend(f"  - {item}" for item in items[:shown])
        if shown < len(items):
            lines.append(dev.console.dim(f"  ... {len(items) - shown} more"))
    return lines


def _sources_in_scope(build_dir: Path, *, dirty_only: bool, explicit: list[str]) -> list[Path]:
    """The `.cc` files to lint: either an explicit list or discovery, intersected with the compile DB.

    A file with no compile_commands.json entry (generated, or excluded by the active backend) can't be
    linted, so it is dropped rather than erroring.
    """
    if explicit:
        files = [Path(f).resolve() for f in explicit]
    else:
        files = [f for f in dev.discover_files(ROOT, dirty_only=dirty_only) if f.suffix == ".cc"]

    try:
        entries = dev.load_entries(build_dir)
    except FileNotFoundError as e:
        print(dev.console.red(f"ERROR: {e}"), file=sys.stderr)
        print("Configure/build the preset first (e.g. `uv run dev.py build`).", file=sys.stderr)
        sys.exit(2)

    known = {os.path.normcase(os.path.normpath(e.get("file", ""))) for e in entries}
    return [f for f in files if os.path.normcase(os.path.normpath(str(f))) in known]


def _run_over_files(base_cmd: list[str], files: list[Path], *, jobs: int) -> tuple[int, str, str]:
    """Run clang-tidy once per file across a thread pool; return (returncode, combined_stdout, stderr).

    Each file is an independent invocation — clang-tidy is CPU-bound per translation unit, so fanning out
    over `jobs` workers scales with cores (the built-in single-process driver is strictly sequential). Its
    diagnostics land on stdout, its per-file chatter on stderr; we collect both and OR the exit codes.
    Progress prints as each file finishes.
    """
    import concurrent.futures

    total = len(files)
    stdout_parts: list[str] = []
    stderr_parts: list[str] = []
    returncode = 0
    done = 0

    def one(f: Path) -> subprocess.CompletedProcess:
        return subprocess.run(base_cmd + [str(f)], cwd=str(ROOT), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = {ex.submit(one, f): f for f in files}
        for fut in concurrent.futures.as_completed(futures):
            f = futures[fut]
            r = fut.result()
            done += 1
            print(dev.console.dim(f"  [{done}/{total}] {dev.report.rel(f, ROOT)}"), file=sys.stderr)
            if r.stdout:
                stdout_parts.append(r.stdout)
            if r.stderr:
                stderr_parts.append(r.stderr)
            if r.returncode != 0:
                returncode = 1
    return returncode, "".join(stdout_parts), "".join(stderr_parts)


def main() -> None:
    # Emit UTF-8 regardless of the Windows console codepage: our prose (and clang-tidy's) carries em dashes,
    # and dev.py's run_step reads this process's pipes as UTF-8 — a codepage mismatch would mangle them.
    # Rewrap the raw byte buffers directly; reconfigure() reports success but doesn't take under `uv run`.
    import io
    for name in ("stdout", "stderr"):
        buffer = getattr(getattr(sys, name), "buffer", None)
        if buffer is not None:
            setattr(sys, name, io.TextIOWrapper(buffer, encoding="utf-8", errors="replace", line_buffering=True))

    parser = argparse.ArgumentParser(description="Run the clang-tidy gates over shaped-core C++ sources.")
    parser.add_argument("--build-dir", metavar="DIR", default=None,
                        help="Directory holding compile_commands.json (default: the platform preset's build dir)")
    parser.add_argument("--dirty-only", action="store_true",
                        help="Only lint git-dirty/untracked .cc sources (the next commit's files)")
    parser.add_argument("--fix", action="store_true",
                        help="Let clang-tidy apply its fixes in place")
    parser.add_argument("--include-incubator", action="store_true",
                        help="Also run the incubator candidates from clang-tidy-gates.yml — a preview of "
                             "what promoting them would flag (not a gate; does not affect the gate result)")
    parser.add_argument("--limit", type=int, default=200, metavar="N",
                        help="Max diagnostic lines to print verbatim; past N, print a grouped-by-check "
                             "digest instead so every failing check stays visible (default: 200)")
    parser.add_argument("-j", "--jobs", type=int, default=None, metavar="N",
                        help="Parallel clang-tidy invocations (default: CPU count). --fix forces 1 to avoid "
                             "concurrent edits to a shared header.")
    parser.add_argument("files", nargs="*", help="Specific .cc files to lint (default: discover)")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve() if args.build_dir else _default_build_dir()
    if build_dir is None:
        print(dev.console.red(f"ERROR: no default preset for {platform.system()!r}; pass --build-dir."),
              file=sys.stderr)
        sys.exit(2)

    clang_tidy = dev.resolve_tool("clang-tidy", "CLANG_TIDY", build_dir)
    if clang_tidy is None:
        print(dev.console.red("ERROR: clang-tidy not found (env CLANG_TIDY, PATH, or beside the compiler)."),
              file=sys.stderr)
        sys.exit(2)

    files = _sources_in_scope(build_dir, dirty_only=args.dirty_only, explicit=args.files)
    if not files:
        print(dev.console.green("clang-tidy: nothing to lint (no .cc sources in scope)"), file=sys.stderr)
        sys.exit(0)

    gates = _load_gates()
    base_cmd = [
        clang_tidy,
        "-p", str(build_dir),
        _tidy_config_arg(gates, include_incubator=args.include_incubator),
        f"--header-filter={_header_filter_regex()}",
        "--warnings-as-errors=*",  # every whitelisted check is a gate: any firing -> non-zero exit
        "--quiet",  # suppress the per-file "N warnings generated" chatter — ×N files is just noise
    ]
    if args.fix:
        base_cmd.append("--fix")

    # --fix in parallel would let two TUs rewrite a shared header at once; force one worker there.
    jobs = 1 if args.fix else (args.jobs or os.cpu_count() or 4)
    print(dev.console.dim(
        f"clang-tidy: {len(files)} file(s) against {dev.report.rel(GATES_CONFIG, ROOT)} ({jobs} job(s))"),
        file=sys.stderr)
    returncode, out, err = _run_over_files(base_cmd, files, jobs=jobs)

    # Everything the runner emits goes to stderr — the same stream as the per-file progress and the verdict —
    # so ordering is deterministic even under dev.py's run_step (which pumps stdout/stderr on separate threads).
    diag_lines = out.splitlines()
    if not out.strip():
        # No lint diagnostics. If a TU still failed (e.g. a genuine compile error), surface its stderr.
        if returncode != 0 and err.strip():
            sys.stderr.write(err if err.endswith("\n") else err + "\n")
    elif len(diag_lines) <= args.limit:
        sys.stderr.write(out if out.endswith("\n") else out + "\n")  # under budget: verbatim detail
    else:
        groups = _parse_diagnostics(out)
        total = sum(len(v) for v in groups.values())
        print(dev.console.dim(
            f"clang-tidy: {total} diagnostic(s) across {len(groups)} check(s) - "
            f"grouped digest (raw output exceeded {args.limit} lines):"), file=sys.stderr)
        rationales = _rationales(gates, include_incubator=args.include_incubator)
        print("\n".join(_render_grouped(groups, args.limit, rationales)), file=sys.stderr)

    if returncode == 0:
        print(dev.console.green("clang-tidy: OK"), file=sys.stderr)
    else:
        print(dev.console.red("clang-tidy: FAIL (gate violations above)"), file=sys.stderr)
    sys.exit(returncode)


if __name__ == "__main__":
    main()
