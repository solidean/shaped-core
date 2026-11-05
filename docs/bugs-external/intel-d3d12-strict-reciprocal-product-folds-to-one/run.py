#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the strict zero-times-infinity reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shader.hlsl with the newest Windows SDK's dxc, with -Gis and without.
Each shader runs once on the high-performance adapter and once on WARP.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when the hardware adapter returns a product that is no NaN from strict DXIL while WARP returns NaNs,
0 when every strict product is a NaN, 2 when the run is inconclusive.
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
SHADER = HERE / "shader.hlsl"

# (name, dxc flags); without -Gis DXC marks the products `fast`, which licenses the fold, so that row is no bug
FLAGS = [
    ("strict (-Gis)", ["-Gis"]),
    ("fast (no -Gis)", []),
]


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


def compile_shader(dxc: Path, flags: list[str], out_path: Path) -> bool:
    cmd = [str(dxc), "-nologo", "-T", "cs_6_6", "-E", "cs", "-O3", "-WX", *flags, "-Fo", str(out_path), str(SHADER)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def run(exe: Path, cso: Path, warp: bool) -> tuple[int, str]:
    try:
        out = subprocess.run([str(exe), str(cso), *(["--warp"] if warp else [])], capture_output=True, text=True,
                             timeout=60)
    except subprocess.TimeoutExpired:
        return 2, "timed out"
    return out.returncode, out.stdout.strip()


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
        results = {}
        for name, flags in FLAGS:
            cso = workdir / f"cs{'-gis' if flags else ''}.cso"
            if not compile_shader(dxc, flags, cso):
                return 2
            for warp in (False, True):
                code, line = run(exe, cso, warp)
                results[(name, warp)] = code
                print(f"{name:16} {'WARP' if warp else 'hardware':9} {'NaN everywhere' if code == 0 else 'NOT NaN' if code == 1 else 'failed'}: {line}")
    strict_hardware = results[("strict (-Gis)", False)]
    strict_warp = results[("strict (-Gis)", True)]
    if 2 in (strict_hardware, strict_warp):
        print("inconclusive: a strict run did not complete")
        return 2
    if strict_hardware == 1 and strict_warp == 0:
        print("REPRODUCED: the hardware driver folds a strict 0 * inf, which WARP keeps a NaN")
        return 1
    print("not reproduced: every strict product is a NaN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
