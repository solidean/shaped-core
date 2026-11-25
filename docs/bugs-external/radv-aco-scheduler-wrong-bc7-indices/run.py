#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the RADV ACO scheduler BC7 reproduction.

Standalone: no shaped-core imports, no dev.py.
It compiles shader.hlsl to SPIR-V with the Vulkan SDK's DXC, as shaped-core does, checks it with spirv-val, and builds repro.cc against the SDK.
Then it runs the shader twice, as the driver compiles it and with ACO's scheduler off (ACO_DEBUG=nosched), each without RADV's shader cache.

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
`--vendor` picks the device, `1002` for AMD; the first GPU otherwise.

Exits 1 when blocks decode wrong as compiled and right with the scheduler off, 0 when every block decodes right, 2 when the run is inconclusive.
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
# shaped-core's DXC arguments for a vulkan compute shader, less the flags that change nothing here
DXC_ARGS = ["-E", "bc7_cs", "-T", "cs_6_8", "-O3", "-spirv", "-fspv-target-env=vulkan1.3"]
ARMS = [
    ("as compiled", {}),
    ("ACO's scheduler off", {"ACO_DEBUG": "nosched"}),
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


def run(cmd: list[str], **kwargs) -> bool:
    out = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if out.returncode != 0:
        print(out.stdout, out.stderr, file=sys.stderr)
    return out.returncode == 0


def build(workdir: Path, sdk: Path) -> Path | None:
    spv = workdir / "shader.spv"
    if not run([str(tool(sdk, "dxc")), *DXC_ARGS, str(HERE / "shader.hlsl"), "-Fo", str(spv)]):
        return None
    if not run([str(tool(sdk, "spirv-val")), "--target-env", "vulkan1.3", str(spv)]):
        print("spirv-val rejects DXC's module: inconclusive", file=sys.stderr)
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
    if not run(cmd, cwd=workdir) or not exe.is_file():
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
    workdir = Path(tempfile.gettempdir()) / "radv-aco-scheduler-bc7-repro"
    workdir.mkdir(exist_ok=True)
    exe = build(workdir, sdk)
    if exe is None:
        return 2
    results = {}
    for label, extra in ARMS:
        env = {**os.environ, "RADV_DEBUG": "nocache", **extra}
        args = [str(exe), "--spirv", str(workdir / "shader.spv")] + (["--vendor", options.vendor] if options.vendor else [])
        try:
            out = subprocess.run(args, capture_output=True, text=True, timeout=300, env=env)
        except subprocess.TimeoutExpired:
            print(f"{label}: timed out")
            continue
        print(f"{label}:")
        for line in out.stdout.strip().splitlines():
            print(f"    {line}")
        found = re.search(r"bad=(\S+) other=(\S+)", out.stdout)
        if out.returncode == 0 and found is not None:
            results[label] = found.groups()
    if len(results) != len(ARMS):
        print("inconclusive: an arm did not complete")
        return 2
    (bad, other), (bad_off, other_off) = results["as compiled"], results["ACO's scheduler off"]
    if bad == "none":
        print("not reproduced: every block decodes to its source")
        return 0
    if bad_off == "none" and other_off == "none":
        print(f"REPRODUCED: blocks {bad} decode wrong as compiled, and every block is right with ACO's scheduler off")
        return 1
    print(f"inconclusive: blocks {bad} decode wrong, and the scheduler-off arm is wrong too (bad={bad_off} other={other_off})")
    return 2


if __name__ == "__main__":
    sys.exit(main())
