#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib"]
# ///

"""
Run the async fork-floor sweep and chart it.

Drives `bench-async-fork-floor (thread sweep)` through dev.py in release, parses the FLOORCSV rows, and writes
one PNG: x = element count, y = total ns for the whole graph, both log; one line per pool worker count on a
viridis gradient.

The question it answers: a graph that forks even once costs ~11 us regardless of size, while an un-split single
node costs ~0.3 us. Is that a FIXED handoff cost (lines flat across worker counts) or contention (lines fan out
as the pool grows)?

Usage:
    uv run libs/base/clean-core/tests/benchmarks/async/fork-floor-plot.py
    uv run .../fork-floor-plot.py --input raw.txt     # re-plot a previous capture, no re-run
    uv run .../fork-floor-plot.py --linear-y

The sweep takes ~4 minutes; every run saves its raw stdout next to the PNG so --input can replot for free.
"""

import argparse
import pathlib
import re
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import cm, colors

TEST_NAME = "bench-async-fork-floor (thread sweep)"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[6]

# FLOORCSV <workers>,<n>,<ns_total> -- the header row is non-numeric and so is skipped for free.
ROW_RE = re.compile(r"^FLOORCSV\s+(\d+),(\d+),([0-9.eE+-]+)\s*$")


def run_benchmark(preset: str) -> str:
    """Run the sweep via dev.py and return its stdout. Raises if dev.py reports failure."""
    cmd = [
        "uv",
        "run",
        "dev.py",
        "--mirror-test-output",  # dev.py is quiet by default; this streams the binary's stdout to ours
        "--plain",
        "test",
        TEST_NAME,
        "--preset",
        preset,  # --preset is per-subcommand: it goes after `test`
        "--timeout",
        "0",  # ~4 min sweep; dev.py's default 60 s per-binary timeout would kill it mid-table
    ]
    print(f"$ {' '.join(cmd)}", file=sys.stderr)
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"dev.py failed (exit {proc.returncode})")
    return proc.stdout


def parse(text: str) -> dict[int, list[tuple[int, float]]]:
    """-> {workers: [(n, ns_total), ...] sorted by n}"""
    out: dict[int, list[tuple[int, float]]] = {}
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        out.setdefault(int(m.group(1)), []).append((int(m.group(2)), float(m.group(3))))
    for pts in out.values():
        pts.sort()
    return out


def plot(workers: dict[int, list[tuple[int, float]]], out_path: pathlib.Path, log_y: bool) -> None:
    keys = sorted(workers)
    # The gradient IS the worker-count axis, so it gets a colorbar rather than one legend entry per line.
    norm = colors.Normalize(vmin=min(keys), vmax=max(keys))
    cmap = cm.viridis

    fig, ax = plt.subplots(figsize=(11, 6.5))
    for w in keys:
        pts = workers[w]
        ax.plot(
            [n for n, _ in pts],
            [v for _, v in pts],
            color=cmap(norm(w)),
            marker="o",
            markersize=2.5,
            linewidth=1.3,
        )

    ax.set_xscale("log", base=2)
    if log_y:
        ax.set_yscale("log")
    ax.set_xlabel("elements (n) — grain 1, so this is also the leaf count")
    ax.set_ylabel("ns per pass (whole graph)")
    ax.set_title("cc::async_thread_pool fork floor — parallel-for, grain 1")
    ax.grid(True, which="both", alpha=0.25, linewidth=0.5)

    sm = cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    fig.colorbar(sm, ax=ax, label="pool workers (+ the participating caller)")

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"wrote {out_path}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="release-clang", help="dev.py preset (default: release-clang)")
    ap.add_argument("--input", type=pathlib.Path, help="parse this captured stdout instead of re-running")
    ap.add_argument("--out-dir", type=pathlib.Path, help="default: build/bench-async-fork-floor/")
    ap.add_argument("--linear-y", action="store_true", help="linear y axis")
    args = ap.parse_args()

    out_dir = args.out_dir or (REPO_ROOT / "build" / "bench-async-fork-floor")
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.input:
        text = args.input.read_text()
    else:
        text = run_benchmark(args.preset)
        raw = out_dir / "raw.txt"
        raw.write_text(text)
        print(f"wrote {raw}", file=sys.stderr)

    data = parse(text)
    if not data:
        raise SystemExit("no FLOORCSV rows found -- did the test run?")

    plot(data, out_dir / "async-fork-floor.png", log_y=not args.linear_y)


if __name__ == "__main__":
    main()
