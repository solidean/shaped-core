#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.7"]
# ///
"""Run the node-allocation design benchmark and plot the fast-path variants.

The benchmark is compiled out in the tree: set CC_BENCH_NODE_ALLOCATION_DESIGN to 1
at the top of tests/benchmarks/node-allocation-design-benchmark.cc before running this.

Executes `bench-node-design - fast-path variants` via `dev.py benchmark` and reads
its JSON sidecar (or parses a captured sidecar with --input), then writes two SVGs:
throughput in M alloc+free pairs/s and in GB/s, versus allocation size (log2 X).
Each variant is a line; cache-line placement is encoded as solid (same) vs dashed
(diff), variant family as color.

The sidecar carries the median and its confidence interval, computed over hundreds of samples.
This script used to scrape `RESULT,` CSV rows off stdout and median three timed runs itself; the
harness does that better, so it does not any more.

    uv run libs/base/clean-core/scripts/plot-node-allocation-design.py
    uv run .../plot-node-allocation-design.py --input run.bench.json --out /tmp
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

ROOT = Path(__file__).resolve().parents[4]  # scripts -> clean-core -> base -> libs -> repo root
BENCHMARK_NAME = "bench-node-design - fast-path variants"

# Draw order + style per variant.
# Okabe-Ito colorblind-safe palette; solid = metadata in one cache line, dashed = remote bitmap on a 2nd line, and the references (single/mimalloc/system) get their own dashes.
# "node" is the REAL shipped cc::node_allocator — drawn thick black on top so it reads as the ground truth.
# key: (label, color, linestyle, marker, linewidth, zorder)
STYLE: dict[str, tuple] = {
    "node":           ("node — real cc::node_allocator", "#000000", "-",           "*", 3.2, 9),
    "single":         ("single — no atomics (floor)",   "#000000", (0, (1, 1)),   ".", 1.6, 2),
    "step2_tls_same": ("step2 tls · same line",         "#009E73", "-",           "o", 2.2, 6),
    "step2_tls_diff": ("step2 tls · diff line",         "#009E73", (0, (5, 2)),   "s", 1.8, 5),
    "step2_teb_same": ("step2 teb · same line",         "#CC79A7", "-",           "o", 2.2, 6),
    "step2_teb_diff": ("step2 teb · diff line",         "#CC79A7", (0, (5, 2)),   "s", 1.8, 5),
    "step1_same":     ("step1 · same line",             "#0072B2", "-",           "o", 2.2, 6),
    "step1_diff":     ("step1 · diff line",             "#0072B2", (0, (5, 2)),   "s", 1.8, 5),
    "atomic":         ("atomic — current (2 locks)",    "#D55E00", "-",           "D", 2.8, 7),
    "mimalloc":       ("mimalloc",                      "#E69F00", (0, (3, 1, 1, 1)), "^", 2.0, 3),
    "system":         ("system malloc",                 "#999999", (0, (1, 2)),   "v", 1.6, 2),
}


def capture_benchmark(preset: str, target: str) -> dict:
    """Run the benchmark through dev.py and return its parsed JSON sidecar."""
    with tempfile.TemporaryDirectory() as tmp:
        sidecar = Path(tmp) / "design.json"
        cmd = ["uv", "run", "dev.py", "benchmark", BENCHMARK_NAME,
               "--target", target, "--preset", preset, "--json", str(sidecar), "--timeout", "0"]
        print(f"running: {' '.join(cmd)}  (cwd={ROOT})", file=sys.stderr)
        proc = subprocess.run(cmd, cwd=ROOT, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"benchmark run failed (exit {proc.returncode})")

        # dev.py suffixes the sidecar with the binary it came from, so several binaries never overwrite one another.
        written = sorted(Path(tmp).glob("design*.json"))
        if not written:
            # The likely cause by far, and the failure it would otherwise become — an empty plot — says nothing.
            raise SystemExit(
                f"{BENCHMARK_NAME!r} wrote no sidecar.\n"
                f"The benchmark is compiled out in the tree: set CC_BENCH_NODE_ALLOCATION_DESIGN to 1 in\n"
                f"libs/base/clean-core/tests/benchmarks/node-allocation-design-benchmark.cc and rerun."
            )
        return json.loads(written[0].read_text(encoding="utf-8"))


def parse(sidecar: dict) -> dict[str, dict[int, tuple[float, float]]]:
    """Parse the sidecar's loops into {variant: {size: (M pairs/s, GB/s)}}.

    Loop names are `<variant> @<size>B`, which is the contract with the benchmark.
    Everything else in a loop — the interval, the samples, the outlier count — is there to be used by a richer
    plot later; this one takes the two throughput figures it has always drawn.
    """
    out: dict[str, dict[int, tuple[float, float]]] = {}
    for loop in sidecar.get("loops", []):
        name = str(loop.get("loop", ""))
        if " @" not in name or not name.endswith("B"):
            continue
        variant, _, size_text = name.partition(" @")
        size_text = size_text[:-1]
        if not size_text.isdigit():
            continue

        mops = float(loop.get("items_per_second", 0.0)) / 1e6

        gbps = 0.0
        for q in loop.get("quantities", []):
            if q.get("name") == "bytes":
                gbps = float(q.get("per_second", 0.0)) / 1e9

        out.setdefault(variant, {})[int(size_text)] = (mops, gbps)
    return out


def _plot(data, metric_idx: int, ylabel: str, title: str, logy: bool, path: Path) -> None:
    fig, ax = plt.subplots(figsize=(10.5, 6.5))
    for variant, (label, color, ls, marker, lw, z) in STYLE.items():
        if variant not in data:
            continue
        sizes = sorted(data[variant])
        ys = [data[variant][s][metric_idx] for s in sizes]
        ax.plot(sizes, ys, label=label, color=color, linestyle=ls, marker=marker,
                markersize=5, linewidth=lw, zorder=z)

    ax.set_xscale("log", base=2)
    all_sizes = sorted({s for v in data.values() for s in v})
    ax.set_xticks(all_sizes)
    ax.xaxis.set_major_formatter(ScalarFormatter())
    ax.set_xlabel("allocation size (bytes, log₂)")
    if logy:
        ax.set_yscale("log")
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.grid(True, which="both", axis="both", alpha=0.25, linewidth=0.6)
    ax.margins(x=0.02)
    ax.legend(loc="center left", bbox_to_anchor=(1.01, 0.5), fontsize=9, frameon=False,
              title="variant", title_fontsize=10)
    fig.text(0.01, 0.01, "node-allocation fast-path variants · batch alloc+free, single-thread · higher is better",
             fontsize=8, color="#666666")
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    fig.savefig(path, format="svg", bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")


def summarize(data) -> None:
    """Print a compact speedup summary at a representative size."""
    ref = 16
    if "atomic" not in data or ref not in data["atomic"]:
        return
    base = data["atomic"][ref][0]
    print(f"\nspeedup vs current 'atomic' at {ref} B (median M pairs/s):")
    for variant, (label, *_ ) in STYLE.items():
        if variant in data and ref in data[variant]:
            m = data[variant][ref][0]
            print(f"  {variant:16s} {m:7.1f}   {m / base:4.2f}x")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="release-clang", help="build preset (default: release-clang)")
    ap.add_argument("--target", default="clean-core-test", help="test target (default: clean-core-test)")
    ap.add_argument("--input", metavar="FILE", help="parse a captured .bench.json sidecar instead of executing it")
    ap.add_argument("--out", default=".", metavar="DIR", help="output directory for the SVGs (default: .)")
    args = ap.parse_args()

    sidecar = json.loads(Path(args.input).read_text(encoding="utf-8")) if args.input \
        else capture_benchmark(args.preset, args.target)

    data = parse(sidecar)
    if not data:
        raise SystemExit("the sidecar carried no `<variant> @<size>B` loops — did the benchmark run?")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    _plot(data, 0, "throughput (M alloc+free pairs / s)",
          "node-allocation fast-path variants — throughput", logy=False,
          path=out / "node-alloc-design-mops.svg")
    _plot(data, 1, "throughput (GB / s)",
          "node-allocation fast-path variants — bandwidth", logy=True,
          path=out / "node-alloc-design-gbps.svg")
    summarize(data)


if __name__ == "__main__":
    main()
