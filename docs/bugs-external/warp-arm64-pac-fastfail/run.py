#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the WARP-on-ARM64 fastfail reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl, or with cl from an MSVC developer environment, then runs one shader per process:
a pixel and a compute shader, each reading a 64-element buffer at four offsets, on the first non-software adapter and on WARP.
Windows only, since the bug is WARP's.

    uv run run.py

Exits 1 when a run dies with a fastfail (0xC0000409), 0 when every run is correct.
Exits 2 when the run is inconclusive: no compiler, a setup failure, or a wrong value rather than a crash.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"

# 0 and 32 read in bounds; 64 starts exactly at the end; 1 << 20 lands far past any page the buffer occupies.
OFFSETS = ("0", "32", "64", "1048576")
FASTFAIL = 0xC0000409


def build(workdir: Path) -> Path | None:
    """Compile repro.cc with clang-cl, or with cl when a developer environment put it on PATH."""
    exe = workdir / "repro.exe"
    compiler = shutil.which("clang-cl") or shutil.which("cl")
    if compiler is None:
        print("neither clang-cl nor cl is on PATH; run from an MSVC developer prompt", file=sys.stderr)
        return None
    cmd = [compiler, "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(SOURCE), f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


def run(exe: Path, *args: str) -> tuple[int, str]:
    out = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return out.returncode & 0xFFFFFFFF, out.stdout.strip()


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(Path(tmp))
        if exe is None:
            return 2
        print(run(exe, "adapters")[1])
        print()
        crashed = False
        inconclusive = False
        print(f"{'adapter':<20} {'stage':<6} {'offset':>8}  result")
        for adapter in ([], ["--warp"]):
            name = "WARP" if adapter else "first non-software"
            for stage in ("ps", "cs"):
                for offset in OFFSETS:
                    code, text = run(exe, stage, offset, *adapter)
                    if code == FASTFAIL:
                        verdict = "FASTFAIL (0xC0000409)"
                        crashed = True
                    elif code == 0:
                        verdict = "correct"
                    else:
                        verdict = f"exit 0x{code:08x}: {text.splitlines()[0] if text else ''}"
                        inconclusive = True
                    print(f"{name:<20} {stage:<6} {offset:>8}  {verdict}")
        if crashed:
            print("\nREPRODUCED: a bounds-checked read past a buffer's end fastfails")
            return 1
        if inconclusive:
            print("\ninconclusive: a run failed without the fastfail")
            return 2
        print("\nnot reproduced")
        return 0


if __name__ == "__main__":
    sys.exit(main())
