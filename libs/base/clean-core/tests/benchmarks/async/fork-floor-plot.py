#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib"]
# ///

"""
Run the async fork-floor sweep and chart it.

Drives `bench-async-grain - fork floor thread sweep` through `dev.py benchmark` in release, reads its JSON
sidecar, and writes one PNG: x = element count, y = total ns for the whole graph, both log; one line per pool
worker count on a viridis gradient.

The question it answers: a graph that forks even once costs far more than an un-split single node, whatever its size.
Is that a FIXED handoff cost (lines flat across worker counts) or contention (lines fan out as the pool grows)?

Usage:
    uv run libs/base/clean-core/tests/benchmarks/async/fork-floor-plot.py
    uv run .../fork-floor-plot.py --input floor.json  # re-plot a previous capture, no re-run
    uv run .../fork-floor-plot.py --linear-y

The sweep takes ~15 s; every run saves its sidecar next to the PNG so --input can replot for free.
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import cm, colors

BENCHMARK_NAME = "bench-async-grain - fork floor thread sweep"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[6]


def run_benchmark(preset: str) -> dict:
    """Run the sweep through `dev.py benchmark` and return its parsed JSON sidecar."""
    with tempfile.TemporaryDirectory() as tmp:
        sidecar = pathlib.Path(tmp) / "floor.json"
        cmd = [
            "uv", "run", "dev.py", "--plain", "benchmark", BENCHMARK_NAME,
            "--preset", preset,  # --preset is per-subcommand: it goes after `benchmark`
            "--json", str(sidecar),
            "--timeout", "0",  # dev.py's default 60 s per-binary timeout would cut the sweep off partway through
        ]
        print(f"$ {' '.join(cmd)}", file=sys.stderr)
        proc = subprocess.run(cmd, cwd=REPO_ROOT, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"dev.py failed (exit {proc.returncode})")

        # dev.py suffixes the sidecar with the binary it came from, so several binaries never overwrite one another.
        written = sorted(pathlib.Path(tmp).glob("floor*.json"))
        if not written:
            raise SystemExit(f"{BENCHMARK_NAME!r} wrote no sidecar -- did it run?")
        return json.loads(written[0].read_text(encoding="utf-8"))


def parse(sidecar: dict) -> dict[int, list[tuple[int, float]]]:
    """-> {workers: [(n, ns_total), ...] sorted by n}

    Loop names are `w=<workers> n=<n>`, which is the contract with the benchmark.
    The benchmark declares no items, so the median IS the cost of the whole pass -- which is the y axis here.
    """
    out: dict[int, list[tuple[int, float]]] = {}
    for loop in sidecar.get("loops", []):
        parts = str(loop.get("loop", "")).split()
        if len(parts) != 2 or not parts[0].startswith("w=") or not parts[1].startswith("n="):
            continue
        median = float(loop.get("statistics", {}).get("median", 0.0))
        if median <= 0:
            continue
        out.setdefault(int(parts[0][2:]), []).append((int(parts[1][2:]), median * 1e9))
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
    ap.add_argument("--input", type=pathlib.Path, help="parse this captured .json sidecar instead of re-running")
    ap.add_argument("--out-dir", type=pathlib.Path, help="default: build/bench-async-fork-floor/")
    ap.add_argument("--linear-y", action="store_true", help="linear y axis")
    args = ap.parse_args()

    out_dir = args.out_dir or (REPO_ROOT / "build" / "bench-async-fork-floor")
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.input:
        sidecar = json.loads(args.input.read_text(encoding="utf-8"))
    else:
        sidecar = run_benchmark(args.preset)
        raw = out_dir / "floor.json"
        raw.write_text(json.dumps(sidecar), encoding="utf-8")
        print(f"wrote {raw}", file=sys.stderr)

    data = parse(sidecar)
    if not data:
        raise SystemExit("the sidecar carried no `w=<workers> n=<n>` loops -- did the sweep run?")

    plot(data, out_dir / "async-fork-floor.png", log_y=not args.linear_y)


if __name__ == "__main__":
    main()
