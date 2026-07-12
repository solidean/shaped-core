#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.7"]
# ///
"""Run the node-allocation design benchmark and plot the fast-path variants.

Executes `bench-node-design (fast-path variants)` via dev.py (or parses a captured
run with --input), medians the 3 runs per (variant, size), and writes two SVGs:
throughput in M alloc+free pairs/s and in GB/s, versus allocation size (log2 X).
Each variant is a line; cache-line placement is encoded as solid (same) vs dashed
(diff), variant family as color.

    uv run libs/base/clean-core/scripts/plot-node-allocation-design.py
    uv run .../plot-node-allocation-design.py --input run.txt --out /tmp
"""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

ROOT = Path(__file__).resolve().parents[4]  # scripts -> clean-core -> base -> libs -> repo root
TEST_NAME = "bench-node-design (fast-path variants)"

# Draw order + style per variant. Okabe-Ito colorblind-safe palette; solid = metadata in one cache line,
# dashed = remote bitmap on a 2nd line; references (single/mimalloc/system) get their own dashes.
# key: (label, color, linestyle, marker, linewidth, zorder)
STYLE: dict[str, tuple] = {
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


def capture_benchmark(preset: str, target: str) -> str:
    """Run the benchmark through dev.py with mirrored output and return its stdout."""
    cmd = ["uv", "run", "dev.py", "--mirror-output", "test", TEST_NAME,
           "--target", target, "--preset", preset, "--timeout", "0"]
    print(f"running: {' '.join(cmd)}  (cwd={ROOT})", file=sys.stderr)
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"benchmark run failed (exit {proc.returncode})")
    return proc.stdout


def parse(text: str) -> dict[str, dict[int, tuple[float, float]]]:
    """Parse RESULT rows into {variant: {size: (median_mops, median_gbps)}}."""
    raw: dict[str, dict[int, list[tuple[float, float]]]] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("RESULT,"):
            continue
        _, variant, size, run, mops, gbps = line.split(",")
        if not size.isdigit():  # skip the header row
            continue
        raw.setdefault(variant, {}).setdefault(int(size), []).append((float(mops), float(gbps)))
    out: dict[str, dict[int, tuple[float, float]]] = {}
    for variant, per_size in raw.items():
        out[variant] = {
            sz: (statistics.median(m for m, _ in vals), statistics.median(g for _, g in vals))
            for sz, vals in per_size.items()
        }
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
    ap.add_argument("--input", metavar="FILE", help="parse a captured benchmark run instead of executing it")
    ap.add_argument("--out", default=".", metavar="DIR", help="output directory for the SVGs (default: .)")
    args = ap.parse_args()

    text = Path(args.input).read_text(encoding="utf-8", errors="replace") if args.input \
        else capture_benchmark(args.preset, args.target)

    data = parse(text)
    if not data:
        raise SystemExit("no RESULT rows found — did the benchmark run?")

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
