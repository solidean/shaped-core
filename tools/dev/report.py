"""Opinionated CLI presentation for a dev.py-style driver.

This is project presentation policy, not generic tooling: it knows the exact
wording, the diag-tool hints, and the success/warning/failure coloring dev.py
wants. The rest of tools/dev returns plain data (StepResult, run records, totals);
the functions here turn that data into the lines a developer reads, colored via
`console`. They print and return a bool (or nothing) — the caller owns sys.exit.
"""

from __future__ import annotations

import sys
from pathlib import Path

from . import console
from .models import Preset, StepResult


def rel(p: Path, root: Path) -> str:
    """Path relative to the repo root (posix style) for compact hints."""
    try:
        return p.relative_to(root).as_posix()
    except ValueError:
        return str(p)


def fmt_dur(seconds: float) -> str:
    return f"{seconds * 1000:.0f} ms" if seconds < 1 else f"{seconds:.1f} s"


# ---------------------------------------------------------------------------
# Diagnosis hints
# ---------------------------------------------------------------------------

def build_diag_hint(presets: list[Preset], root: Path) -> str:
    if len(presets) == 1:
        return f'diagnose with: build_diag base_path="{rel(presets[0].build_dir, root)}"'
    return "diagnose with: build_diag"


def test_diag_hint(presets: list[Preset], root: Path) -> str:
    if len(presets) == 1:
        sel = f"{rel(presets[0].build_dir, root)}/**/*.results.xml"
    else:
        sel = "build/**/*.results.xml"
    return f'diagnose with: test_diag path="{sel}" errors_only=true'


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def print_build_failure(results: list[StepResult], presets: list[Preset], root: Path) -> None:
    """Report a failed build phase with the right diagnosis hint.

    A configure failure leaves no per-translation-unit sidecars, so the
    build_diag hint would point at an empty scan. Point at the captured configure
    log instead; only genuine compile/link failures get the build_diag hint.
    """
    cfg_fail = next((r for r in results if not r.ok and r.step_type == "configure"), None)
    if cfg_fail is not None:
        print(console.red(f"\nconfigure failed - see {rel(cfg_fail.stderr_log, root)}"), file=sys.stderr)
    else:
        print(console.red(f"\nbuild failed - {build_diag_hint(presets, root)}"), file=sys.stderr)


