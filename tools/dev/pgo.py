"""PGO: IR-based profile-guided optimization pipeline.

Mirrors the coverage pipeline's shape (captured steps, JSON sidecars) but for
`-fprofile-generate` / `-fprofile-use`:

  instrument: build the *-pgo-generate preset (SC_PGO_GENERATE → -fprofile-generate)
  train:      run the guide benchmarks on it with LLVM_PROFILE_FILE → *.profraw,
              then `llvm-profdata merge` → build/pgo/pgo.profdata
  optimize:   build the *-pgo-use preset (SC_PGO_USE consumes that profile)
  measure:    run the guide benchmarks on baseline (release) and pgo-use, diff the
              recorded metrics into a speedup table

The merged profile lands at a stable source-relative path (build/pgo/pgo.profdata)
so the *-pgo-use configure preset consumes it without dev.py injecting -D flags.

Public API:
    profile_path(root)                              -> Path
    pgo_instrument(gen_preset, ...)                 -> list[StepResult]
    pgo_train(gen_preset, binaries, ...)            -> dict
    pgo_optimize(use_preset, ...)                   -> list[StepResult]
    pgo_measure(baseline, use_preset, binaries, ...)-> dict
    pgo_run(...)                                    -> dict
"""

from __future__ import annotations

import shutil
from datetime import datetime
from pathlib import Path

from . import perf
from .build import build as run_build
from .llvm_tools import resolve_tool
from .logs import step_fields, write_sidecar
from .models import Preset, StepResult
from .process import run_step


class PgoError(Exception):
    """Raised when a PGO prerequisite (tool or profile) is missing."""


def profile_path(root: Path) -> Path:
    """The stable, source-relative merged-profile location the *-pgo-use preset reads."""
    return root / "build" / "pgo" / "pgo.profdata"


def _profraw_dir(gen_preset: Preset) -> Path:
    return gen_preset.build_dir / "pgo" / "profraw"


def _profile_env_for(profraw: Path):
    return lambda name, d=profraw: {"LLVM_PROFILE_FILE": str(d / f"{name}-%p.profraw")}


# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------

def pgo_instrument(
    gen_preset: Preset, *, root: Path, mirror: bool = False, verbose: bool = False
) -> list[StepResult]:
    """Configure + build the instrumented (-fprofile-generate) preset."""
    return run_build([gen_preset], None, root=root, mirror=mirror, verbose=verbose)


def pgo_train(
    gen_preset: Preset, binary_names: list[str], *, root: Path,
    timeout: float | None = None, mirror: bool = False, verbose: bool = False,
) -> dict:
    """Run the guide benchmarks on the instrumented build and merge the profile.

    Cleans the profraw directory first so a stale run can't pollute the merge,
    drives every test binary's guide-benchmark bucket with a distinct
    LLVM_PROFILE_FILE, then `llvm-profdata merge -sparse` into build/pgo/pgo.profdata.
    Raises PgoError if llvm-profdata is missing or no profraw was produced.
    """
    profdata_tool = resolve_tool("llvm-profdata", "LLVM_PROFDATA", gen_preset.build_dir)
    if profdata_tool is None:
        raise PgoError("llvm-profdata not found (install LLVM or set LLVM_PROFDATA). Run: uv run dev.py doctor")

    profraw = _profraw_dir(gen_preset)
    if profraw.exists():
        shutil.rmtree(profraw, ignore_errors=True)
    profraw.mkdir(parents=True, exist_ok=True)

    perf.run_and_collect(
        gen_preset, binary_names, root=root, perf_dir=gen_preset.build_dir / "pgo" / "train-perf",
        extra_env_for=_profile_env_for(profraw),
        timeout=timeout, mirror=mirror, verbose=verbose,
    )

    raws = sorted(profraw.glob("*.profraw"))
    profile = profile_path(root)
    profile.parent.mkdir(parents=True, exist_ok=True)
    if not raws:
        raise PgoError(
            f"no *.profraw produced under {profraw} — the instrumented binaries ran no guide benchmarks "
            f"(add a GUIDE_BENCHMARK), or instrumentation is off (is this a *-pgo-generate preset?)"
        )

    merge = run_step(
        [profdata_tool, "merge", "-sparse", *[str(p) for p in raws], "-o", str(profile)],
        step_type="pgo", name="merge", build_dir=gen_preset.build_dir, cwd=root,
        mirror=mirror, verbose=verbose,
    )
    write_sidecar(
        gen_preset.build_dir, "pgo.json",
        {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "preset": gen_preset.name,
            "profile": str(profile),
            "profraw_count": len(raws),
            "steps": [step_fields(merge, gen_preset.build_dir)],
        },
    )
    return {"ok": merge.ok, "profile": profile, "profraw_count": len(raws), "steps": [merge]}


