#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Runs repro.mjs under every JS runtime with WebGPU it can find, and says whether the bug is still there.

Exit 1 when it reproduces — some implementation accepts 4294967295 as a timestamp write index — and 0 when none does.
Exit 2 when no runtime could be driven at all, which is neither answer.

node needs the `webgpu` npm package; pass --node-modules DIR to point at a node_modules that has it, or let the
script find the one shaped-core installs under tools/dev/js.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPRO = HERE / "repro.mjs"


def run(label: str, argv: list[str], env: dict[str, str]) -> tuple[str, str]:
    """The runtime's verdict and its raw output; 'skip' when it had no WebGPU."""
    try:
        p = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as e:
        return "skip", f"{label}: could not run ({e})"

    out = (p.stdout or "") + (p.stderr or "")
    if p.returncode == 2 or "SKIP:" in out:
        return "skip", out.strip()
    if "end = 4294967295 : ACCEPTED" in out:
        return "accepts", out.strip()
    if "end = 4294967295 : REFUSED" in out:
        return "refuses", out.strip()
    return "skip", out.strip()


def find_node() -> str | None:
    """node on PATH, else the one emsdk bundles, found the way dev.py finds it: SC_EMSDK_PATH, then EMSDK."""
    on_path = shutil.which("node")
    if on_path:
        return on_path
    for var in ("SC_EMSDK_PATH", "EMSDK"):
        root = os.environ.get(var)
        if not root:
            continue
        for pattern in ("node/*/bin/node.exe", "node/*/bin/node", "node/*/node.exe", "node/*/node"):
            hits = sorted(Path(root).glob(pattern))
            if hits:
                return str(hits[-1])
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--node", default=find_node())
    ap.add_argument("--deno", default=shutil.which("deno"))
    ap.add_argument("--node-modules", default=None, help="a node_modules directory holding the `webgpu` package")
    args = ap.parse_args()

    # The one shaped-core installs, so the common case needs no argument; any other tree passes --node-modules.
    node_modules = args.node_modules
    if node_modules is None:
        guess = HERE.parents[2] / "tools" / "dev" / "js" / "node_modules"
        node_modules = str(guess) if guess.is_dir() else None

    results: dict[str, tuple[str, str]] = {}
    if args.node:
        env = dict(os.environ)
        if node_modules:
            env["NODE_PATH"] = node_modules
        results["node (Dawn)"] = run("node", [args.node, str(REPRO)], env)
    if args.deno:
        results["deno (wgpu)"] = run("deno", [args.deno, "run", "--unstable-webgpu", str(REPRO)], dict(os.environ))

    for label, (verdict, output) in results.items():
        print(f"== {label}: {verdict} ==")
        print(output)
        print()

    verdicts = {label: verdict for label, (verdict, _) in results.items()}
    if not verdicts or all(v == "skip" for v in verdicts.values()):
        print("no runtime could be driven; install node with the `webgpu` package, or deno")
        return 2

    accepting = [label for label, v in verdicts.items() if v == "accepts"]
    if accepting:
        print(f"REPRODUCED: {', '.join(accepting)} accept an out-of-range timestamp write index of 4294967295")
        return 1

    print("gone: every runtime refuses 4294967295, so the sentinel no longer passes as 'no write'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
