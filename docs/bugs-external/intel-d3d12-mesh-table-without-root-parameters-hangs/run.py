#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the mesh-table hang reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shader.hlsl's mesh stage with the newest Windows SDK's dxc, with the table
and without it.
Each draws under a root signature of no parameter, and the table's also under one of a constant no shader reads.
Every row runs on each hardware adapter whose name holds one of the vendor names below, and on WARP.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when an Intel adapter loses the device on the table under the empty root signature and keeps it on the other
two rows, 0 when it keeps it on all three, 2 when the run is inconclusive.
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
ADAPTERS = ["Intel", "AMD", "NVIDIA", "--warp"]
# (name, the table, a root constant)
ROWS = [
    ("table, no root parameter", 1, False),
    ("table, one root constant", 1, True),
    ("no table, no root parameter", 0, False),
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


def compile_mesh(dxc: Path, table: int, out_path: Path) -> bool:
    cmd = [str(dxc), "-nologo", "-T", "ms_6_5", "-E", "ms", "-O3", f"-DTABLE={table}", "-Fo", str(out_path), str(SHADER)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def run(exe: Path, mesh: Path, adapter: str, constant: bool) -> tuple[int, str]:
    args = [adapter] if adapter == "--warp" else ["--adapter", adapter]
    if constant:
        args.append("--root-constant")
    try:
        out = subprocess.run([str(exe), str(mesh), *args], capture_output=True, text=True, timeout=60)
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
    results: dict[tuple[str, str], int] = {}
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        for name, table, constant in ROWS:
            mesh = workdir / f"ms{table}.cso"
            if not compile_mesh(dxc, table, mesh):
                return 2
            print(name)
            for adapter in ADAPTERS:
                code, line = run(exe, mesh, adapter, constant)
                if code == 3:
                    continue
                results[(name, adapter)] = code
                verdict = {0: "kept", 1: "LOST"}.get(code, "failed")
                print(f"    {adapter.lstrip('-'):7} {verdict:6} {line}")
    hung = results.get((ROWS[0][0], "Intel"))
    others = [results.get((name, "Intel")) for name, _, _ in ROWS[1:]]
    if hung is None or any(o != 0 for o in others) or results.get((ROWS[0][0], "--warp")) != 0:
        print("inconclusive: no Intel adapter ran, it lost the device on a control row, or WARP lost it on the table")
        return 2
    if hung == 1:
        print("REPRODUCED: the Intel adapter loses the device on a mesh stage's table under a root signature of none")
        return 1
    if hung != 0:
        print("inconclusive: the table's run on the Intel adapter did not complete")
        return 2
    print("not reproduced: the Intel adapter keeps the device on every row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
