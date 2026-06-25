"""Coverage: LLVM source-based test coverage collection and post-processing.

The pipeline mirrors the LLVM workflow end to end:

  1. run instrumented test binaries with a distinct LLVM_PROFILE_FILE each, so
     every run drops a raw `*.profraw` (reusing the standard test runner);
  2. `llvm-profdata merge` the raw files into one indexed `coverage.profdata`;
  3. `llvm-cov export` that into a JSON sidecar (`coverage.llvm-cov.json`) and,
     optionally, a browsable `llvm-cov show` HTML report.

The export JSON is written at the build-dir root, suffixed `.llvm-cov.json`, on
purpose: it is the machine-readable artifact a future `coverage_diag` MCP tool
discovers, exactly like build.json/test.json feed build_diag/test_diag. A second
`coverage.json` sidecar records the dev.py step metadata plus distilled totals.

Instrumentation itself is a build concern (the SC_COVERAGE CMake option, set by
the *-coverage presets); this module only collects and post-processes.

Public API:
    coverage_run(...)    -> list[dict]   build-already-done; run + merge + report
    coverage_report(...) -> list[dict]   re-post-process an existing profdata
    coverage_merge(...)  -> dict         combine profdata across presets/runs
    find_tool(name, env_var)             locate an llvm-* tool (used by doctor)
"""

from __future__ import annotations

import json
import os
import shutil
from datetime import datetime
from pathlib import Path

from . import targets as targets_mod
from .logs import step_fields, write_sidecar
from .models import Preset, StepResult
from .process import run_step
from .test import test as run_tests

# llvm-cov regex matching files to drop from the report. Separator-agnostic so it
# works with Windows backslash paths too: we measure library sources, not the
# tests that exercise them nor vendored third-party code under extern/.
DEFAULT_IGNORE_REGEX = r"([/\\]extern[/\\]|[/\\]tests[/\\])"

# Coverage metrics we surface in the distilled summary, in report order.
_METRICS = ("lines", "functions", "regions", "branches")


# ---------------------------------------------------------------------------
# Tool resolution
# ---------------------------------------------------------------------------

def find_tool(name: str, env_var: str) -> str | None:
    """Locate an llvm-* tool by env override then PATH (no build dir needed).

    `env_var` (e.g. LLVM_COV) wins if set, so a user can pin a specific install;
    otherwise PATH is searched. Returns the resolved path/command or None.
    """
    override = os.environ.get(env_var)
    if override:
        return override
    return shutil.which(name)


def _compiler_from_cache(build_dir: Path) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_CXX_COMPILER:"):
                return line.partition("=")[2].strip()
    except OSError:
        return None
    return None


def resolve_tool(name: str, env_var: str, build_dir: Path) -> str | None:
    """Like find_tool, but also looks beside the configured compiler.

    On Windows clang-cl and llvm-cov/llvm-profdata ship in the same LLVM bin/
    that often isn't on PATH; falling back to the compiler's directory keeps the
    versions matched (llvm-cov must match the clang that built the binaries).
    """
    found = find_tool(name, env_var)
    if found:
        return found
    cxx = _compiler_from_cache(build_dir)
    if cxx:
        exe = name + (".exe" if os.name == "nt" else "")
        cand = Path(cxx).parent / exe
        if cand.is_file():
            return str(cand)
    return None


class CoverageToolError(Exception):
    """Raised when llvm-profdata / llvm-cov cannot be located."""


def _require_tools(build_dir: Path) -> tuple[str, str]:
    profdata = resolve_tool("llvm-profdata", "LLVM_PROFDATA", build_dir)
    cov = resolve_tool("llvm-cov", "LLVM_COV", build_dir)
    missing = [n for n, v in (("llvm-profdata", profdata), ("llvm-cov", cov)) if v is None]
    if missing:
        raise CoverageToolError(
            f"{', '.join(missing)} not found (install LLVM, or set "
            f"LLVM_PROFDATA / LLVM_COV). Run: uv run dev.py doctor"
        )
    return profdata, cov  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

class _Paths:
    """Coverage artifact locations under a build directory."""

    def __init__(self, build_dir: Path):
        self.build_dir = build_dir
        self.cov = build_dir / "coverage"
        self.profraw = self.cov / "profraw"
        self.profdata = self.cov / "coverage.profdata"
        self.html = self.cov / "html"
        self.json = build_dir / "coverage.llvm-cov.json"  # diag sidecar


