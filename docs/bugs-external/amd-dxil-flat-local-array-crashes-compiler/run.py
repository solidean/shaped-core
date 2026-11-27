#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the AMD flat-local-array compiler crash reproduction.

Standalone: no shaped-core imports, no dev.py.
It compiles flat.hlsl, the variant, and split.hlsl, the control, with dxc: the newest Windows SDK's unless --dxc names one.
Each container then goes through AMD's compiler one of two ways.
Offline by default, through Radeon GPU Analyzer's bundled compiler for --chip (gfx1200 unless named), no AMD GPU needed.
--rga names rga.exe, which is otherwise looked for on PATH and under C:\\tools.
With --hardware, repro.cc is built with clang-cl and each container becomes a pipeline on the first AMD adapter, a process each.
Windows only, since the bug is D3D12's.

    uv run run.py [--dxc <dxc.exe>] [--rga <rga.exe>] [--chip <gfxNNNN>] [--hardware]

Exits 1 when the variant crashes the compiler and the control compiles, 0 when both compile, 2 when inconclusive.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent


def find_dxc() -> Path | None:
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def find_rga() -> Path | None:
    on_path = shutil.which("rga")
    if on_path:
        return Path(on_path)
    found = sorted(Path(r"C:\tools").glob("rga*/rga.exe"))
    return found[-1] if found else None


def compile_hlsl(dxc: Path, source: Path, out: Path) -> bool:
    run = subprocess.run([str(dxc), "-nologo", "-T", "cs_6_0", "-E", "main", "-Fo", str(out), str(source)],
                         capture_output=True, text=True)
    if run.returncode != 0:
        print(run.stdout, run.stderr, file=sys.stderr)
    return run.returncode == 0


def offline(rga: Path, chip: str, container: Path, workdir: Path) -> str:
    # RGA writes a log of the compiler it substitutes into its working directory, so it runs in the scratch one
    try:
        run = subprocess.run([str(rga), "-s", "dx12", "--offline", "-c", chip, "--cs-blob", str(container),
                              "--isa", str(workdir / (container.stem + ".isa.txt"))],
                             capture_output=True, text=True, timeout=120, cwd=workdir)
    except subprocess.TimeoutExpired:
        return "timeout"
    if "extracted successfully" in run.stdout:
        return "compiled"
    if f"failed to extract pipeline binary for {chip}" in run.stdout:
        return "crashed"
    if "no supported target detected" in run.stdout:
        return f"inconclusive: this RGA compiles no dx12 pipeline for {chip}"
    return "inconclusive: " + (run.stdout + run.stderr).strip()[-200:]


def build(workdir: Path) -> Path | None:
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = ["clang-cl", "/O2", "/EHsc", "/MD", "/nologo", str(HERE / "repro.cc"), f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    run = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if run.returncode != 0 or not exe.is_file():
        print(run.stdout, run.stderr, file=sys.stderr)
        return None
    return exe


def on_hardware(exe: Path, container: Path) -> str:
    try:
        run = subprocess.run([str(exe), str(container)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "timeout"
    if run.returncode == 0:
        return "compiled"
    if run.returncode == 4:
        return "no adapter"
    if run.returncode == 1:
        return "refused: " + run.stdout.strip().splitlines()[-1]
    return f"crashed (exit 0x{run.returncode & 0xffffffff:08x})"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--rga", type=Path)
    parser.add_argument("--chip", default="gfx1200")
    parser.add_argument("--hardware", action="store_true")
    args = parser.parse_args()
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    dxc = args.dxc or find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found; pass --dxc")
        return 2
    rga = None
    if not args.hardware:
        rga = args.rga or find_rga()
        if rga is None:
            print("no rga.exe found; pass --rga, or --hardware on an AMD GPU")
            return 2

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir) if args.hardware else None
        if args.hardware and exe is None:
            return 2
        results = {}
        for label, source in (("split (DXC's sixteen [8 x float])", "split.hlsl"), ("flat (one [128 x float])", "flat.hlsl")):
            container = workdir / (Path(source).stem + ".dxil")
            if not compile_hlsl(dxc, HERE / source, container):
                return 2
            verdict = on_hardware(exe, container) if exe else offline(rga, args.chip, container, workdir)
            results[source] = verdict
            print(f"{label:36} {verdict}")
    where = "the adapter's driver" if args.hardware else f"RGA's compiler for {args.chip}"
    if results["split.hlsl"] == "compiled" and results["flat.hlsl"].startswith("crashed"):
        print(f"REPRODUCED: {where} crashes on the flat array and compiles the split one")
        return 1
    if all(v == "compiled" for v in results.values()):
        print(f"not reproduced: {where} compiles both")
        return 0
    print("inconclusive")
    return 2


if __name__ == "__main__":
    sys.exit(main())
