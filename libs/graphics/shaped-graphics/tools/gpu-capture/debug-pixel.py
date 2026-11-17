#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Debugs one pixel of a RenderDoc capture and prints what the shader debugger saw, as JSON.

    uv run libs/graphics/shaped-graphics/tools/gpu-capture/debug-pixel.py <capture.rdc> <x> <y> [--draw N] [--out result.json]

A capture comes from any sg program run with `SC_SG_CAPTURE=renderdoc` (libs/graphics/shaped-graphics/docs/concepts/gpu-capture.md).
The replay runs in RenderDoc's own UI process, whose embedded Python is the only one with the `renderdoc` module, through renderdoc_replay.py beside this file.
The result names each stage's debug files, the source variables, and every step's file, line, call stack and variables.
"""

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def find_qrenderdoc() -> pathlib.Path | None:
    if found := os.environ.get("SC_QRENDERDOC"):
        return pathlib.Path(found)
    root = os.environ.get("ProgramFiles")
    if root and (candidate := pathlib.Path(root) / "RenderDoc" / "qrenderdoc.exe").exists():
        return candidate
    for name in ("qrenderdoc", "qrenderdoc.exe"):
        for directory in os.environ.get("PATH", "").split(os.pathsep):
            if (candidate := pathlib.Path(directory) / name).exists():
                return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("x", type=int)
    parser.add_argument("y", type=int)
    parser.add_argument("--draw", type=int, default=-1, help="which draw of the capture, as a Python index; the last by default")
    parser.add_argument("--out", type=pathlib.Path, help="where the JSON goes; stdout by default")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds RenderDoc gets before it is stopped")
    args = parser.parse_args()

    qrenderdoc = find_qrenderdoc()
    if qrenderdoc is None:
        print("qrenderdoc was not found: install RenderDoc, or name it in SC_QRENDERDOC", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as scratch:
        request = pathlib.Path(scratch) / "request.json"
        result = pathlib.Path(scratch) / "result.json"
        request.write_text(json.dumps({"capture": str(args.capture.resolve()), "x": args.x, "y": args.y, "draw": args.draw}))
        env = dict(os.environ, SG_RENDERDOC_REQUEST=str(request), SG_RENDERDOC_RESULT=str(result))
        try:
            subprocess.run([str(qrenderdoc), "--python", str(HERE / "renderdoc_replay.py")], env=env, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"RenderDoc did not finish within {args.timeout:.0f} s", file=sys.stderr)
            return 1
        if not result.exists():
            print("RenderDoc ran, but the replay script wrote no result", file=sys.stderr)
            return 1
        text = result.read_text(encoding="utf-8")

    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text)
    return 1 if "error" in json.loads(text) else 0


if __name__ == "__main__":
    sys.exit(main())
