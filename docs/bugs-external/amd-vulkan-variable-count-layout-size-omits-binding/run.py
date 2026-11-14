#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the variable-count descriptor layout size reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl (clang++ off Windows) against the Vulkan SDK's headers and loader library, and runs it once.

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
`--vendor` picks the device, `1002` for AMD; the first discrete GPU otherwise.

Exits as repro.exe does: 1 when a variable-count layout is smaller than the spec allows, 0 when none is, 2 when it could not run.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent


def find_vulkan(given: str | None) -> Path | None:
    candidates = [given, os.environ.get("VULKAN_SDK")]
    tools = Path.home() / "tools" / "vulkan-sdk"
    if tools.is_dir():
        candidates += [str(p) for p in sorted(tools.iterdir(), reverse=True)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "include" / "vulkan" / "vulkan.h").is_file():
            return Path(candidate)
    return None


def build(workdir: Path, sdk: Path) -> Path | None:
    exe = workdir / ("repro.exe" if sys.platform == "win32" else "repro")
    if sys.platform == "win32":
        if not shutil.which("clang-cl"):
            print("no clang-cl on PATH", file=sys.stderr)
            return None
        cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", f"/I{sdk / 'include'}", str(HERE / "repro.cc"),
               f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}", "/link", f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib"]
    else:
        cmd = ["clang++", "-O2", "-std=c++20", f"-I{sdk / 'include'}", str(HERE / "repro.cc"), "-o", str(exe),
               f"-L{sdk / 'lib'}", "-lvulkan"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout, out.stderr, file=sys.stderr)
        return None
    return exe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vulkan-sdk")
    parser.add_argument("--vendor", help="hex PCI vendor of the device, 1002 for AMD")
    options = parser.parse_args()
    sdk = find_vulkan(options.vulkan_sdk)
    if sdk is None:
        print("no Vulkan SDK found; pass --vulkan-sdk <dir>")
        return 2
    # one stable directory rather than a fresh temporary one, so reruns leave no litter
    workdir = Path(tempfile.gettempdir()) / "amd-vulkan-variable-count-layout-size-repro"
    workdir.mkdir(exist_ok=True)
    exe = build(workdir, sdk)
    if exe is None:
        return 2
    try:
        out = subprocess.run([str(exe), *(["--vendor", options.vendor] if options.vendor else [])], timeout=60)
    except subprocess.TimeoutExpired:
        print("timed out")
        return 2
    return out.returncode


if __name__ == "__main__":
    sys.exit(main())