def summarize_build(
    build_steps: list[StepResult], built_files: int, presets: list[Preset]
) -> None:
    """Print the success summary for a build phase (callers handle failures)."""
    total_s = sum(r.duration_s for r in build_steps)
    print(
        console.green(
            f"\nBuilt {built_files} file(s) across {len(build_steps)} target(s), "
            f"{len(presets)} preset(s) in {fmt_dur(total_s)}."
        ),
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def summarize_tests(records: list[dict], presets: list[Preset], root: Path) -> bool:
    """Print the pass/fail summary for a set of test runs; return True if all passed."""
    total_s = sum(r["duration_s"] for r in records)
    failed = sum(1 for r in records if r["returncode"] != 0)
    tests = sum(r["junit"]["tests"] for r in records if r["junit"])
    checks = sum(r["junit"]["assertions"] for r in records if r["junit"])
    stats = f"{tests} tests, {checks} checks"
    if failed:
        print(
            console.red(f"\n{failed} of {len(records)} test run(s) failed ({stats}) in {fmt_dur(total_s)}"),
            file=sys.stderr,
        )
        print(console.red(f"tests failed - {test_diag_hint(presets, root)}"), file=sys.stderr)
        return False
    print(
        console.green(f"\nAll {len(records)} test run(s) passed: {stats} in {fmt_dur(total_s)}."),
        file=sys.stderr,
    )
    return True


# ---------------------------------------------------------------------------
# Coverage
# ---------------------------------------------------------------------------

def summarize_coverage(results: list[dict], root: Path) -> bool:
    """Print the per-preset coverage tables; return True if every step succeeded."""
    ok = True
    for r in results:
        if not r["ok"]:
            ok = False
            failed = next((s for s in r["steps"] if not s.ok), None)
            where = f" - see {rel(failed.stderr_log, root)}" if failed else ""
            print(console.red(f"\ncoverage [{r['preset']}] FAILED{where}"), file=sys.stderr)
            continue

        t = r["totals"]
        def pct(metric: str) -> str:
            return f"{t.get(metric, {}).get('percent', 0.0):.1f}%"
        lines = t.get("lines", {})
        print(
            console.green(
                f"\nCoverage [{r['preset']}]: lines {pct('lines')} "
                f"({lines.get('covered', 0)}/{lines.get('count', 0)}), "
                f"functions {pct('functions')}, regions {pct('regions')}"
            ),
            file=sys.stderr,
        )
        for lib, m in r["libraries"].items():
            lm = m.get("lines", {})
            print(
                f"  {lib:<30} {lm.get('percent', 0.0):6.1f}%  "
                f"({lm.get('covered', 0)}/{lm.get('count', 0)} lines)",
                file=sys.stderr,
            )
        print(console.dim(f"  JSON: {rel(r['llvm_cov_json'], root)}"), file=sys.stderr)
        if r["html_dir"]:
            print(console.dim(f"  HTML: {rel(Path(r['html_dir']) / 'index.html', root)}"), file=sys.stderr)
    return ok


# ---------------------------------------------------------------------------
# PGO / perf metrics
# ---------------------------------------------------------------------------

def summarize_perf(metrics: list[dict]) -> None:
    """Print a baseline -> PGO delta table (per metric, oriented % change).

    ASCII-only: dev.py output is captured/redisplayed through Windows consoles
    (cp1252), where arrow/dot glyphs turn to mojibake. The signed % and the
    green/red coloring carry the direction.
    """
    if not metrics:
        print(console.yellow("  no comparable metrics (did the guide benchmarks record any?)"), file=sys.stderr)
        return

    name_w = max((len(f"{m['test']} | {m['name']}") for m in metrics), default=10)
    for m in metrics:
        label = f"{m['test']} | {m['name']}"
        delta = m["delta_pct"]
        color = console.green if delta > 0 else (console.red if delta < 0 else console.dim)
        line = (
            f"  {label:<{name_w}}  {m['baseline']:>10.2f} -> {m['pgo']:>10.2f} {m['unit']:<8} "
            f"{delta:+.1f}%"
        )
        print(color(line), file=sys.stderr)


def summarize_pgo(result: dict, root: Path) -> bool:
    """Print the PGO outcome (and, when present, the measure delta table). True if ok."""
    if not result.get("ok"):
        stage = result.get("stage", "?")
        print(console.red(f"\nPGO FAILED at stage: {stage}"), file=sys.stderr)
        return False

    train = result.get("train")
    if train:
        print(
            console.green(f"\nPGO profile built from {train['profraw_count']} profraw file(s): "
                          f"{rel(Path(train['profile']), root)}"),
            file=sys.stderr,
        )

    measure = result.get("measure")
    if measure is not None:
        print(
            console.bold(f"\nPGO speedup [{measure['baseline_preset']} -> {measure['pgo_preset']}]:"),
            file=sys.stderr,
        )
        summarize_perf(measure["metrics"])
    return True


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def summarize_format(result, root: Path) -> bool:
    """Print the summary for a `format.run_format` result; return True on success.

    Offenders (check mode) go to stdout (pipeable); the verdict to stderr.
    """
    if result.nothing:
        scope = "dirty libs/ sources" if result.dirty_only else "libs/ sources"
        print(console.dim(f"No {scope} to format."), file=sys.stderr)
        return True

    if result.check:
        if result.ok:
            print(console.green(f"\n{result.files} file(s) already formatted."), file=sys.stderr)
            return True
        for f in result.offenders:
            print(rel(f, root))
        sys.stdout.flush()
        hint = " --dirty-only" if result.dirty_only else ""
        print(
            console.red(
                f"\n{len(result.offenders)} of {result.files} file(s) need formatting "
                f"- run: uv run dev.py format{hint}"
            ),
            file=sys.stderr,
        )
        return False

    if not result.ok:
        where = rel(result.stderr_log, root) if result.stderr_log else "the format log"
        print(console.red(f"\nformat failed - see {where}"), file=sys.stderr)
        return False
    print(
        console.green(f"\nFormatted {result.files} file(s) in {fmt_dur(result.duration_s)}."),
        file=sys.stderr,
    )
    return True


# ---------------------------------------------------------------------------
# Cross-references
# ---------------------------------------------------------------------------

def summarize_crossrefs(result, root: Path) -> bool:
    """Print the cross-reference check result; return True if all valid.

    Offenders go to stdout (data, pipeable); the verdict to stderr.
    """
    files = result.md_files + result.src_files
    if not result.ok:
        for offender in result.offenders:
            print(offender)
        sys.stdout.flush()
        print(
            console.red(f"\n{len(result.offenders)} stale or broken cross-reference(s) across {files} file(s)"),
            file=sys.stderr,
        )
        return False
    total = result.md_links + result.src_refs
    print(
        console.green(
            f"\nOK: {total} cross-references valid across {files} files "
            f"({result.md_links} md links, {result.src_refs} source refs)"
        ),
        file=sys.stderr,
    )
    return True