def _binary_artifacts(preset: Preset, binary_names: list[str]) -> list[Path]:
    """Resolve test target names to their built executable artifacts."""
    by_name = {
        t.name: t
        for t in targets_mod.discover_targets(preset.build_dir, preset.build_type)
    }
    out: list[Path] = []
    for name in binary_names:
        t = by_name.get(name)
        if t is not None and t.artifact is not None:
            out.append(t.artifact)
    return out


def _objects_args(binaries: list[Path]) -> list[str]:
    """llvm-cov object arguments: first binary positional, rest via -object."""
    if not binaries:
        return []
    args = [str(binaries[0])]
    for b in binaries[1:]:
        args += ["-object", str(b)]
    return args


# ---------------------------------------------------------------------------
# Pipeline steps (each captured via run_step, like every other dev.py step)
# ---------------------------------------------------------------------------

def _merge_profraw(
    profdata_tool: str, paths: _Paths, *, root: Path, mirror: bool, verbose: bool
) -> StepResult:
    raws = sorted(paths.profraw.glob("*.profraw"))
    cmd = [profdata_tool, "merge", "-sparse", *[str(p) for p in raws], "-o", str(paths.profdata)]
    return run_step(
        cmd, step_type="coverage", name="merge",
        build_dir=paths.build_dir, cwd=root, mirror=mirror, verbose=verbose,
    )


def _export_json(
    cov_tool: str, profdata: Path, binaries: list[Path], out_json: Path,
    *, build_dir: Path, root: Path, ignore_regex: str, mirror: bool, verbose: bool,
) -> StepResult:
    cmd = [
        cov_tool, "export", f"-instr-profile={profdata}",
        *_objects_args(binaries), f"-ignore-filename-regex={ignore_regex}",
    ]
    result = run_step(
        cmd, step_type="coverage", name="export",
        build_dir=build_dir, cwd=root, mirror=mirror, verbose=verbose,
    )
    # llvm-cov prints only JSON to stdout (diagnostics go to stderr), so the
    # captured stdout log *is* the report; publish it as the diag sidecar.
    if result.ok:
        try:
            shutil.copyfile(result.stdout_log, out_json)
        except OSError:
            pass
    return result


def _generate_html(
    cov_tool: str, profdata: Path, binaries: list[Path], html_dir: Path,
    *, build_dir: Path, root: Path, ignore_regex: str, mirror: bool, verbose: bool,
) -> StepResult:
    cmd = [
        cov_tool, "show", "-format=html", f"-output-dir={html_dir}",
        f"-instr-profile={profdata}", *_objects_args(binaries),
        f"-ignore-filename-regex={ignore_regex}",
    ]
    return run_step(
        cmd, step_type="coverage", name="html",
        build_dir=build_dir, cwd=root, mirror=mirror, verbose=verbose,
    )


# ---------------------------------------------------------------------------
# Post-processing (distill the verbose export into compact totals)
# ---------------------------------------------------------------------------

def _library_key(filename: str) -> str | None:
    """Map a source path to its library key 'libs/<category>/<lib>', or None."""
    parts = Path(filename).parts
    try:
        i = len(parts) - 1 - list(reversed(parts)).index("libs")
    except ValueError:
        return None
    if i + 2 < len(parts):
        return "/".join(("libs", parts[i + 1], parts[i + 2]))
    return None


def _pct(covered: int, count: int) -> float:
    return round(100.0 * covered / count, 2) if count else 0.0


def summarize(json_path: Path) -> dict:
    """Distill an llvm-cov export JSON into overall + per-library totals.

    Returns {"totals": {metric: {covered, count, percent}}, "libraries": {key:
    {metric: {...}}}}. Empty dict when the export is missing/unparseable.
    """
    try:
        data = json.loads(json_path.read_text(encoding="utf-8"))
        export = data["data"][0]
    except (OSError, ValueError, KeyError, IndexError):
        return {}

    totals = {
        m: {
            "covered": export["totals"][m]["covered"],
            "count": export["totals"][m]["count"],
            "percent": round(export["totals"][m]["percent"], 2),
        }
        for m in _METRICS
        if m in export.get("totals", {})
    }

    libraries: dict[str, dict] = {}
    for f in export.get("files", []):
        key = _library_key(f.get("filename", ""))
        if key is None:
            continue
        agg = libraries.setdefault(key, {m: {"covered": 0, "count": 0} for m in _METRICS})
        for m in _METRICS:
            s = f.get("summary", {}).get(m)
            if s:
                agg[m]["covered"] += s["covered"]
                agg[m]["count"] += s["count"]
    for agg in libraries.values():
        for m in _METRICS:
            agg[m]["percent"] = _pct(agg[m]["covered"], agg[m]["count"])

    return {"totals": totals, "libraries": dict(sorted(libraries.items()))}