def pgo_optimize(
    use_preset: Preset, *, root: Path, mirror: bool = False, verbose: bool = False
) -> list[StepResult]:
    """Configure + build the optimized (-fprofile-use) preset.

    The SC_PGO_USE CMake side reads build/pgo/pgo.profdata; we pre-check it exists
    so the failure is a clear dev.py message rather than a configure-time fatal.
    """
    profile = profile_path(root)
    if not profile.is_file():
        raise PgoError(f"PGO profile not found at {profile} — run: uv run dev.py pgo train")
    return run_build([use_preset], None, root=root, mirror=mirror, verbose=verbose)


def pgo_measure(
    baseline_preset: Preset, use_preset: Preset, binary_names: list[str], *, root: Path,
    build_first: bool = True, timeout: float | None = None,
    mirror: bool = False, verbose: bool = False,
) -> dict:
    """Run the guide benchmarks on baseline + pgo-use and diff the recorded metrics.

    When `build_first`, both presets are (incrementally) built so a standalone
    `pgo measure` works; the pgo-use build needs the profile to exist. Returns
    {ok, baseline_preset, pgo_preset, metrics, baseline_count, pgo_count}.
    """
    if build_first:
        run_build([baseline_preset], None, root=root, mirror=mirror, verbose=verbose)
        pgo_optimize(use_preset, root=root, mirror=mirror, verbose=verbose)

    base_metrics = perf.run_and_collect(
        baseline_preset, binary_names, root=root,
        perf_dir=baseline_preset.build_dir / "pgo" / "measure-perf",
        timeout=timeout, mirror=mirror, verbose=verbose,
    )
    pgo_metrics = perf.run_and_collect(
        use_preset, binary_names, root=root,
        perf_dir=use_preset.build_dir / "pgo" / "measure-perf",
        timeout=timeout, mirror=mirror, verbose=verbose,
    )
    metrics = perf.diff(base_metrics, pgo_metrics)
    result = {
        "ok": True,
        "baseline_preset": baseline_preset.name,
        "pgo_preset": use_preset.name,
        "metrics": metrics,
        "baseline_count": len(base_metrics),
        "pgo_count": len(pgo_metrics),
    }
    write_sidecar(
        use_preset.build_dir, "pgo-measure.json",
        {"timestamp": datetime.now().isoformat(timespec="seconds"), **result},
    )
    return result


def pgo_run(
    gen_preset: Preset, use_preset: Preset, baseline_preset: Preset, binary_names: list[str], *,
    root: Path, measure: bool = True, timeout: float | None = None,
    mirror: bool = False, verbose: bool = False,
) -> dict:
    """Full pipeline: instrument → train → optimize → (measure).

    Returns {ok, train, measure}. Stops early (ok=False) if instrument, train, or
    optimize fails; `measure` is None when skipped or not reached.
    """
    inst = pgo_instrument(gen_preset, root=root, mirror=mirror, verbose=verbose)
    if not all(s.ok for s in inst):
        return {"ok": False, "stage": "instrument", "train": None, "measure": None}

    train = pgo_train(gen_preset, binary_names, root=root, timeout=timeout, mirror=mirror, verbose=verbose)
    if not train["ok"]:
        return {"ok": False, "stage": "train", "train": train, "measure": None}

    opt = pgo_optimize(use_preset, root=root, mirror=mirror, verbose=verbose)
    if not all(s.ok for s in opt):
        return {"ok": False, "stage": "optimize", "train": train, "measure": None}

    measure_result = None
    if measure:
        measure_result = pgo_measure(
            baseline_preset, use_preset, binary_names, root=root,
            build_first=True, timeout=timeout, mirror=mirror, verbose=verbose,
        )
    return {"ok": True, "stage": "done", "train": train, "measure": measure_result}
