#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the NVIDIA payload-qualifier reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl against the Agility SDK's headers, and compiles shader.hlsl at run time through DXC's
dxcompiler.dll, which must know Shader Model 6.9.
Windows only, since the Agility SDK is D3D12's.

    uv run run.py [--agility <install>] [--dxc-dir <dir of dxcompiler.dll>] [--vendor <hex PCI id>]

`--agility` is a directory holding the Agility SDK's `include/` and `bin/` (its NuGet package's `build/native`).
It defaults to $AGILITY_SDK_DIR, then to the `extern/agility/.install` of the repository this entry lives in.
`--dxc-dir` defaults to $DXC_DIR, then to that repository's `extern/dxc/.install/bin`.

Exits 1 when a pipeline loses its closest hit's payload write beside a twin of the other qualifier setting on one
device, while every control keeps it; 0 when nothing is lost; 2 when the run is inconclusive.
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

# (label, arguments, whether the bug shows in it)
ARMS = [
    ("qualified, then unqualified", ["--qualifiers", "mixed"], True),
    ("unqualified, then qualified", ["--qualifiers", "reversed"], True),
    ("both qualified", ["--qualifiers", "both"], False),
    ("neither qualified", ["--qualifiers", "neither"], False),
    ("mixed, a device each", ["--qualifiers", "mixed", "--separate-devices"], False),
    ("mixed, no reordering", ["--qualifiers", "mixed", "-D", "NO_REORDER"], False),
]


def find_agility(given: str | None) -> Path | None:
    for candidate in (given, os.environ.get("AGILITY_SDK_DIR"), str(HERE.parents[2] / "extern" / "agility" / ".install")):
        if candidate and (Path(candidate) / "bin" / "D3D12Core.dll").is_file():
            return Path(candidate)
    return None


def find_dxc(given: str | None) -> Path | None:
    for candidate in (given, os.environ.get("DXC_DIR"), str(HERE.parents[2] / "extern" / "dxc" / ".install" / "bin")):
        if candidate and (Path(candidate) / "dxcompiler.dll").is_file():
            return Path(candidate)
    return None


def build(workdir: Path, agility: Path) -> Path | None:
    """Compile repro.cc with clang-cl, which finds the Windows SDK and the MSVC libraries on its own."""
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", f"/I{agility / 'include'}", str(SOURCE),
           f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agility")
    parser.add_argument("--dxc-dir")
    parser.add_argument("--vendor", default="10de", help="hex PCI vendor of the adapter to run on; NVIDIA's by default")
    options = parser.parse_args()
    if sys.platform != "win32":
        print("the Agility SDK runs on Windows alone")
        return 2
    agility = find_agility(options.agility)
    if agility is None:
        print("no Agility SDK found; pass --agility <dir holding include/ and bin/>")
        return 2
    dxc = find_dxc(options.dxc_dir)
    if dxc is None:
        print("no dxcompiler.dll found; pass --dxc-dir")
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(Path(tmp), agility)
        if exe is None:
            return 2
        codes = {}
        for label, args, _ in ARMS:
            cmd = [str(exe), "--agility", str(agility / "bin") + os.sep, "--dxc", str(dxc), "--shader", str(SHADER),
                   "--vendor", options.vendor, *args]
            try:
                out = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            except subprocess.TimeoutExpired:
                print(f"{label}: timed out")
                return 2
            lines = out.stdout.strip().splitlines()
            second = next((l.split(":", 1)[1].strip() for l in lines if l.startswith("second")), lines[-1] if lines else "")
            codes[label] = out.returncode
            print(f"{label:30} exit {out.returncode}  second pipeline: {second[:40]}")
            if out.returncode == 2:
                print("inconclusive: an arm failed to set up:", "\n".join(lines[-3:]))
                return 2
    bug_arms = [label for label, _, shows in ARMS if shows]
    control_arms = [label for label, _, shows in ARMS if not shows]
    if all(codes[a] == 1 for a in bug_arms) and all(codes[a] == 0 for a in control_arms):
        print("REPRODUCED: beside a pipeline of the other qualifier setting on one device, a reordering raygen loses "
              "its closest hit's payload write")
        return 1
    if all(code == 0 for code in codes.values()):
        print("not reproduced: every pipeline keeps the payload")
        return 0
    print("inconclusive: the arms disagree with both readings")
    return 2


if __name__ == "__main__":
    sys.exit(main())