def _rel(p: Path, base: Path) -> str:
    try:
        return str(p.relative_to(base))
    except ValueError:
        return str(p)


def _write_meta(
    build_dir: Path, *, preset: str, paths_json: Path, profdata: Path,
    html_dir: Path | None, ignore_regex: str, steps: list[StepResult], summary: dict,
) -> None:
    write_sidecar(
        build_dir, "coverage.json",
        {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "preset": preset,
            "llvm_cov_json": _rel(paths_json, build_dir),
            "profdata": _rel(profdata, build_dir),
            "html_dir": _rel(html_dir, build_dir) if html_dir else None,
            "ignore_regex": ignore_regex,
            "steps": [step_fields(s, build_dir) for s in steps],
            "totals": summary.get("totals", {}),
            "libraries": summary.get("libraries", {}),
        },
    )


# ---------------------------------------------------------------------------
# Public orchestration
# ---------------------------------------------------------------------------

def _report_from_profdata(
    preset: Preset, binaries: list[Path], cov_tool: str, paths: _Paths,
    *, root: Path, html: bool, ignore_regex: str, mirror: bool, verbose: bool,
    prior_steps: list[StepResult],
) -> dict:
    """Export JSON (+ optional HTML) from an existing profdata and summarize."""
    steps = list(prior_steps)
    export = _export_json(
        cov_tool, paths.profdata, binaries, paths.json,
        build_dir=preset.build_dir, root=root, ignore_regex=ignore_regex,
        mirror=mirror, verbose=verbose,
    )
    steps.append(export)

    html_dir: Path | None = None
    if html and export.ok:
        html_step = _generate_html(
            cov_tool, paths.profdata, binaries, paths.html,
            build_dir=preset.build_dir, root=root, ignore_regex=ignore_regex,
            mirror=mirror, verbose=verbose,
        )
        steps.append(html_step)
        if html_step.ok:
            html_dir = paths.html

    summary = summarize(paths.json) if export.ok else {}
    _write_meta(
        preset.build_dir, preset=preset.name, paths_json=paths.json,
        profdata=paths.profdata, html_dir=html_dir, ignore_regex=ignore_regex,
        steps=steps, summary=summary,
    )
    return {
        "preset": preset.name,
        "ok": all(s.ok for s in steps),
        "llvm_cov_json": paths.json,
        "profdata": paths.profdata,
        "html_dir": html_dir,
        "totals": summary.get("totals", {}),
        "libraries": summary.get("libraries", {}),
        "steps": steps,
    }


def coverage_run(
    presets: list[Preset], binary_names: list[str], *, root: Path,
    test_name: str | None = None, html: bool = False,
    ignore_regex: str = DEFAULT_IGNORE_REGEX, timeout: float | None = None,
    mirror: bool = False, verbose: bool = False,
) -> list[dict]:
    """Run the instrumented tests, merge counters, and post-process per preset.

    Assumes the coverage preset is already built. Returns one result dict per
    preset (preset, ok, llvm_cov_json, profdata, html_dir, totals, libraries,
    steps). Raises CoverageToolError if the llvm tools are missing.
    """
    results: list[dict] = []
    for preset in presets:
        profdata_tool, cov_tool = _require_tools(preset.build_dir)
        paths = _Paths(preset.build_dir)
        # Start clean so a prior run's profraw can't leak into this merge.
        if paths.profraw.exists():
            shutil.rmtree(paths.profraw, ignore_errors=True)
        paths.profraw.mkdir(parents=True, exist_ok=True)

        run_tests(
            [preset], binary_names, root=root, test_name=test_name,
            extra_env_for=lambda name, d=paths.profraw: {
                "LLVM_PROFILE_FILE": str(d / f"{name}-%p.profraw")
            },
            timeout=timeout, write_xml=True, mirror=mirror, verbose=verbose,
        )

        merge = _merge_profraw(profdata_tool, paths, root=root, mirror=mirror, verbose=verbose)
        binaries = _binary_artifacts(preset, binary_names)
        if not merge.ok:
            _write_meta(
                preset.build_dir, preset=preset.name, paths_json=paths.json,
                profdata=paths.profdata, html_dir=None, ignore_regex=ignore_regex,
                steps=[merge], summary={},
            )
            results.append({
                "preset": preset.name, "ok": False, "llvm_cov_json": paths.json,
                "profdata": paths.profdata, "html_dir": None, "totals": {},
                "libraries": {}, "steps": [merge],
            })
            continue

        results.append(_report_from_profdata(
            preset, binaries, cov_tool, paths, root=root, html=html,
            ignore_regex=ignore_regex, mirror=mirror, verbose=verbose,
            prior_steps=[merge],
        ))
    return results


