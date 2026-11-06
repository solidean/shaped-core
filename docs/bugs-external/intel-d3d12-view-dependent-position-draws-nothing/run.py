#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the view-dependent position reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles the shaders with the newest Windows SDK's dxc.
Then every configuration runs on the high-performance adapter and on WARP.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when the hardware adapter draws nothing where WARP draws, 0 when they agree, 2 when the run is inconclusive.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent

# (name, vertex defines, pixel defines)
CONFIGURATIONS = [
    ("position by view, pixel stage ignores the view", ["-DMOVED"], []),
    ("position by view, pixel stage reads the view", ["-DMOVED"], ["-DREADS_VIEW"]),
    ("fixed position, pixel stage ignores the view", [], []),
    ("fixed position, pixel stage reads the view", [], ["-DREADS_VIEW"]),
    ("neither stage declares the view", ["-DNO_VIEW"], []),
]


def find_dxc() -> Path | None:
    """The newest Windows SDK's dxc, which signs DXIL through the dxil.dll beside it."""
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def compile_shader(dxc: Path, source: Path, profile: str, defines: list[str], output: Path) -> bool:
    result = subprocess.run([str(dxc), "-nologo", "-T", profile, "-E", "main", *defines, "-Fo", str(output), str(source)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode == 0


def draw(exe: Path, vs: Path, ps: Path, warp: bool) -> tuple[str, str]:
    """`drew`, `nothing` or the error the run stopped at, and the adapter's line."""
    try:
        result = subprocess.run([str(exe), str(vs), str(ps), *(["--warp"] if warp else [])], capture_output=True, text=True,
                                timeout=15)
    except subprocess.TimeoutExpired:
        return "hung", "adapter: unknown"
    lines = result.stdout.splitlines()
    adapter = next((line for line in lines if line.startswith("adapter:")), "adapter: unknown")
    if result.returncode not in (0, 1):
        return next((line for line in lines if line.startswith("error:")), f"exit {result.returncode}"), adapter
    return ("drew" if result.returncode == 0 else "nothing"), adapter


def main() -> int:
    if os.name != "nt":
        print("Windows only", file=sys.stderr)
        return 2
    dxc = find_dxc()
    if dxc is None or shutil.which("clang-cl") is None:
        print("needs clang-cl on PATH and a Windows SDK's dxc", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        exe = work / "repro.exe"
        built = subprocess.run(["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(HERE / "repro.cc"),
                                f"/Fe:{exe}", f"/Fo:{work}/"], capture_output=True, text=True, cwd=work)
        if built.returncode != 0:
            print(built.stdout + built.stderr, file=sys.stderr)
            return 2

        reproduced = False
        printed = False
        for name, vertex_defines, pixel_defines in CONFIGURATIONS:
            vs = work / "vs.dxil"
            ps = work / "ps.dxil"
            if not (compile_shader(dxc, HERE / "vertex.hlsl", "vs_6_1", vertex_defines, vs)
                    and compile_shader(dxc, HERE / "pixel.hlsl", "ps_6_1", pixel_defines, ps)):
                return 2
            hardware, adapter = draw(exe, vs, ps, warp=False)
            warp, warp_adapter = draw(exe, vs, ps, warp=True)
            if not printed:
                print(f"{adapter}\n{warp_adapter}\n")
                print(f"{'configuration':<50} {'hardware':<10} WARP")
                printed = True
            print(f"{name:<50} {hardware:<10} {warp}")
            reproduced = reproduced or (warp == "drew" and hardware != "drew")

    print("\nbug reproduced" if reproduced else "\nnot reproduced: the adapters agree")
    return 1 if reproduced else 0


if __name__ == "__main__":
    sys.exit(main())
