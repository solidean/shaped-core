#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the view instancing reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles the shaders with the newest Windows SDK's dxc.
Then each configuration runs on WARP once and on the high-performance adapter three times.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when the hardware adapter does not draw where WARP does, 0 when they agree, 2 when the run is inconclusive.
A debug-layer complaint on any run makes it inconclusive, since undefined input to a driver proves nothing about it.
"""

from __future__ import annotations

import collections
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
HARDWARE_RUNS = 3
# A run that wrote its result can still be slow to exit after a driver failure, so a timeout keeps what it printed.
RUN_TIMEOUT_SECONDS = 6

POSITION = "-DPOSITION_BY_VIEW"
VARYING = "-DVARYING"
BY_VIEW = "-DVARYING_BY_VIEW"
BY_VERTEX = "-DVARYING_BY_VERTEX"
READS = "-DVIEW_INPUT"
DECLARES = ["-DVIEW_INPUT", "-DVIEW_UNREAD"]

# (name, vertex defines, pixel defines, views)
CONFIGURATIONS = [
    ("nothing varies", [], [], 2),
    ("nothing varies, 1 view", [], [], 1),
    ("a constant varying", [VARYING], [VARYING], 2),
    ("a varying by vertex", [VARYING, BY_VERTEX], [VARYING], 2),
    ("a varying by vertex, pixel ignores it", [VARYING, BY_VERTEX], [], 2),
    ("a varying by view", [VARYING, BY_VIEW], [VARYING], 2),
    ("pixel reads the view", [], [READS], 2),
    ("position by view", [POSITION], [], 2),
    ("position by view, 1 view", [POSITION], [], 1),
    ("position by view, a varying by vertex", [POSITION, VARYING, BY_VERTEX], [VARYING], 2),
    ("position by view, a varying by view", [POSITION, VARYING, BY_VIEW], [VARYING], 2),
    ("position by view, pixel declares the view", [POSITION], DECLARES, 2),
    ("position by view, pixel reads the view", [POSITION], [READS], 2),
]


def find_dxc() -> Path | None:
    """The newest Windows SDK's dxc, which signs DXIL through the dxil.dll beside it."""
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def compile_shader(dxc: Path, source: Path, profile: str, defines: list[str], output: Path) -> bool:
    result = subprocess.run([str(dxc), "-nologo", "-T", profile, "-E", "main", *defines, "-Fo", str(output), str(source)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode == 0


def draw(exe: Path, vs: Path, ps: Path, views: int, warp: bool) -> tuple[str, str, list[str]]:
    """`drew`, `nothing` or what stopped the run; the adapter's line; and what the debug layer said."""
    command = [str(exe), str(vs), str(ps), "--views", str(views), *(["--warp"] if warp else [])]
    try:
        output = subprocess.run(command, capture_output=True, timeout=RUN_TIMEOUT_SECONDS).stdout
    except subprocess.TimeoutExpired as hang:
        output = hang.stdout or b""
    lines = output.decode(errors="replace").splitlines()
    adapter = next((line for line in lines if line.startswith("adapter:")), "adapter: unknown")
    complaints = [line for line in lines if line.startswith("debug layer:") and "DEVICE_REMOVAL" not in line and "RemoveDevice" not in line]
    for line in lines:
        if line.startswith("written:"):
            return ("nothing" if line.split()[1:] == ["0", "0"] else "drew"), adapter, complaints
        if line.startswith("error:"):
            return line[7:], adapter, complaints
    return "did not finish", adapter, complaints


def main() -> int:
    if os.name != "nt":
        print("Windows only", file=sys.stderr)
        return 2
    dxc = find_dxc()
    if dxc is None or shutil.which("clang-cl") is None:
        print("needs clang-cl on PATH and a Windows SDK's dxc", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        exe = work / "repro.exe"
        built = subprocess.run(["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(HERE / "repro.cc"),
                                f"/Fe:{exe}", f"/Fo:{work}/"], capture_output=True, text=True, cwd=work)
        if built.returncode != 0:
            print(built.stdout + built.stderr, file=sys.stderr)
            return 2

        reproduced = False
        complained = False
        printed = False
        for name, vertex_defines, pixel_defines, views in CONFIGURATIONS:
            vs = work / "vs.dxil"
            ps = work / "ps.dxil"
            if not (compile_shader(dxc, HERE / "vertex.hlsl", "vs_6_1", vertex_defines, vs)
                    and compile_shader(dxc, HERE / "pixel.hlsl", "ps_6_1", pixel_defines, ps)):
                return 2
            warp, warp_adapter, warp_complaints = draw(exe, vs, ps, views, warp=True)
            runs = [draw(exe, vs, ps, views, warp=False) for _ in range(HARDWARE_RUNS)]
            if not printed:
                print(f"{runs[0][1]}\n{warp_adapter}\n")
                print(f"{'configuration':<44} {'WARP':<9} hardware")
                printed = True
            counts = collections.Counter(result for result, _, _ in runs)
            print(f"{name:<44} {warp:<9} {', '.join(f'{n}x {what}' for what, n in counts.most_common())}", flush=True)
            reproduced = reproduced or (warp == "drew" and any(result != "drew" for result, _, _ in runs))
            for complaint in warp_complaints + [c for _, _, cs in runs for c in cs]:
                complained = True
                print(f"    {complaint[:150]}")

    if complained:
        print("\ninconclusive: the debug layer objected, so this run is not evidence about a driver")
        return 2
    print("\nbug reproduced" if reproduced else "\nnot reproduced: the adapters agree")
    return 1 if reproduced else 0


if __name__ == "__main__":
    sys.exit(main())
