"""`compile-time` — measure what the compiler spends its time on.

    compile-time headers <glob>...   what including a header costs, in a TU that does nothing else
    compile-time tu <glob>...        what a TU costs, full and reduced to its includes
    compile-time pch <glob>...       what a target's precompiled header is worth, per TU

All report end-to-end wall clock of a real compile with the target's real flags, and `headers` / `tu` fold each run's `-ftime-trace` split into the JSON.
Serial by default, because a parallel measurement measures the machine rather than the code.

`headers` and `tu` strip the target's PCH flags, so they measure parsing from source whether or not the preset builds with a PCH.
`pch` is the mode that keeps them, and it needs the preset BUILT rather than merely configured.

docs/guides/compile-times.md is the workflow; tools/dev/lib/perf/compile_time.py owns the mechanism and the traps it avoids.
"""

from __future__ import annotations

import argparse
import csv
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

    p_ = csub.add_parser("pch", help="What a target's precompiled header is worth: each TU with it and without")
    _common(p_)

    e = csub.add_parser(
        "export",
        help="Flatten report JSON into tidy CSV, for pivoting in a spreadsheet or dataframe",
    )
    e.add_argument("inputs", nargs="+", metavar="FILE", help="Report JSON(s) written by headers/tu")
    e.add_argument("--csv-dir", metavar="DIR", default=str(_DEFAULT_OUT),
                   help=f"Where the CSVs land (default: {_DEFAULT_OUT.as_posix()})")
    e.add_argument("--prefix", default="", help="Filename prefix, to keep several exports side by side")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if args.compile_time_cmd == "export":
        _run_export(args, ctx)
        return

    presets = ctx.resolve_build_presets(args)
    mode = args.compile_time_cmd
    targets = [s.strip() for spec in (args.target or []) for s in spec.split(",") if s.strip()] or None

    scratch_ctx = None
    if args.keep_tus:
        # Absolute, because every compile runs with cwd set to the build directory the entry names:
        # a relative scratch path would be written here and looked for there.
        scratch = Path(args.keep_tus).resolve()
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


