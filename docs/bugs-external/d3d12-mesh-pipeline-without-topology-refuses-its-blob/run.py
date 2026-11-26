#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the mesh-pipeline blob reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shaders.hlsl's two stages with the newest Windows SDK's dxc.
repro.exe restores three pipelines from their own blobs on WARP and on every hardware adapter with mesh shaders.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when an adapter refuses the blob of a mesh stage stated without a topology and takes the other two, 0 when
every adapter takes all three, 2 when the run is inconclusive.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"
SHADERS = HERE / "shaders.hlsl"


def find_dxc() -> Path | None:
    """The newest Windows SDK's dxc, which signs DXIL through the dxil.dll beside it."""
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def build(workdir: Path) -> Path | None:
    """Compile repro.cc with clang-cl, which finds the Windows SDK and the MSVC libraries on its own."""
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(SOURCE), f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


def compile_stage(dxc: Path, profile: str, entry: str, out_path: Path) -> bool:
    cmd = [str(dxc), "-nologo", "-T", profile, "-E", entry, "-Fo", str(out_path), str(SHADERS)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def main() -> int:
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    dxc = find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found")
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        mesh, vertex = workdir / "ms.cso", workdir / "vs.cso"
        if not compile_stage(dxc, "ms_6_5", "ms", mesh) or not compile_stage(dxc, "vs_6_5", "vs", vertex):
            return 2
        try:
            out = subprocess.run([str(exe), str(mesh), str(vertex)], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            print("inconclusive: repro.exe timed out")
            return 2
        print(out.stdout.rstrip())
    if out.returncode == 1:
        print("REPRODUCED: a mesh pipeline stated without a topology refuses its own cached blob")
        return 1
    if out.returncode == 0:
        print("not reproduced: every adapter takes every blob")
        return 0
    print("inconclusive: no adapter with mesh shaders ran, or a control row was refused")
    return 2


if __name__ == "__main__":
    sys.exit(main())
