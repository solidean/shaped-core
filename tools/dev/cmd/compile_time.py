"""`compile-time` — measure what the compiler spends its time on.

    compile-time headers <glob>...   what including a header costs, in a TU that does nothing else
    compile-time tu <glob>...        what a TU costs, full and reduced to its includes

Both report end-to-end wall clock of a real compile with the target's real flags, and fold each run's `-ftime-trace` split into the JSON.
Serial by default, because a parallel measurement measures the machine rather than the code.

docs/guides/compile-times.md is the workflow; tools/dev/lib/perf/compile_time.py owns the mechanism and the traps it avoids.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import sys
import tempfile
from datetime import datetime
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.perf import compile_select, compile_time as ct

from . import args as a
from .context import Context

NAME = "compile-time"

_DEFAULT_OUT = Path(".tmp") / "compile-time"


def _common(p: argparse.ArgumentParser) -> None:
    a.preset(p)
    a.build_overrides(p)
    a.profile(p)
    p.add_argument("patterns", nargs="+", metavar="GLOB",
                   help="File(s) to measure: paths, globs, or a directory (repeatable)")
    p.add_argument("--target", action="append",
                   help="Restrict to target(s): comma-list, repeatable, wildcards")
    p.add_argument("--repeat", type=int, default=1, metavar="N",
                   help="Compile each file N times and keep the fastest (default: 1). "
                        "The minimum is the low-noise estimator; use 3+ when comparing small differences.")
    p.add_argument("--out", metavar="FILE",
                   help=f"Write the JSON report here (default: {_DEFAULT_OUT.as_posix()}/<mode>-<preset>.json)")
    p.add_argument("--top", type=int, default=20, metavar="K",
                   help="How many rows the stdout summary shows (default: 20)")
    p.add_argument("--no-time-trace", action="store_true",
                   help="Skip -ftime-trace. It costs ~8%% of the compile to collect, so drop it when "
                        "the absolute wall clock matters more than the frontend/backend split.")
    p.add_argument("--keep-tus", metavar="DIR",
                   help="Keep the synthetic TUs here instead of a temp dir, to inspect what was measured")


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Measure header and translation-unit compile times")
    csub = p.add_subparsers(dest="compile_time_cmd", required=True)

    h = csub.add_parser("headers", help="Cost of including a header, in a TU that does nothing else")
    _common(h)

    t = csub.add_parser("tu", help="Cost of a TU, measured full and reduced to its includes")
    _common(t)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    mode = args.compile_time_cmd
    targets = [s.strip() for spec in (args.target or []) for s in spec.split(",") if s.strip()] or None

    scratch_ctx = None
    if args.keep_tus:
        scratch = Path(args.keep_tus)
        scratch.mkdir(parents=True, exist_ok=True)
    else:
        scratch_ctx = tempfile.TemporaryDirectory(prefix="dev-compile-time-")
        scratch = Path(scratch_ctx.name)

    report = {
        "schema": "compile-time/1",
        "mode": mode,
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "machine": {"platform": platform.platform(), "cpu_count": os.cpu_count()},
        "settings": {
            "repeat": args.repeat,
            "serial": True,
            "time_trace": not args.no_time_trace,
            "patterns": args.patterns,
            "targets": targets,
        },
        "presets": [],
    }

    try:
        for preset in presets:
            report["presets"].append(_measure_preset(args, ctx, preset, mode, targets, scratch))
    finally:
        if scratch_ctx is not None:
            scratch_ctx.cleanup()

    out = Path(args.out) if args.out else _DEFAULT_OUT / f"{mode}-{presets[0].name}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    ok = _summarize(report, ctx, out, args.top)
    sys.exit(0 if ok else 1)


def _measure_preset(args, ctx: Context, preset, mode: str, targets, scratch: Path) -> dict:
    """Configure if needed, resolve what to measure, and run the whole set for one preset."""
    dev.ensure_configured(preset, root=ctx.root, mirror=args.mirror_output, verbose=args.verbose)
    try:
        entries = dev.load_entries(preset.build_dir)
    except FileNotFoundError as e:
        ctx.die(str(e))

    if mode == "headers":
        selected = compile_select.resolve_headers(args.patterns, entries, ctx.root, targets=targets)
        noun = "header"
    else:
        selected = compile_select.resolve_sources(args.patterns, entries, ctx.root, targets=targets)
        noun = "TU"
    if not selected:
        ctx.die(f"no {noun}(s) matched {' '.join(args.patterns)!r} in preset {preset.name!r}")

    env = dev.env_for_preset(preset)
    family = preset.family
    trace = not args.no_time_trace
    n_compiles = len(selected) * args.repeat * (2 if mode == "tu" else 1)
    print(f"{preset.name}: {len(selected)} {noun}(s), {n_compiles} compile(s), serial", file=sys.stderr)

    base = ct.baseline(selected[0][1], scratch=scratch, repeat=args.repeat,
                       time_trace=trace, family=family, env=env)
    measure = ct.measure_headers if mode == "headers" else ct.measure_tus
    records = measure(selected, scratch=scratch, repeat=args.repeat, time_trace=trace,
                      family=family, env=env, root=ctx.root)

    return {
        "preset": preset.name,
        "build_dir": ctx.rel(preset.build_dir),
        "baseline": base.as_dict(),
        "records": [r.as_dict() for r in records],
    }


def _summarize(report: dict, ctx: Context, out: Path, top: int) -> bool:
    """Print the top-K table and the totals, and report whether everything compiled."""
    all_ok = True
    for block in report["presets"]:
        records = block["records"]
        failed = [r for r in records if not r["ok"]]
        ok_records = [r for r in records if r["ok"]]
        all_ok = all_ok and not failed

        print(f"\n=== {block['preset']} ===")
        print(f"baseline (empty TU): {block['baseline']['wall_s']:.3f} s")

        if report["mode"] == "headers":
            _print_headers(ok_records, top)
        else:
            _print_tus(ok_records, top)

        total = sum(r["wall_s"] for r in ok_records)
        print(f"\n  {len(ok_records)} measured, {total:.1f} s total")
        if failed:
            print(console.red(f"  {len(failed)} failed to compile:"))
            for r in failed[:10]:
                first = (r.get("error") or "").strip().splitlines()
                print(console.red(f"    {r['path']} [{r['kind']}]: {first[0] if first else 'unknown error'}"))

    print(f"\nReport written to {ctx.rel(out)}", file=sys.stderr)
    return all_ok


def _print_headers(records: list[dict], top: int) -> None:
    rows = sorted(records, key=lambda r: -r["wall_s"])[:top]
    print(f"\n  {'wall':>8}  {'incl':>5}  {'ours':>7}  {'sys':>7}  header")
    for r in rows:
        t = r.get("trace", {})
        print(f"  {r['wall_s']:7.3f}s  {t.get('include_count', 0):5d}  "
              f"{t.get('ours_source_s', 0):6.2f}s  {t.get('system_source_s', 0):6.2f}s  {r['path']}")


def _print_tus(records: list[dict], top: int) -> None:
    """One row per TU, pairing its full and includes-only measurements.

    The ratio is the number this mode exists for: how much of a TU is its includes.
    Both halves ran back to back, so the ratio is not exposed to the run-to-run variance a build shows.
    """
    full = {r["path"]: r for r in records if r["kind"] == "tu-full"}
    inc = {r["path"]: r for r in records if r["kind"] == "tu-includes"}
    paired = [(p, full[p], inc[p]) for p in full if p in inc]
    paired.sort(key=lambda row: -row[1]["wall_s"])

    print(f"\n  {'full':>8}  {'includes':>9}  {'incl%':>6}  {'own':>8}  TU")
    for path, f, i in paired[:top]:
        pct = 100.0 * i["wall_s"] / f["wall_s"] if f["wall_s"] else 0.0
        print(f"  {f['wall_s']:7.3f}s  {i['wall_s']:8.3f}s  {pct:5.1f}%  "
              f"{f['wall_s'] - i['wall_s']:7.3f}s  {path}")

    if paired:
        tf = sum(f["wall_s"] for _, f, _ in paired)
        ti = sum(i["wall_s"] for _, _, i in paired)
        print(f"\n  across {len(paired)} TU(s): {tf:.1f} s full, {ti:.1f} s includes-only "
              f"({100.0 * ti / tf:.0f}% of the time is includes)")
