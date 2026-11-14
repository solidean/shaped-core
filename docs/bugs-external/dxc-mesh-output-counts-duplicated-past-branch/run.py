#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the duplicated SetMeshOutputCounts reproduction.

Standalone: no shaped-core imports, no dev.py.
It compiles shader.hlsl with every dxc it finds, once per row of the matrix, and reports whether DXC's own validator
refuses the result for calling SetMeshOutputCounts more than once.

    uv run run.py

Exits 1 when the failing row is refused by any dxc found, 0 when no dxc refuses it, 2 when no dxc was found.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SHADER = HERE / "shader.hlsl"
MESSAGE = "cannot be called multiple times"

# (row, dxc flags); the first row is the failing shader, every other changes one ingredient
ROWS = [
    ("as written, ms_6_6 -O3", ["-T", "ms_6_6", "-O3"]),
    ("ms_6_9 -O3", ["-T", "ms_6_9", "-O3"]),
    ("ms_6_6 -O1", ["-T", "ms_6_6", "-O1"]),
    ("ms_6_5 -O3", ["-T", "ms_6_5", "-O3"]),
    ("ms_6_6 -Od", ["-T", "ms_6_6", "-Od"]),
    ("no branch before the call", ["-T", "ms_6_6", "-O3", "-DFIRST_BRANCH=0"]),
    ("first branch on group.y", ["-T", "ms_6_6", "-O3", "-DFIRST_AXIS=y"]),
    ("first branch tests 5", ["-T", "ms_6_6", "-O3", "-DFIRST_VALUE=5"]),
    ("uint, not int", ["-T", "ms_6_6", "-O3", "-DSIGNED=0"]),
]


def find_dxcs() -> list[Path]:
    """`$DXC`, shaped-core's fetched dxc where this sits in the tree, the newest Windows SDK's, and the one on PATH."""
    found: list[Path] = []
    exe = "dxc.exe" if os.name == "nt" else "dxc"
    if os.environ.get("DXC"):
        found.append(Path(os.environ["DXC"]))
    fetched = HERE.parents[2] / "extern" / "dxc" / ".install" / "bin" / exe
    if fetched.exists():
        found.append(fetched)
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    if kits.exists():
        sdks = sorted((p for p in kits.glob("10.*") if (p / "x64" / "dxc.exe").exists()), key=lambda p: p.name)
        if sdks:
            found.append(sdks[-1] / "x64" / "dxc.exe")
    if on_path := shutil.which("dxc"):
        found.append(Path(on_path))
    unique: list[Path] = []
    for path in found:
        if path.exists() and path.resolve() not in [u.resolve() for u in unique]:
            unique.append(path)
    return unique


def version_of(dxc: Path) -> str:
    out = subprocess.run([str(dxc), "--version"], capture_output=True, text=True)
    text = (out.stdout + out.stderr).strip().splitlines()
    return text[0].split(" - ")[-1] if text else "unknown"


def refuses(dxc: Path, flags: list[str]) -> bool:
    null = "NUL" if os.name == "nt" else "/dev/null"
    out = subprocess.run([str(dxc), "-E", "main", *flags, str(SHADER), "-Fo", null], capture_output=True, text=True)
    return MESSAGE in out.stdout + out.stderr


def main() -> int:
    dxcs = find_dxcs()
    if not dxcs:
        print("no dxc found: set DXC, install a Windows SDK, or put dxc on PATH")
        return 2
    reproduced = False
    for dxc in dxcs:
        print(f"\n{dxc}  ({version_of(dxc)})\n")
        print(f"{'row':<28}  result")
        print(f"{'-' * 28}  {'-' * 34}")
        for i, (row, flags) in enumerate(ROWS):
            bad = refuses(dxc, flags)
            reproduced = reproduced or (i == 0 and bad)
            print(f"{row:<28}  {'refused: called multiple times' if bad else 'compiles'}")
    print("\nreproduced" if reproduced else "\nnot reproduced")
    return 1 if reproduced else 0


if __name__ == "__main__":
    sys.exit(main())
