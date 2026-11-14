#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the AMD unsigned-container shader-cache reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shaders/plain.hlsl and shaders/ray_query.hlsl with dxc,
the newest Windows SDK's unless --dxc names one.
An unsigned container is the compiled one with its digest replaced by the value the runtime accepts from a container no
validator signed, sixteen bytes of 0x01; nothing else of it changes.

Each sequence runs in an empty AMD shader cache, each container in a process of its own, on the first AMD adapter.
The cache is %LOCALAPPDATA%\\AMD\\DxcCache: it is set aside for the run and put back after, so nothing of it is lost.
Windows only, since the bug is D3D12's.

    uv run run.py [--dxc <dxc.exe>] [--vendor <hex PCI id>]

Exits 1 when the ray query crashes the driver after the plain shader and both controls compile,
0 when nothing crashes, 2 when inconclusive.
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
UNSIGNED_DIGEST = bytes([1] * 16)


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


def compile_shader(dxc: Path, shader: Path, out: Path) -> bool:
    result = subprocess.run([str(dxc), "-nologo", "-T", "cs_6_6", "-E", "cs", "-Fo", str(out), str(shader)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"{shader.name}: dxc failed\n{result.stdout}{result.stderr}", file=sys.stderr)
    return result.returncode == 0


def unsigned(container: Path) -> Path:
    data = bytearray(container.read_bytes())
    data[4:20] = UNSIGNED_DIGEST
    out = container.with_suffix(".unsigned.dxil")
    out.write_bytes(data)
    return out


def create_pipeline(exe: Path, container: Path, vendor: str) -> str:
    try:
        out = subprocess.run([str(exe), str(container), "--vendor", vendor], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "timeout"
    code = out.returncode & 0xFFFFFFFF
    return {0: "compiled", 2: "no adapter", 3: "refused"}.get(code, f"crashed (exit {code:#x})")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--vendor", default="1002")
    args = parser.parse_args()
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    dxc = args.dxc or find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found; pass --dxc")
        return 2
    cache = Path(os.environ["LOCALAPPDATA"]) / "AMD" / "DxcCache"
    kept = cache.with_name("DxcCache.run-py-kept")
    if kept.exists():
        print(f"{kept} is left from an earlier run; put it back as DxcCache, or delete it, first")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        plain, ray_query = workdir / "plain.dxil", workdir / "ray_query.dxil"
        if not compile_shader(dxc, HERE / "shaders" / "plain.hlsl", plain):
            return 2
        if not compile_shader(dxc, HERE / "shaders" / "ray_query.hlsl", ray_query):
            return 2
        sequences = {
            "ray query unsigned alone": [unsigned(ray_query)],
            "plain unsigned, then ray query unsigned": [unsigned(plain), unsigned(ray_query)],
            "plain unsigned, then ray query signed": [unsigned(plain), ray_query],
        }

        is_set_aside = False
        try:
            if cache.exists():
                cache.rename(kept)
            is_set_aside = True
        except PermissionError:
            print(f"{cache} is held open by another process, so the run uses it as it is")
        results: dict[str, str] = {}
        try:
            for name, containers in sequences.items():
                verdicts = [create_pipeline(exe, c, args.vendor) for c in containers]
                results[name] = verdicts[-1]
                print(f"{name:42} {' -> '.join(verdicts)}")
        finally:
            if is_set_aside:
                shutil.rmtree(cache, ignore_errors=True)
                if kept.exists():
                    kept.rename(cache)

    if any(v in ("no adapter", "timeout") for v in results.values()):
        print("inconclusive: no such adapter, no inline ray tracing, or a run did not finish")
        return 2
    crashed = results["plain unsigned, then ray query unsigned"].startswith("crashed")
    controls = results["ray query unsigned alone"] == "compiled" and results["plain unsigned, then ray query signed"] == "compiled"
    if crashed and controls:
        print("REPRODUCED: an unsigned ray query crashes the driver once an unsigned plain shader is in its cache")
        return 1
    if crashed and results["ray query unsigned alone"].startswith("crashed"):
        print("inconclusive: the ray query crashed alone too, so the cache already held a plain shader's entry; reset it and run again")
        return 2
    print("not reproduced" if not crashed else "inconclusive: a control crashed too")
    return 0 if not crashed else 2


if __name__ == "__main__":
    sys.exit(main())
