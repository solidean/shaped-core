#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the mesh-pipeline restore reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shaders.hlsl's stages with the newest Windows SDK's dxc.
Per adapter, one process builds the pipeline fresh and saves its blob, and a second process runs each row's steps.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when the Intel adapter draws nothing for a mesh pipeline built fresh after a restore and draws every other row,
0 when it draws every row, 2 when the run is inconclusive.
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
ADAPTERS = ["Intel", "AMD", "NVIDIA", "Basic Render"]
# (name, the first stage, the steps of the second process)
ROWS = [
    ("mesh: restored, then built fresh", "ms", "rf"),
    ("mesh: built fresh, then restored", "ms", "fr"),
    ("vertex: restored, then built fresh", "vs", "rf"),
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


def compile_stage(dxc: Path, profile: str, entry: str, out_path: Path) -> bool:
    out = subprocess.run([str(dxc), "-nologo", "-T", profile, "-E", entry, "-Fo", str(out_path), str(SHADERS)], capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def run(exe: Path, args: list[str]) -> tuple[int, str]:
    try:
        out = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 2, "timed out"
    return out.returncode, " | ".join(out.stdout.strip().splitlines())


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
        stages = {name: workdir / f"{name}.cso" for name in ("ms", "vs", "ps")}
        for name, profile in (("ms", "ms_6_5"), ("vs", "vs_6_5"), ("ps", "ps_6_5")):
            if not compile_stage(dxc, profile, name, stages[name]):
                return 2
        for name, first, steps in ROWS:
            print(name)
            for adapter in ADAPTERS:
                blob = workdir / f"{first}-{adapter.replace(' ', '')}.bin"
                extra = ["--vertex"] if first == "vs" else []
                # the blob comes from a process of its own, which is what the bug needs
                code, line = run(exe, [str(stages[first]), str(stages["ps"]), str(blob), "fs", adapter, *extra])
                if code == 3:
                    continue
                if code != 0:
                    print(f"    {adapter:12} could not save a blob: {line}")
                    continue
                code, line = run(exe, [str(stages[first]), str(stages["ps"]), str(blob), steps, adapter, *extra])
                results[(name, adapter)] = code
                print(f"    {adapter:12} {line}")
    bug = results.get((ROWS[0][0], "Intel"))
    others = [results.get((name, "Intel")) for name, _, _ in ROWS[1:]]
    if bug is None or any(o != 0 for o in others):
        print("inconclusive: no Intel adapter ran, or it drew nothing on a control row")
        return 2
    if bug == 1:
        print("REPRODUCED: the Intel adapter draws nothing for a mesh pipeline built fresh after one was restored")
        return 1
    if bug != 0:
        print("inconclusive: the mesh row did not complete on the Intel adapter")
        return 2
    print("not reproduced: the Intel adapter draws every row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
