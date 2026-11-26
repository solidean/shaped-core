#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the negated-equality reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and runs the two committed DXIL containers on every hardware adapter whose name holds
one of the vendor names below, and on WARP.
negated.dxil branches on `xor (icmp ne v, 3), true`, and swapped.dxil on `icmp ne v, 3` with its two ways swapped.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when an AMD adapter runs negated.dxil wrong and swapped.dxil right, 0 when it runs both right,
2 when the run is inconclusive.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"
CONTAINERS = ["negated", "swapped"]
ADAPTERS = ["AMD", "Intel", "NVIDIA", "--warp"]


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


def run(exe: Path, container: Path, adapter: str) -> tuple[int, str]:
    args = [adapter] if adapter == "--warp" else ["--adapter", adapter]
    try:
        out = subprocess.run([str(exe), str(container), *args], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 2, "timed out"
    return out.returncode, out.stdout.strip()


def main() -> int:
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    results: dict[tuple[str, str], int] = {}
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(Path(tmp))
        if exe is None:
            return 2
        for container in CONTAINERS:
            print(f"{container}.dxil")
            for adapter in ADAPTERS:
                code, line = run(exe, HERE / f"{container}.dxil", adapter)
                if code == 3:
                    continue
                results[(container, adapter)] = code
                print(f"    {adapter.lstrip('-'):7} {({0: 'right', 1: 'WRONG'}).get(code, 'failed'):6} {line}")
    negated = results.get(("negated", "AMD"))
    swapped = results.get(("swapped", "AMD"))
    if negated not in (0, 1) or swapped != 0 or results.get(("negated", "--warp")) != 0:
        print("inconclusive: no AMD adapter ran, the swapped module failed there, or WARP ran the negated one wrong")
        return 2
    if negated == 1:
        print("REPRODUCED: the AMD adapter stores v as 3 where a branch on a negated `v != 3` says it is not")
        return 1
    print("not reproduced: the AMD adapter runs both modules right")
    return 0


if __name__ == "__main__":
    sys.exit(main())
