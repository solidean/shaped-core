#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib"]
# ///

"""
Run the async grain sweep and chart it.

Drives `bench-async-grain (sweep)` through dev.py in release, parses the GRAINCSV rows it prints, and writes
two PNGs per case (parallel-for, reduction), one line per grain value on a viridis gradient:

    <case>.png        x = element count, y = ns per input element
    <case>-total.png  x = element count, y = ns for the whole pass

Both axes log (the sweep spans ~4 decades; a linear y buries everything under the small-n end). The two views
answer different questions: per-element shows the per-node cost as the vertical gap between grain lines, while
total time shows the fixed submit/drive overhead as a flat left-hand plateau.

Usage:
    uv run libs/base/clean-core/tests/benchmarks/async/grain-plot.py
    uv run .../grain-plot.py --linear-y              # linear y, if you really want it
    uv run .../grain-plot.py --input raw.txt         # re-plot a previous capture, no re-run
    uv run .../grain-plot.py --preset relwithdebinfo-clang

The sweep takes a couple of minutes; every run saves its raw stdout next to the PNGs so --input can replot it
without paying for it again.
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

TEST_NAME = "bench-async-grain (sweep)"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[6]

# GRAINCSV <case>,<n>,<grain>,<ns_per_elem> -- the header row has non-numeric fields and so is skipped for free.
ROW_RE = re.compile(r"^GRAINCSV\s+(\w+),(\d+),(\d+),([0-9.eE+-]+)\s*$")

CASES = {
    "pfor": ("parallel-for transform", "async-grain-pfor"),
    "reduce": ("reduction", "async-grain-reduce"),
}


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
        "0",  # the sweep runs ~5 min; dev.py's default 60 s per-binary timeout would kill it mid-table
    ]
    print(f"$ {' '.join(cmd)}", file=sys.stderr)
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"dev.py failed (exit {proc.returncode})")
    return proc.stdout


def parse(text: str) -> dict[str, dict[int, list[tuple[int, float]]]]:
    """-> {case: {grain: [(n, ns_per_elem), ...] sorted by n}}"""
    out: dict[str, dict[int, list[tuple[int, float]]]] = {}
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        case, n, grain, ns = m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))
        out.setdefault(case, {}).setdefault(grain, []).append((n, ns))
    for grains in out.values():
        for pts in grains.values():
            pts.sort()
    return out


def plot(case: str, grains: dict[int, list[tuple[int, float]]], out_path: pathlib.Path, log_y: bool,
         total: bool) -> None:
    """total=False: ns per input element. total=True: ns for the whole pass, which turns the fixed
    submit/drive overhead into a flat left-hand plateau instead of a hyperbola."""
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
    ap.add_argument("--input", type=pathlib.Path, help="parse this captured stdout instead of re-running")
    ap.add_argument("--out-dir", type=pathlib.Path, help="default: build/bench-async-grain/")
    ap.add_argument("--linear-y", action="store_true",
                    help="linear y axis; the data spans ~4 decades, so the small-n end flattens everything else")
    args = ap.parse_args()

    out_dir = args.out_dir or (REPO_ROOT / "build" / "bench-async-grain")
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
        raise SystemExit("no GRAINCSV rows found -- did the test run?")

    for case, (_, stem) in CASES.items():
        if case not in data:
            print(f"warning: no rows for case {case!r}", file=sys.stderr)
            continue
        log_y = not args.linear_y
        plot(case, data[case], out_dir / f"{stem}.png", log_y=log_y, total=False)
        plot(case, data[case], out_dir / f"{stem}-total.png", log_y=log_y, total=True)


if __name__ == "__main__":
    main()
