#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the cached graphics-pipeline reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles vertex.hlsl and pixel.hlsl with the newest Windows SDK's dxc.
Then each configuration draws twice, in two processes, under a nonce no earlier run has used.
The first draw is the driver's fresh compile and the second its restore from its own shader cache.
Windows only, since the bug is D3D12's.

    uv run run.py

Exits 1 when a restore draws nothing where the fresh compile drew, 0 when every draw lands, 2 when the run is inconclusive.
"""

from __future__ import annotations

import os
import secrets
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"
VERTEX = HERE / "vertex.hlsl"
PIXEL = HERE / "pixel.hlsl"

# (name, dxc defines, repro.exe flags); the first is the pipeline as shaped-core built it, the last two its controls.
ATTEMPTS = 5

CONFIGURATIONS = [
    ("as found", [], []),
    ("as found, under the debug layer and GPU-based validation", [], ["--debug"]),
    ("shaped-core's whole root signature", [], ["--full-layout"]),
    ("without the light loop", ["-DNO_LIGHT_LOOP"], []),
    ("without the ambient term", ["-DNO_AMBIENT"], []),
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


def compile_shader(dxc: Path, source: Path, entry: str, profile: str, defines: list[str], out_path: Path) -> bool:
    # The options shaped-core compiled with.
    cmd = [str(dxc), "-nologo", "-T", profile, "-E", entry, "-O3", "-WX", "-enable-16bit-types", *defines, "-Fo", str(out_path), str(source)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return False
    return True


def run(exe: Path, vs: Path, ps: Path, flags: list[str]) -> tuple[int, str]:
    out = subprocess.run([str(exe), str(vs), str(ps), *flags], capture_output=True, text=True)
    return out.returncode, out.stdout.strip()


def main() -> int:
    dxc = find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        vs = workdir / "vertex.cso"
        if exe is None or not compile_shader(dxc, VERTEX, "main_vs", "vs_6_8", [], vs):
            return 2
        reproduced = False
        inconclusive = False
        for adapter in ([], ["--warp"]):
            print(f"on {'WARP' if adapter else 'the high-performance adapter'}:")
            for name, defines, flags in CONFIGURATIONS:
                # Several attempts, since about one in eight fresh compiles writes a cache entry that restores correctly.
                hits = 0
                lines = []
                for _ in range(ATTEMPTS):
                    # A nonce of this attempt's own, so the first draw cannot meet a cache entry an earlier run left.
                    nonce = (1 << 31) | secrets.randbits(30)
                    ps = workdir / f"pixel-{nonce:08x}.cso"
                    if not compile_shader(dxc, PIXEL, "sv_raster", "ps_6_8", [*defines, f"-DNONCE={nonce}u"], ps):
                        return 2
                    fresh, fresh_out = run(exe, vs, ps, [*flags, *adapter])
                    restored, restored_out = run(exe, vs, ps, [*flags, *adapter])
                    if fresh == 2 or restored == 2 or fresh != 0:
                        inconclusive = True
                        lines.append(f"    inconclusive: fresh {fresh_out} / restored {restored_out}")
                        continue
                    hits += restored != 0
                    lines.append(f"    fresh: {fresh_out}")
                    lines.append(f"      restored: {restored_out}")
                reproduced = reproduced or hits > 0
                verdict = f"REPRODUCED in {hits} of {ATTEMPTS}" if hits > 0 else "not reproduced"
                print(f"  {name}: {verdict}")
                print("\n".join(lines))
    if reproduced:
        return 1
    return 2 if inconclusive else 0


if __name__ == "__main__":
    sys.exit(main())