def _run_export(args: argparse.Namespace, ctx: Context) -> None:
    """Flatten report JSON into two long-format CSVs.

    Long format rather than one wide table, because the interesting question is an aggregation the JSON cannot express:
    a header's total cost across the build is `SUM(header_self_s) GROUP BY header`, which is one pivot over `attribution.csv` and nothing at all over the nested records.
    """
    reports = []
    for raw in args.inputs:
        path = Path(raw)
        if not path.is_file():
            ctx.die(f"{raw!r} is not a file")
        try:
            reports.append((path, json.loads(path.read_text(encoding="utf-8"))))
        except ValueError as exc:
            ctx.die(f"{raw!r} is not valid JSON: {exc}")

    out_dir = Path(args.csv_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rec_path = out_dir / f"{args.prefix}records.csv"
    att_path = out_dir / f"{args.prefix}attribution.csv"

    rec_rows, att_rows = [], []
    for path, report in reports:
        mode = report.get("mode", "")
        for block in report.get("presets", []):
            preset = block.get("preset", "")
            # A TU contributes two records; pair them so `incl_pct` lands on one row rather than needing a join.
            includes = {r["path"]: r for r in block.get("records", []) if r["kind"] == "tu-includes"}
            for rec in block.get("records", []):
                if rec["kind"] == "tu-includes":
                    continue
                trace = rec.get("trace", {})
                inc = includes.get(rec["path"], {}) if rec["kind"] == "tu-full" else {}
                inc_wall = inc.get("wall_s", "")
                rec_rows.append({
                    "source": path.name,
                    "mode": mode,
                    "preset": preset,
                    "kind": rec["kind"],
                    "path": rec["path"],
                    "name": Path(rec["path"]).name,
                    "target": rec.get("target", ""),
                    "ok": int(bool(rec.get("ok", True))),
                    "wall_s": rec.get("wall_s", ""),
                    "includes_wall_s": inc_wall,
                    "incl_pct": (round(100.0 * inc_wall / rec["wall_s"], 1)
                                 if inc_wall not in ("", None) and rec.get("wall_s") else ""),
                    "own_s": (round(rec["wall_s"] - inc_wall, 4)
                              if inc_wall not in ("", None) and rec.get("wall_s") else ""),
                    "frontend_s": trace.get("frontend_s", ""),
                    "backend_s": trace.get("backend_s", ""),
                    "source_s": trace.get("source_s", ""),
                    "ours_source_s": trace.get("ours_source_s", ""),
                    "system_source_s": trace.get("system_source_s", ""),
                    "instantiate_s": trace.get("instantiate_s", ""),
                    "include_count": trace.get("include_count", ""),
                })
                for header in trace.get("headers", []):
                    att_rows.append({
                        "preset": preset,
                        "kind": rec["kind"],
                        "tu_path": rec["path"],
                        "tu_target": rec.get("target", ""),
                        "header": header["file"],
                        "header_name": Path(header["file"]).name,
                        "header_self_s": header["self_s"],
                        "is_system": int(bool(header.get("system"))),
                    })

    _write_csv(rec_path, rec_rows)
    _write_csv(att_path, att_rows)

    print(f"{len(rec_rows)} record row(s)      -> {ctx.rel(rec_path)}")
    print(f"{len(att_rows)} attribution row(s) -> {ctx.rel(att_path)}")
    _print_attribution_preview(att_rows)


def _write_csv(path: Path, rows: list[dict]) -> None:
    # newline="" is required, or csv doubles the line endings on Windows.
    with open(path, "w", encoding="utf-8", newline="") as f:
        if not rows:
            return
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def _print_attribution_preview(rows: list[dict]) -> None:
    """The aggregation the second table exists for, so the first useful answer needs no spreadsheet."""
    if not rows:
        return
    total: dict[str, float] = {}
    count: dict[str, int] = {}
    system: dict[str, bool] = {}
    for r in rows:
        key = r["header"]
        total[key] = total.get(key, 0.0) + r["header_self_s"]
        count[key] = count.get(key, 0) + 1
        system[key] = bool(r["is_system"])

    ours_s = sum(v for k, v in total.items() if not system[k])
    sys_s = sum(v for k, v in total.items() if system[k])
    print(f"\nattributable parse time: {ours_s:.0f} s ours, {sys_s:.0f} s system "
          f"({100.0 * sys_s / (ours_s + sys_s):.0f}% system)")
    print("\n  top headers by TOTAL cost across every measured TU:")
    print(f"  {'total':>8}  {'TUs':>5}  {'each':>7}  header")
    for key, secs in sorted(total.items(), key=lambda kv: -kv[1])[:15]:
        tag = "sys " if system[key] else "ours"
        print(f"  {secs:7.1f}s  {count[key]:5d}  {secs / count[key]:6.3f}s  {tag}  {Path(key).name}")


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
    if mode == "pch":
        # The PCH half compiles against a .pch the target's own build produces, so a configured-but-unbuilt preset fails every TU.
        # Build here rather than reporting 100 % failure with a compiler error each.
        dev.build([preset], None, root=ctx.root, mirror=args.mirror_output, verbose=args.verbose)
    if not selected:
        ctx.die(f"no {noun}(s) matched {' '.join(args.patterns)!r} in preset {preset.name!r}")

    env = dev.env_for_preset(preset)
    family = preset.family
    trace = not args.no_time_trace
    n_compiles = len(selected) * args.repeat * (1 if mode == "headers" else 2)
    print(f"{preset.name}: {len(selected)} {noun}(s), {n_compiles} compile(s), serial", file=sys.stderr)

    base = ct.baseline(selected[0][1], scratch=scratch, repeat=args.repeat,
                       time_trace=trace, family=family, env=env)
    if mode == "pch":
        records = ct.measure_pch(selected, scratch=scratch, repeat=args.repeat,
                                 family=family, env=env, root=ctx.root)
    else:
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
        elif report["mode"] == "pch":
            _print_pch(ok_records, top)
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


def _print_pch(records: list[dict], top: int) -> None:
    """One row per TU, pairing its with-PCH and without-PCH measurements, plus the per-target totals.

    The per-target ratio at the bottom is the number that decides a tier, since a tier is a property of a target rather than of one file.
    A ratio of 1.00 across the board means the preset has precompiled headers disabled.
    """
    with_pch = {r["path"]: r for r in records if r["kind"] == "tu-pch"}
    without = {r["path"]: r for r in records if r["kind"] == "tu-nopch"}
    paired = [(p, with_pch[p], without[p]) for p in with_pch if p in without]
    paired.sort(key=lambda row: -row[2]["wall_s"])

    print(f"\n  {'no pch':>8}  {'with pch':>9}  {'ratio':>6}  {'saved':>8}  TU")
    for path, w, n in paired[:top]:
        ratio = w["wall_s"] / n["wall_s"] if n["wall_s"] else 0.0
        print(f"  {n['wall_s']:7.3f}s  {w['wall_s']:8.3f}s  {ratio:5.2f}  "
              f"{n['wall_s'] - w['wall_s']:7.3f}s  {path}")

    by_target: dict[str, list[tuple[float, float]]] = {}
    for _, w, n in paired:
        by_target.setdefault(w.get("target", ""), []).append((n["wall_s"], w["wall_s"]))
    if by_target:
        print(f"\n  {'no pch':>8}  {'with pch':>9}  {'ratio':>6}  {'TUs':>4}  target")
        for target, rows in sorted(by_target.items(), key=lambda kv: -sum(r[0] for r in kv[1])):
            tn = sum(r[0] for r in rows)
            tw = sum(r[1] for r in rows)
            print(f"  {tn:7.2f}s  {tw:8.2f}s  {tw / tn if tn else 0.0:5.2f}  {len(rows):4d}  {target}")


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
