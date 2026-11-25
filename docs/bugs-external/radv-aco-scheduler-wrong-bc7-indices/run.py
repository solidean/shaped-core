#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the reproduction of RADV's LDS spills reordered by ACO's scheduler.

Standalone: no shaped-core imports, no dev.py.
It compiles shader.hlsl to SPIR-V with the Vulkan SDK's DXC, checks it with spirv-val, and builds repro.cc against the SDK.
Then it runs the shader twice, as the driver compiles it and with ACO's scheduler off (ACO_DEBUG=nosched), each without RADV's shader cache.

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>] [--prebuilt] [--ir <dir>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
Without an SDK, it builds against the system's Vulkan headers and loader and runs the committed `shader.spv`.
`--prebuilt` runs the committed `shader.spv` even with an SDK, so no DXC is needed.
`--vendor` picks the device, `1002` for AMD; the first GPU otherwise.
`--ir <dir>` also sets RADV_DEBUG=shaders and writes each arm's NIR, ACO IR and disassembly to `<dir>/aco-<arm>.txt`.

Exits 1 when threads are wrong as compiled and all right with the scheduler off, 0 when every thread is right, 2 when the run is inconclusive.
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
DXC_ARGS = ["-E", "main", "-T", "cs_6_8", "-O3", "-spirv", "-fspv-target-env=vulkan1.3"]
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


def tool(sdk: Path | None, name: str) -> Path:
    if sdk is None:
        return Path(name)
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


def build(workdir: Path, sdk: Path | None, prebuilt: bool) -> Path | None:
    if not prebuilt:
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
        cxx = shutil.which("clang++") or shutil.which("g++") or "c++"
        paths = [f"-I{sdk / 'include'}", f"-L{sdk / 'lib'}"] if sdk is not None else []
        cmd = [cxx, "-O2", "-std=c++20", *paths, str(HERE / "repro.cc"), "-o", str(exe), "-lvulkan"]
    if not run(cmd, cwd=workdir) or not exe.is_file():
        return None
    return exe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vulkan-sdk")
    parser.add_argument("--vendor", help="hex PCI vendor of the device, 1002 for AMD")
    parser.add_argument("--prebuilt", action="store_true", help="run the committed shader.spv, without DXC")
    parser.add_argument("--ir", help="directory to write each arm's RADV_DEBUG=shaders output into")
    options = parser.parse_args()
    sdk = find_vulkan(options.vulkan_sdk)
    prebuilt = options.prebuilt or sdk is None
    if sdk is None and sys.platform == "win32":
        print("no Vulkan SDK found; pass --vulkan-sdk <dir>")
        return 2
    if sdk is None:
        print("no Vulkan SDK found: building against the system's Vulkan, running the committed shader.spv")
    # one stable directory rather than a fresh temporary one, so reruns leave no litter
    workdir = Path(tempfile.gettempdir()) / "radv-aco-lds-spill-repro"
    workdir.mkdir(exist_ok=True)
    exe = build(workdir, sdk, prebuilt)
    if exe is None:
        return 2
    spv = HERE / "shader.spv" if prebuilt else workdir / "shader.spv"
    ir = Path(options.ir) if options.ir else None
    if ir is not None:
        ir.mkdir(parents=True, exist_ok=True)
    results = {}
    for label, extra in ARMS:
        env = {**os.environ, "RADV_DEBUG": "nocache,shaders" if ir is not None else "nocache", **extra}
        args = [str(exe), "--spirv", str(spv)] + (["--vendor", options.vendor] if options.vendor else [])
        try:
            out = subprocess.run(args, capture_output=True, text=True, timeout=300, env=env)
        except subprocess.TimeoutExpired:
            print(f"{label}: timed out")
            continue
        if ir is not None:
            name = "aco-nosched.txt" if extra else "aco-as-compiled.txt"
            (ir / name).write_text(out.stderr)
        print(f"{label}:")
        for line in out.stdout.strip().splitlines():
            print(f"    {line}")
        found = re.search(r"wrong=(\S+)", out.stdout)
        if out.returncode == 0 and found is not None:
            results[label] = found.group(1)
    if len(results) != len(ARMS):
        print("inconclusive: an arm did not complete")
        return 2
    wrong, wrong_off = results["as compiled"], results["ACO's scheduler off"]
    if wrong == "none":
        print("not reproduced: every thread matches the CPU")
        return 0
    if wrong_off == "none":
        print(f"REPRODUCED: threads {wrong} are wrong as compiled, and every thread is right with ACO's scheduler off")
        return 1
    print(f"inconclusive: threads {wrong} are wrong, and the scheduler-off arm is wrong too ({wrong_off})")
    return 2


if __name__ == "__main__":
    sys.exit(main())
