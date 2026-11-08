#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the non-uniform OpArrayLength reproduction.

Standalone: no shaped-core imports, no dev.py.
It assembles length.spvasm with the Vulkan SDK's spirv-as, builds repro.cc with clang-cl against the SDK's headers and loader
library, and runs the shader through a descriptor buffer and through a descriptor set, the second also with its first lane on the other element.

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
`--vendor` picks the device, `1002` for AMD; the first discrete GPU otherwise.

Exits 1 when an arm reads the right first words and a wrong length, 0 when every arm is right, 2 when the run is inconclusive.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
# (label, arguments, the right lengths, the right first words)
ARMS = [
    ("descriptor buffer", ["--buffer"], "2,8,2,8", "10,20,10,20"),
    ("descriptor set", ["--sets"], "2,8,2,8", "10,20,10,20"),
    ("descriptor set, the first lane on the long view", ["--sets", "--shift", "1"], "8,2,8,2", "20,10,20,10"),
]


def find_vulkan(given: str | None) -> Path | None:
    candidates = [given, os.environ.get("VULKAN_SDK")]
    tools = Path.home() / "tools" / "vulkan-sdk"
    if tools.is_dir():
        candidates += [str(p) for p in sorted(tools.iterdir(), reverse=True)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "include" / "vulkan" / "vulkan.h").is_file():
            return Path(candidate)
    return None


def tool(sdk: Path, name: str) -> Path:
    for sub in ("Bin", "bin"):
        for suffix in (".exe", ""):
            if (sdk / sub / (name + suffix)).is_file():
                return sdk / sub / (name + suffix)
    return Path(name)


def build(workdir: Path, sdk: Path) -> Path | None:
    spv = workdir / "length.spv"
    out = subprocess.run([str(tool(sdk, "spirv-as")), "--target-env", "vulkan1.3", str(HERE / "length.spvasm"), "-o", str(spv)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout, out.stderr, file=sys.stderr)
        return None
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
    workdir = Path(tempfile.gettempdir()) / "amd-vulkan-nonuniform-array-length-repro"
    workdir.mkdir(exist_ok=True)
    exe = build(workdir, sdk)
    if exe is None:
        return 2
    wrong, right = [], []
    for label, flags, right_lengths, right_firsts in ARMS:
        args = [str(exe), "--spirv", str(workdir / "length.spv"), *flags] + (["--vendor", options.vendor] if options.vendor else [])
        try:
            out = subprocess.run(args, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            print(f"{label}: timed out")
            continue
        print(f"{label}:")
        for line in out.stdout.strip().splitlines():
            print(f"    {line}")
        found = re.search(r"lengths=(\S+) firsts=(\S+)", out.stdout)
        if out.returncode != 0 or found is None:
            continue
        lengths, firsts = found.groups()
        if firsts != right_firsts:
            print(f"    the loads read {firsts}, not {right_firsts}: inconclusive for this arm")
        elif lengths == right_lengths:
            right.append(label)
        else:
            wrong.append(label)
    if wrong:
        print(f"REPRODUCED: OpArrayLength at a non-uniform index is wrong in: {'; '.join(wrong)}, "
              f"while the loads reach the right elements")
        return 1
    if len(right) == len(ARMS):
        print("not reproduced: every arm reads each lane's own length")
        return 0
    print("inconclusive: an arm did not complete")
    return 2


if __name__ == "__main__":
    sys.exit(main())
