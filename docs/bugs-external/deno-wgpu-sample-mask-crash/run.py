#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///

"""Runs repro.mjs under deno's vulkan backend, and says whether the crash is still there.

Exit 1 when it reproduces, 0 when it does not, and 2 when no deno could be found, which is neither answer.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    deno = shutil.which("deno") or shutil.which(str(Path.home() / "tools" / "deno" / "deno"))
    if deno is None:
        print("no deno found")
        return 2

    env = {**os.environ, "DENO_WEBGPU_BACKEND": "vulkan"}
    argv = [deno, "run", "--allow-all", "--unstable-webgpu", str(HERE / "repro.mjs"), "mask", "read"]
    p = subprocess.run(argv, capture_output=True, text=True, errors="replace", env=env, timeout=60)
    print(p.stdout + p.stderr)

    reproduced = p.returncode != 0
    print("REPRODUCED" if reproduced else "not reproduced", f"(exit {p.returncode & 0xffffffff:#x})")
    return 1 if reproduced else 0


if __name__ == "__main__":
    sys.exit(main())
