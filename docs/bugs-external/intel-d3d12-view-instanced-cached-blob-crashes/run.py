#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the view-instanced cached-blob reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shader.hlsl with the newest Windows SDK's dxc.
Then each configuration creates its pipeline fresh in one process and saves the blob, and restores it in a second.
Every adapter whose name holds one of the vendors below is tried, and WARP.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when a restore crashes, 0 when every restore returns, 2 when the run is inconclusive.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
RUN_TIMEOUT_SECONDS = 60
ATTEMPTS = 3
ADAPTERS = ["intel", "amd", "nvidia", "warp"]

# (name, repro.exe flags, in one process); the first is the pipeline shaped-core builds for two views
CONFIGURATIONS = [
    ("2 views", ["--views", "2"], False),
    ("1 view, through the view-instancing subobject", ["--views", "1"], False),
    ("4 views", ["--views", "4"], False),
    ("2 views, the debug layer on", ["--views", "2", "--debug"], False),
    ("no view-instancing subobject", ["--no-view-instancing"], False),
    ("2 views, restored in the process that saved", ["--views", "2"], True),
    ("as above, the view locations moved and overwritten", ["--views", "2", "--scribble"], True),
]


def find_dxc() -> Path | None:
    """The newest Windows SDK's dxc, which signs DXIL through the dxil.dll beside it."""
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def compile_shader(dxc: Path, entry: str, profile: str, output: Path) -> bool:
    result = subprocess.run([str(dxc), "-nologo", "-T", profile, "-E", entry, "-Fo", str(output), str(HERE / "shader.hlsl")],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode == 0


def launch(command: list[str]) -> tuple[int | None, list[str]]:
    """The exit code, None for a hang, and the lines printed."""
    try:
        done = subprocess.run(command, capture_output=True, timeout=RUN_TIMEOUT_SECONDS)
        return done.returncode, done.stdout.decode(errors="replace").splitlines()
    except subprocess.TimeoutExpired as hang:
        return None, (hang.stdout or b"").decode(errors="replace").splitlines()


def restore(exe: Path, vs: Path, ps: Path, blob: Path, adapter: str, flags: list[str], in_one: bool) -> tuple[str, list[str]]:
    """`restored`, `refused`, `crashed`, `hung`, or what stopped the run before the restore."""
    common = [str(exe), str(vs), str(ps), "--adapter", adapter, *flags]
    if not in_one:
        code, lines = launch([*common, "--save", str(blob)])
        if code != 0:
            return next((line for line in lines if line.startswith("error:")), f"save exited {code}"), lines
    code, lines = launch([*common, "--both"] if in_one else [*common, "--load", str(blob)])
    if code is None:
        return "hung", lines
    if code == 0:
        return "restored", lines
    if code == 3:
        return "refused", lines
    error = next((line for line in lines if line.startswith("error:")), None)
    if code == 2 and error is not None:
        return error, lines
    return f"crashed (0x{code & 0xFFFFFFFF:08x})", lines


def main() -> int:
    if os.name != "nt":
        print("Windows only", file=sys.stderr)
        return 2
    dxc = find_dxc()
    if dxc is None or shutil.which("clang-cl") is None:
        print("needs clang-cl on PATH and a Windows SDK's dxc", file=sys.stderr)
        return 2

    reproduced = False
    inconclusive = False
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        exe = work / "repro.exe"
        built = subprocess.run(["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(HERE / "repro.cc"),
                                f"/Fe:{exe}", f"/Fo:{work}/"], capture_output=True, text=True, cwd=work)
        if built.returncode != 0:
            print(built.stdout + built.stderr, file=sys.stderr)
            return 2
        vs = work / "vs.dxil"
        ps = work / "ps.dxil"
        if not (compile_shader(dxc, "main_vs", "vs_6_1", vs) and compile_shader(dxc, "main_ps", "ps_6_1", ps)):
            return 2

        for adapter in ADAPTERS:
            code, lines = launch([str(exe), str(vs), str(ps), "--adapter", adapter, "--save", str(work / "probe.bin")])
            if code != 0:
                continue
            print(next(line for line in lines if line.startswith("adapter:")))
            for name, flags, in_one in CONFIGURATIONS:
                outcomes = [restore(exe, vs, ps, work / "blob.bin", adapter, flags, in_one) for _ in range(ATTEMPTS)]
                results = [result for result, _ in outcomes]
                print(f"  {name:<52} {', '.join(results)}", flush=True)
                for _, lines in outcomes:
                    for line in lines:
                        if line.startswith("debug layer:"):
                            print(f"      {line[:150]}")
                reproduced = reproduced or any(r.startswith("crashed") for r in results)
                inconclusive = inconclusive or any(r.startswith("error:") or r.startswith("save") for r in results)
            print()

    if reproduced:
        print("bug reproduced: a restore from a blob crashed")
        return 1
    if inconclusive:
        print("inconclusive: a run stopped before its restore")
        return 2
    print("not reproduced: every restore returned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
