#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the NVIDIA mesh-stage groupshared atomics reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shader.hlsl with dxc, the newest Windows SDK's unless --dxc names one.
Four containers run on the first NVIDIA adapter: the mesh stage returning on the flag, the same at -Od, the mesh stage
returning on the condition spelled again, and the same body as a compute stage.
Windows only, since the harness is D3D12's.

    uv run run.py [--dxc <dxc.exe>] [--vendor <hex PCI id>]

Exits 1 when the mesh stage returning on the flag loses marks while the three controls keep every one, 0 when nothing
is lost, 2 when inconclusive.
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
SOURCE = HERE / "repro.cc"
SHADER = HERE / "shader.hlsl"

# label, stage, dxc flags; the first is the bug, the rest are controls
VARIANTS = (
    ("mesh, return on the flag", "ms", ["-D", "LATE_RETURN=1"]),
    ("mesh, return on the flag, -Od", "ms", ["-D", "LATE_RETURN=1", "-Od"]),
    ("mesh, return on k >= 4", "ms", ["-D", "LATE_RETURN=0"]),
    ("compute", "cs", []),
)


def find_dxc() -> Path | None:
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def build(workdir: Path) -> Path | None:
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(SOURCE), f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout, out.stderr, file=sys.stderr)
        return None
    return exe


def run(exe: Path, container: Path, stage: str, where: list[str]) -> tuple[str, str]:
    try:
        out = subprocess.run([str(exe), str(container), stage, *where], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    lines = out.stdout.strip().splitlines()
    adapter = lines[0].removeprefix("adapter: ") if lines else ""
    seen = next((l.removeprefix("seen: ") for l in lines if l.startswith("seen: ")), "")
    verdict = {0: "every mark", 1: "marks lost", 2: "no adapter or pipeline"}.get(out.returncode, f"crashed ({out.returncode:#x})")
    return f"{verdict} [{seen}]" if seen else verdict, adapter


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--vendor", default="10de")
    args = parser.parse_args()
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    dxc = args.dxc or find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found; pass --dxc")
        return 2
    results: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        print(f"dxc: {dxc}")
        for i, (label, stage, flags) in enumerate(VARIANTS):
            container = workdir / f"v{i}.dxil"
            out = subprocess.run([str(dxc), "-nologo", "-T", f"{stage}_6_5", "-E", stage, *flags, "-Fo", str(container),
                                  str(SHADER)], capture_output=True, text=True)
            if out.returncode != 0:
                print(out.stdout, out.stderr, file=sys.stderr)
                return 2
            verdict, adapter = run(exe, container, stage, ["--vendor", args.vendor, "--expect", "b"])
            results.append(verdict)
            print(f"{label:32} {verdict:28} {adapter}")
    if any(not v.startswith(("every", "marks lost")) for v in results):
        print("inconclusive: no such adapter, no pipeline, or a run did not finish")
        return 2
    bug, controls = results[0], results[1:]
    if bug.startswith("marks lost") and all(v.startswith("every") for v in controls):
        print("REPRODUCED: the mesh stage loses groupshared InterlockedOr results when a late return reads the flag")
        return 1
    if all(v.startswith("every") for v in results):
        print("not reproduced: every variant keeps every mark")
        return 0
    print("inconclusive: a control loses marks too")
    return 2

if __name__ == "__main__":
    sys.exit(main())
