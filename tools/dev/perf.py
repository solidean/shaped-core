"""Perf: run guide benchmarks and collect their recorded metrics.

A guide benchmark (nexus GUIDE_BENCHMARK) records named metrics via nx::guide and,
when run with `--perf-json <file>`, writes a sidecar of them. This module runs the
guide-benchmark bucket across the given test binaries (reusing the standard test
runner) and parses those sidecars into a flat metric list. `dev.py pgo` uses it to
train (the run itself drives the instrumented binaries) and to measure (baseline
vs PGO), diffing the two metric sets.

Public API:
    run_and_collect(preset, binaries, ...) -> list[dict]   run guide benchmarks, parse metrics
    diff(baseline, pgo)               -> list[dict]   match by (binary, test, name), % change
"""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path

from .models import Preset
from .test import test as run_tests


def _parse_sidecar(path: Path, binary: str) -> list[dict]:
    """Read one binary's .perf.json into a list of metric dicts tagged with `binary`."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    out: list[dict] = []
    for m in data.get("metrics", []):
        out.append({
            "binary": binary,
            "test": m.get("test", ""),
            "name": m.get("name", ""),
            "value": float(m.get("value", 0.0)),
            "unit": m.get("unit", ""),
            "higher_is_better": bool(m.get("higher_is_better", True)),
        })
    return out


def run_and_collect(
    preset: Preset, binary_names: list[str], *, root: Path, perf_dir: Path,
    extra_env_for: Callable[[str], dict[str, str]] | None = None,
    timeout: float | None = None, mirror: bool = False, verbose: bool = False,
) -> list[dict]:
    """Run the guide-benchmark bucket of each binary, collecting recorded metrics.

    Each binary is run once with `--guide-benchmarks --perf-json <perf_dir>/<binary>.perf.json`
    (binaries with no guide benchmarks exit 0 and write no sidecar — they just
    contribute nothing). `extra_env_for` injects per-binary env (the PGO training
    run uses it for LLVM_PROFILE_FILE). Returns the flat list of parsed metrics.
    """
    perf_dir.mkdir(parents=True, exist_ok=True)
    metrics: list[dict] = []
    for binary in binary_names:
        sidecar = perf_dir / f"{binary}.perf.json"
        sidecar.unlink(missing_ok=True)
        run_tests(
            [preset], [binary], root=root,
            extra_args=["--guide-benchmarks", "--perf-json", str(sidecar)],
            extra_env_for=extra_env_for,
            timeout=timeout, write_xml=False, mirror=mirror, verbose=verbose,
        )
        if sidecar.is_file():
            metrics.extend(_parse_sidecar(sidecar, binary))
    return metrics


def _key(m: dict) -> tuple[str, str, str]:
    return (m["binary"], m["test"], m["name"])


def diff(baseline: list[dict], pgo: list[dict]) -> list[dict]:
    """Match metrics by (binary, test, name) and compute the oriented % change.

    `delta_pct` is positive when PGO is better (orientation respects
    higher_is_better), so a speedup reads positive for throughput and for latency
    alike. Metrics present in only one set are skipped.
    """
    pgo_by_key = {_key(m): m for m in pgo}
    out: list[dict] = []
    for b in baseline:
        p = pgo_by_key.get(_key(b))
        if p is None:
            continue
        base_v, pgo_v = b["value"], p["value"]
        if base_v == 0:
            delta = 0.0
        elif b["higher_is_better"]:
            delta = 100.0 * (pgo_v - base_v) / base_v
        else:
            delta = 100.0 * (base_v - pgo_v) / base_v
        out.append({
            "binary": b["binary"],
            "test": b["test"],
            "name": b["name"],
            "unit": b["unit"],
            "higher_is_better": b["higher_is_better"],
            "baseline": base_v,
            "pgo": pgo_v,
            "delta_pct": round(delta, 2),
        })
    return out
