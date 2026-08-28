#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib"]
# ///

"""
Run the async grain sweep and chart it.

Drives `bench-async-grain - grain x size sweep` through `dev.py benchmark` in release, reads its JSON sidecar,
and writes two PNGs per case (parallel-for, reduction), one line per grain value on a viridis gradient:

    <case>.png        x = element count, y = ns per input element
    <case>-total.png  x = element count, y = ns for the whole pass

Both axes log (the sweep spans ~4 decades; a linear y buries everything under the small-n end). The two views
answer different questions: per-element shows the per-node cost as the vertical gap between grain lines, while
total time shows the fixed submit/drive overhead as a flat left-hand plateau.

Usage:
    uv run libs/base/clean-core/tests/benchmarks/async/grain-plot.py
    uv run .../grain-plot.py --linear-y              # linear y, if you really want it
    uv run .../grain-plot.py --input sweep.json      # re-plot a previous capture, no re-run
    uv run .../grain-plot.py --preset relwithdebinfo-clang

The sidecar carries the median and its confidence interval over hundreds of samples per point.
This script used to scrape `GRAINCSV` rows off stdout; the harness does that job better, so it does not any more.

The sweep takes ~20 s; every run saves its sidecar next to the PNGs so --input can replot it for free.
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

BENCHMARK_NAME = "bench-async-grain - grain x size sweep"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[6]

CASES = {
    "pfor": ("parallel-for transform", "async-grain-pfor"),
    "reduce": ("reduction", "async-grain-reduce"),
}


def run_benchmark(preset: str) -> dict:
    """Run the sweep through `dev.py benchmark` and return its parsed JSON sidecar."""
    with tempfile.TemporaryDirectory() as tmp:
        sidecar = pathlib.Path(tmp) / "sweep.json"
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
        written = sorted(pathlib.Path(tmp).glob("sweep*.json"))
        if not written:
            raise SystemExit(f"{BENCHMARK_NAME!r} wrote no sidecar -- did it run?")
        return json.loads(written[0].read_text(encoding="utf-8"))


def parse(sidecar: dict) -> dict[str, dict[int, list[tuple[int, float]]]]:
    """-> {case: {grain: [(n, ns_per_elem), ...] sorted by n}}

    Loop names are `<case> n=<n> grain=<g>`, which is the contract with the benchmark.
    """
    out: dict[str, dict[int, list[tuple[int, float]]]] = {}
    for loop in sidecar.get("loops", []):
        parts = str(loop.get("loop", "")).split()
        if len(parts) != 3 or not parts[1].startswith("n=") or not parts[2].startswith("grain="):
            continue
        case, n, grain = parts[0], int(parts[1][2:]), int(parts[2][6:])

        rate = float(loop.get("items_per_second", 0.0))
        if rate <= 0:
            continue
        out.setdefault(case, {}).setdefault(grain, []).append((n, 1e9 / rate))
    for grains in out.values():
        for pts in grains.values():
            pts.sort()
    return out


def plot(case: str, grains: dict[int, list[tuple[int, float]]], out_path: pathlib.Path, log_y: bool,
         total: bool) -> None:
    """total=False gives ns per input element.
    total=True gives ns for the whole pass, which turns the fixed submit/drive overhead into a flat left-hand plateau instead of a hyperbola.
    """
    title, _ = CASES[case]
    keys = sorted(grains)
    # Gradient over the grain EXPONENT, so the powers of two are evenly spaced in color as well as in meaning.
    norm = colors.Normalize(vmin=0, vmax=max(1, len(keys) - 1))
    cmap = cm.viridis

    fig, ax = plt.subplots(figsize=(11, 6.5))
    for i, g in enumerate(keys):
        pts = grains[g]
        ax.plot(
            [n for n, _ in pts],
            [v * n if total else v for n, v in pts],
            color=cmap(norm(i)),
            marker="o",
            markersize=3,
            linewidth=1.6,
            label=f"grain {g}",
        )

    ax.set_xscale("log", base=2)
    if log_y:
        ax.set_yscale("log")
    ax.set_xlabel("elements (n)")
    ax.set_ylabel("ns per pass (whole graph)" if total else "ns per input element")
    ax.set_title(f"cc::async_thread_pool grain sweep — {title}" + (" — total time" if total else ""))
    ax.grid(True, which="both", alpha=0.25, linewidth=0.5)
    ax.legend(ncol=2, fontsize=8, title="leaf cutoff", framealpha=0.9)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"wrote {out_path}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="release-clang", help="dev.py preset (default: release-clang)")
    ap.add_argument("--input", type=pathlib.Path, help="parse this captured .json sidecar instead of re-running")
    ap.add_argument("--out-dir", type=pathlib.Path, help="default: build/bench-async-grain/")
    ap.add_argument("--linear-y", action="store_true",
                    help="linear y axis; the data spans ~4 decades, so the small-n end flattens everything else")
    args = ap.parse_args()

    out_dir = args.out_dir or (REPO_ROOT / "build" / "bench-async-grain")
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.input:
        sidecar = json.loads(args.input.read_text(encoding="utf-8"))
    else:
        sidecar = run_benchmark(args.preset)
        raw = out_dir / "sweep.json"
        raw.write_text(json.dumps(sidecar), encoding="utf-8")
        print(f"wrote {raw}", file=sys.stderr)

    data = parse(sidecar)
    if not data:
        raise SystemExit("the sidecar carried no `<case> n=<n> grain=<g>` loops -- did the sweep run?")

    for case, (_, stem) in CASES.items():
        if case not in data:
            print(f"warning: no rows for case {case!r}", file=sys.stderr)
            continue
        log_y = not args.linear_y
        plot(case, data[case], out_dir / f"{stem}.png", log_y=log_y, total=False)
        plot(case, data[case], out_dir / f"{stem}-total.png", log_y=log_y, total=True)


if __name__ == "__main__":
    main()
