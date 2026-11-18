#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Holds RenderDoc's shader debugger to what it should see of an SGL shader: its source file, its lines, its variables.

    uv run libs/graphics/shaped-graphics/tools/gpu-capture/check-debugger.py [--backend dx12|vulkan|both] [--debug-info lines|full]

It captures sgl-cube through `uv run dev.py example sgl-cube --capture`, with SC_SG_CAPTURE and SC_SG_NATIVE_DEBUG_INFO set,
debugs one pixel of its draw through debug-pixel.py, and checks the result against the expectations below.
It needs RenderDoc installed, which CI lacks, so it is run by hand: before and after a change to what SGL's binaries carry of the source.
Exit 0 when every expectation holds, 1 when one fails, 2 when the tools are missing.
"""

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[4]

# What a debugger sees of sgl-cube's pixel stage at the target's centre, from examples/graphics/sgl-cube/shaders/cube.sgl.
# The lines are main_ps's body and what it inlines: shade's test, its normalize, its loop and its return, and strength_of_light.
CUBE_FILE = "cube.sgl"
CUBE_LINES = {41, 47, 49, 51, 52, 54, 55, 59}
CUBE_PIXEL = (640, 360)
# The locals a debugger lists once variables are described (EMIT-267); checked at `full`.
CUBE_VARIABLES = {"lit", "n", "light", "i"}


def capture(backend: str, debug_info: str, into: pathlib.Path) -> pathlib.Path | None:
    stem = into / f"cube-{backend}"
    env = dict(
        os.environ,
        SC_SG_CAPTURE="renderdoc",
        SC_SG_CAPTURE_EPOCH="3",
        SC_SG_CAPTURE_FILE=str(stem),
        SC_SG_NATIVE_DEBUG_INFO=debug_info,
    )
    command = ["uv", "run", "dev.py", "example", "sgl-cube", "--capture"]
    if backend == "vulkan":
        command += ["--example-backend", "vulkan"]
    run = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
    if run.returncode != 0:
        print(run.stdout[-4000:], run.stderr[-4000:], sep="\n", file=sys.stderr)
        print(f"{backend}: the capture run failed, exit {run.returncode}", file=sys.stderr)
        return None
    written = sorted(into.glob(f"cube-{backend}*.rdc"))
    if not written:
        print(f"{backend}: sgl-cube ran but wrote no capture; is RenderDoc installed?", file=sys.stderr)
    return written[-1] if written else None


def debug(capture_path: pathlib.Path, into: pathlib.Path) -> dict | None:
    out = into / (capture_path.stem + ".json")
    x, y = CUBE_PIXEL
    run = subprocess.run(
        [sys.executable, str(HERE / "debug-pixel.py"), str(capture_path), str(x), str(y), "--out", str(out)],
        capture_output=True,
        text=True,
    )
    if not out.exists():
        print(run.stdout, run.stderr, sep="\n", file=sys.stderr)
        return None
    return json.loads(out.read_text(encoding="utf-8"))


def check(backend: str, debug_info: str, result: dict) -> list[str]:
    """Every expectation that does not hold, each as one line."""
    wrong = []
    if "error" in result:
        return [f"{backend}: {result['error']}"]
    pixel = result.get("stages", {}).get("pixel", {})
    files = [f["name"] for f in pixel.get("files", [])]
    if debug_info == "none":
        if files:
            wrong.append(f"{backend}: the pixel stage carries {files}, where nothing was asked for")
        return wrong
    if not any(f.endswith(CUBE_FILE) for f in files):
        wrong.append(f"{backend}: the pixel stage names no {CUBE_FILE}, only {files}")
    lines = {s["line"] for s in result.get("steps", []) if s.get("file", "") and s["file"].endswith(CUBE_FILE)}
    if missing := CUBE_LINES - lines:
        wrong.append(f"{backend}: no step stands on line(s) {sorted(missing)} of {CUBE_FILE}; stepped {sorted(lines)}")
    if extra := lines - CUBE_LINES:
        wrong.append(f"{backend}: steps stand on line(s) {sorted(extra)} of {CUBE_FILE}, which no statement of the pixel's path holds")
    if debug_info == "full":
        listed = {v["name"] for s in result.get("steps", []) for v in s.get("vars", [])}
        listed |= {v["name"] for v in result.get("source_vars", [])}
        # RenderDoc lists a vector by its lanes, `n.x`, `n.y` and `n.z`
        listed |= {name.split(".")[0] for name in listed}
        if missing := CUBE_VARIABLES - listed:
            wrong.append(f"{backend}: the debugger lists no variable {sorted(missing)}; it lists {sorted(listed)}")
    return wrong


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--backend", choices=["dx12", "vulkan", "both"], default="both")
    parser.add_argument("--debug-info", choices=["none", "lines", "full"], default="lines")
    parser.add_argument("--keep", type=pathlib.Path, help="a directory to keep the captures and results in")
    args = parser.parse_args()

    backends = ["dx12", "vulkan"] if args.backend == "both" else [args.backend]
    with tempfile.TemporaryDirectory() as scratch:
        into = args.keep or pathlib.Path(scratch)
        into.mkdir(parents=True, exist_ok=True)
        failures = []
        for backend in backends:
            captured = capture(backend, args.debug_info, into)
            if captured is None:
                return 2
            result = debug(captured, into)
            if result is None:
                print(f"{backend}: RenderDoc returned no result; is qrenderdoc installed?", file=sys.stderr)
                return 2
            wrong = check(backend, args.debug_info, result)
            failures += wrong
            steps = len(result.get("steps", []))
            print(f"{backend}: {'ok' if not wrong else 'FAILED'}, {steps} steps at debug info '{args.debug_info}'")
        for line in failures:
            print(line)
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
