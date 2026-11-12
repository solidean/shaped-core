#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the Agility acceleration-structure compile reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl against the Agility SDK's headers, and against the Vulkan SDK's where one is found.
Each arm is a fresh process timing its first acceleration-structure builds, since the cost is paid once per process.
Windows only, since the Agility SDK is D3D12's.

    uv run run.py [--agility <install>] [--vulkan-sdk <dir>]

`--agility` is a directory holding the Agility SDK's `include/` and `bin/` (its NuGet package's `build/native`).
It defaults to $AGILITY_SDK_DIR, then to the `extern/agility/.install` of the repository this entry lives in.
`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`; without one the vulkan arms are skipped.

Exits 1 when the first BLAS build costs seconds on the Agility runtime and milliseconds on the system runtime,
0 when both are fast, 2 when the run is inconclusive.
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
SOURCE = HERE / "repro.cc"

# a first build this slow is the compile; a healthy driver builds one triangle in milliseconds
SLOW_S = 5.0
FAST_S = 1.0

# (label, arguments, needs vulkan); AGILITY is replaced by --agility <bin>
# The driver caches the compile per application, so a freshly built exe misses once on every runtime:
# the first arm only warms that cache, and the verdict reads the second.
ARMS = [
    ("dx12, system runtime, warming the driver's cache", ["--dx12", "blas"], False),
    ("dx12, system runtime", ["--dx12", "blas", "--dx12", "tlas"], False),
    ("dx12, Agility runtime", ["AGILITY", "--dx12", "blas", "--dx12", "tlas"], False),
    ("vulkan alone", ["--vulkan", "blas", "--vulkan", "tlas"], True),
    ("vulkan after the Agility device factory, no dx12 device", ["AGILITY", "--dx12", "factory", "--vulkan", "blas", "--vulkan", "tlas"], True),
]


def find_agility(given: str | None) -> Path | None:
    for candidate in (given, os.environ.get("AGILITY_SDK_DIR"), str(HERE.parents[2] / "extern" / "agility" / ".install")):
        if candidate and (Path(candidate) / "bin" / "D3D12Core.dll").is_file():
            return Path(candidate)
    return None


def find_vulkan(given: str | None) -> Path | None:
    candidates = [given, os.environ.get("VULKAN_SDK")]
    tools = Path.home() / "tools" / "vulkan-sdk"
    if tools.is_dir():
        candidates += [str(p) for p in sorted(tools.iterdir(), reverse=True)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "include" / "vulkan" / "vulkan.h").is_file():
            return Path(candidate)
    return None


def build(workdir: Path, agility: Path, vulkan: Path | None) -> Path | None:
    """Compile repro.cc with clang-cl, which finds the Windows SDK and the MSVC libraries on its own."""
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    includes = [f"/I{agility / 'include'}"] + ([f"/I{vulkan / 'include'}"] if vulkan else [])
    cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", *includes, str(SOURCE), f"/Fe:{exe}",
           f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


def run(exe: Path, args: list[str]) -> dict[str, float] | None:
    """One fresh process; the phase timings it printed, or None where it failed."""
    try:
        out = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print("    timed out")
        return None
    print(f"    {out.stdout.strip()}")
    if out.returncode != 0:
        return None
    return {k: float(v) for k, v in re.findall(r"(\S+)=([0-9.]+)", out.stdout)}


def first_build(times: dict[str, float], api: str) -> float:
    """What the process's first BLAS cost, from its prebuild query to its fence."""
    return sum(v for k, v in times.items() if k.startswith(f"{api}.blas."))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agility")
    parser.add_argument("--vulkan-sdk")
    parser.add_argument("--vendor", help="hex PCI vendor of the adapter to run on, 1002 for AMD; the high-performance one otherwise")
    options = parser.parse_args()
    if sys.platform != "win32":
        print("the Agility SDK runs on Windows alone")
        return 2
    agility = find_agility(options.agility)
    if agility is None:
        print("no Agility SDK found; pass --agility <dir holding include/ and bin/>")
        return 2
    vulkan = find_vulkan(options.vulkan_sdk)
    if vulkan is None:
        print("no Vulkan SDK headers found; the vulkan arms are skipped")
    # one stable directory rather than a fresh temporary one, so reruns leave no litter
    workdir = Path(tempfile.gettempdir()) / "intel-agility-acceleration-structure-repro"
    workdir.mkdir(exist_ok=True)
    exe = build(workdir, agility, vulkan)
    if exe is None:
        return 2
    results = {}
    for label, args, needs_vulkan in ARMS:
        if needs_vulkan and vulkan is None:
            continue
        print(f"{label}:")
        args = [a for arg in args for a in (["--agility", str(agility / "bin") + os.sep] if arg == "AGILITY" else [arg])]
        results[label] = run(exe, (["--vendor", options.vendor] if options.vendor else []) + args)
    system = results.get("dx12, system runtime")
    agile = results.get("dx12, Agility runtime")
    if system is None or agile is None:
        print("inconclusive: a dx12 arm did not complete")
        return 2
    system_s, agile_s = first_build(system, "dx12"), first_build(agile, "dx12")
    print(f"first BLAS: {system_s:.3f} s on the system runtime, {agile_s:.3f} s on the Agility runtime")
    if agile_s > SLOW_S and system_s < FAST_S:
        print("REPRODUCED: a process on the Agility runtime compiles for seconds at its first acceleration-structure build")
        return 1
    if agile_s < FAST_S:
        print("not reproduced: the first build is fast on both runtimes")
        return 0
    print("inconclusive: neither clearly fast nor clearly slow")
    return 2


if __name__ == "__main__":
    sys.exit(main())