def coverage_report(
    presets: list[Preset], binary_names: list[str], *, root: Path,
    html: bool = False, ignore_regex: str = DEFAULT_IGNORE_REGEX,
    mirror: bool = False, verbose: bool = False,
) -> list[dict]:
    """Re-post-process an existing merged profdata, without re-running tests.

    Raises FileNotFoundError if a preset has no coverage.profdata yet.
    """
    results: list[dict] = []
    for preset in presets:
        cov_tool = resolve_tool("llvm-cov", "LLVM_COV", preset.build_dir)
        if cov_tool is None:
            raise CoverageToolError("llvm-cov not found (set LLVM_COV). Run: uv run dev.py doctor")
        paths = _Paths(preset.build_dir)
        if not paths.profdata.is_file():
            raise FileNotFoundError(
                f"no merged coverage for {preset.name!r} at {paths.profdata} - "
                f"run: uv run dev.py coverage run --preset {preset.name}"
            )
        binaries = _binary_artifacts(preset, binary_names)
        results.append(_report_from_profdata(
            preset, binaries, cov_tool, paths, root=root, html=html,
            ignore_regex=ignore_regex, mirror=mirror, verbose=verbose, prior_steps=[],
        ))
    return results


def coverage_merge(
    presets: list[Preset], binary_names_by_preset: dict[str, list[str]], *,
    root: Path, output_dir: Path, html: bool = False,
    ignore_regex: str = DEFAULT_IGNORE_REGEX, mirror: bool = False, verbose: bool = False,
) -> dict:
    """Merge several presets' profdata into one combined report under output_dir.

    Each input preset must already have a coverage.profdata (from coverage run).
    The combined export covers the union of all presets' test binaries. Returns a
    single result dict; writes coverage.profdata, coverage.llvm-cov.json, and
    coverage.json under output_dir.
    """
    profdata_tool = resolve_tool("llvm-profdata", "LLVM_PROFDATA", presets[0].build_dir)
    cov_tool = resolve_tool("llvm-cov", "LLVM_COV", presets[0].build_dir)
    if profdata_tool is None or cov_tool is None:
        raise CoverageToolError(
            "llvm-profdata / llvm-cov not found (set LLVM_PROFDATA / LLVM_COV). Run: uv run dev.py doctor"
        )

    inputs: list[Path] = []
    binaries: list[Path] = []
    for preset in presets:
        pd = _Paths(preset.build_dir).profdata
        if not pd.is_file():
            raise FileNotFoundError(
                f"no merged coverage for {preset.name!r} at {pd} - "
                f"run: uv run dev.py coverage run --preset {preset.name}"
            )
        inputs.append(pd)
        binaries.extend(_binary_artifacts(preset, binary_names_by_preset.get(preset.name, [])))

    output_dir.mkdir(parents=True, exist_ok=True)
    out_profdata = output_dir / "coverage.profdata"
    out_json = output_dir / "coverage.llvm-cov.json"

    merge = run_step(
        [profdata_tool, "merge", "-sparse", *[str(p) for p in inputs], "-o", str(out_profdata)],
        step_type="coverage", name="merge", build_dir=output_dir, cwd=root,
        mirror=mirror, verbose=verbose,
    )
    steps = [merge]
    html_dir: Path | None = None
    summary: dict = {}
    if merge.ok:
        export = _export_json(
            cov_tool, out_profdata, binaries, out_json,
            build_dir=output_dir, root=root, ignore_regex=ignore_regex,
            mirror=mirror, verbose=verbose,
        )
        steps.append(export)
        if html and export.ok:
            html_step = _generate_html(
                cov_tool, out_profdata, binaries, output_dir / "html",
                build_dir=output_dir, root=root, ignore_regex=ignore_regex,
                mirror=mirror, verbose=verbose,
            )
            steps.append(html_step)
            if html_step.ok:
                html_dir = output_dir / "html"
        summary = summarize(out_json) if export.ok else {}

    _write_meta(
        output_dir, preset="+".join(p.name for p in presets), paths_json=out_json,
        profdata=out_profdata, html_dir=html_dir, ignore_regex=ignore_regex,
        steps=steps, summary=summary,
    )
    return {
        "preset": "+".join(p.name for p in presets),
        "ok": all(s.ok for s in steps),
        "llvm_cov_json": out_json,
        "profdata": out_profdata,
        "html_dir": html_dir,
        "totals": summary.get("totals", {}),
        "libraries": summary.get("libraries", {}),
        "steps": steps,
    }
