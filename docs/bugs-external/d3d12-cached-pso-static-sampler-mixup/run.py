#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the cached-PSO static-sampler reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl, stores a clamping pipeline's blob in one process, then restores it in two more.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when a restored pipeline samples through the wrong static sampler, 0 when every run samples as it should.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"


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


def run(exe: Path, *args: str) -> int:
    out = subprocess.run([str(exe), *args], capture_output=True, text=True)
    shown = " ".join(a for a in args if not a.endswith(".blob"))
    print(f"    {shown:<22} {out.stdout.strip()}")
    return out.returncode


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        reproduced = False
        for adapter in ([], ["--warp"]):
            print(f"on {'WARP' if adapter else 'the high-performance adapter'}:")
            blob = str(workdir / f"clamp{'-warp' if adapter else ''}.blob")
            # Each step is its own process: the fault needs the blob to meet a process that never built its pipeline.
            stored = run(exe, "store", blob, *adapter)
            alone = run(exe, "restore", blob, *adapter)
            same = run(exe, "restore", blob, "same", *adapter)
            after_twin = run(exe, "restore", blob, "twin", *adapter)
            if stored != 0 or alone != 0 or same != 0:
                print("  inconclusive: a run that should sample correctly did not")
                return 2
            if after_twin != 0:
                print("  REPRODUCED: a correct blob, restored after its repeating twin was built, samples by repeat")
                reproduced = True
            else:
                print("  not reproduced")
    return 1 if reproduced else 0


if __name__ == "__main__":
    sys.exit(main())
