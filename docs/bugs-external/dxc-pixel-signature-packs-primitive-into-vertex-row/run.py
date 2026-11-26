#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the per-primitive row-sharing reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shaders.hlsl with the newest Windows SDK's dxc, once with the vertex
attribute nointerpolation (SHARED=1) and once linear (SHARED=0).
Each pair runs on every hardware adapter whose name holds one of the vendor names below, and on WARP.
It also prints the pixel stage's input signature, where SHARED=1 shows WEIGHT in ID's row.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when DXC shares the row and an AMD adapter draws it wrong while it draws the linear one right,
0 when it draws both right, 2 when the run is inconclusive.
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
ADAPTERS = ["AMD", "Intel", "NVIDIA", "--warp"]


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


def compile_stage(dxc: Path, profile: str, entry: str, shared: int, out_path: Path, listing: Path | None = None) -> bool:
    cmd = [str(dxc), "-nologo", "-T", profile, "-E", entry, "-O3", f"-DSHARED={shared}", "-Fo", str(out_path), str(SHADERS)]
    if listing is not None:
        cmd += ["-Fc", str(listing)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def input_rows(listing: Path) -> str:
    """The pixel stage's user inputs as `NAME row mask`, from the listing's input signature."""
    rows = []
    lines = listing.read_text().splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith("; Input signature"))
    for line in lines[start + 4 :]:
        parts = line[1:].split()
        if len(parts) < 6:
            break
        if not parts[0].startswith("SV_"):
            rows.append(f"{parts[0]} row {parts[3]} {parts[2]}")
    return ", ".join(rows)


def run(exe: Path, mesh: Path, pixel: Path, adapter: str) -> tuple[int, str]:
    args = [adapter] if adapter == "--warp" else ["--adapter", adapter]
    try:
        out = subprocess.run([str(exe), str(mesh), str(pixel), *args], capture_output=True, text=True, timeout=60)
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
    results: dict[tuple[int, str], int] = {}
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        for shared in (1, 0):
            mesh = workdir / f"ms{shared}.cso"
            pixel = workdir / f"ps{shared}.cso"
            listing = workdir / f"ps{shared}.txt"
            if not compile_stage(dxc, "ms_6_5", "ms", shared, mesh) or not compile_stage(dxc, "ps_6_5", "ps", shared, pixel, listing):
                return 2
            print(f"SHARED={shared}, pixel inputs: {input_rows(listing)}")
            for adapter in ADAPTERS:
                code, line = run(exe, mesh, pixel, adapter)
                if code == 3:
                    continue
                results[(shared, adapter)] = code
                print(f"    {adapter.lstrip('-'):7} {({0: 'right', 1: 'WRONG'}).get(code, 'failed'):6} {line}")
    shared_amd = results.get((1, "AMD"))
    linear_amd = results.get((0, "AMD"))
    if shared_amd not in (0, 1) or linear_amd != 0 or results.get((1, "--warp")) != 0:
        print("inconclusive: no AMD adapter ran, it drew the linear pair wrong, or WARP drew the shared pair wrong")
        return 2
    if shared_amd == 1:
        print("REPRODUCED: DXC packs a primitive input into a vertex input's row, and the AMD adapter draws it wrong")
        return 1
    print("not reproduced: the AMD adapter draws both pairs right")
    return 0


if __name__ == "__main__":
    sys.exit(main())
