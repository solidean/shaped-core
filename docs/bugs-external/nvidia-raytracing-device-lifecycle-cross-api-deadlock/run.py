#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the cross-API ray-tracing device-lifecycle deadlock reproduction.

Standalone: no shaped-core imports, no dev.py.
It finds a compiler and the Vulkan SDK itself, builds repro.cc, and runs every pair of thread roles in the matrix.
Windows only, since one half of the bug is D3D12.

    uv run run.py               # the full matrix
    uv run run.py --quick       # just the cross-API pair that hangs fastest
    uv run run.py --repeat 5    # more attempts per pair; the hang is intermittent
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


def find_vulkan_sdk() -> Path | None:
    """The Vulkan SDK, from VULKAN_SDK or the newest install under the usual root."""
    env = os.environ.get("VULKAN_SDK")
    if env and Path(env).is_dir():
        return Path(env)
    root = Path("C:/VulkanSDK")
    if root.is_dir():
        installs = sorted((d for d in root.iterdir() if d.is_dir()), reverse=True)
        if installs:
            return installs[0]
    return None


def build(sdk: Path, workdir: Path) -> Path | None:
    """Compile repro.cc with clang-cl, which finds the Windows SDK and the MSVC libraries on its own."""
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = [
        "clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD",
        f"/I{sdk / 'Include'}", str(SOURCE),
        f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}",
        "/link", f"/LIBPATH:{sdk / 'Lib'}",
    ]
    print("building with clang-cl ...")
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


# (a, b, expected) — expected is what driver 591.86 did, so a changed row is the news.
PAIRS = [
    ("vk", "dx-prebuild", "hang"),
    ("dx-prebuild", "vk-pipeline", "hang"),
    ("vk", "vk-pipeline", "hang"),
    ("vk-plain", "dx-prebuild", "ok"),
    ("vk-plain", "vk-pipeline", "ok"),
    ("dx", "vk-pipeline", "ok"),
    ("vk-sizes", "dx", "ok"),
    ("vk", "vk-sizes", "ok"),
    ("dx", "dx-prebuild", "ok"),
    ("dx-prebuild", "dx-prebuild", "ok"),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true", help="run only the first cross-API pair")
    parser.add_argument("--repeat", type=int, default=2, help="attempts per pair")
    args = parser.parse_args()

    if sys.platform != "win32":
        print("Windows only: one half of this bug is D3D12", file=sys.stderr)
        return 2

    sdk = find_vulkan_sdk()
    if sdk is None:
        print("no Vulkan SDK found (set VULKAN_SDK)", file=sys.stderr)
        return 2
    print(f"Vulkan SDK: {sdk}")

    workdir = Path(tempfile.mkdtemp(prefix="nv-rt-lifecycle-"))
    try:
        exe = build(sdk, workdir)
        if exe is None:
            return 2

        pairs = PAIRS[:1] if args.quick else PAIRS
        rows: list[tuple[str, str, str, str]] = []
        for a, b, expected in pairs:
            verdicts: list[str] = []
            for _ in range(args.repeat):
                try:
                    # The repro reports a hang itself after five seconds without progress, so this is only a backstop.
                    out = subprocess.run([str(exe), "--a", a, "--b", b], capture_output=True, text=True, timeout=60)
                except subprocess.TimeoutExpired:
                    verdicts.append("HUNG(hard)")
                    continue
                verdicts.append({0: "ok", 1: "HUNG", 2: "setup-failed"}.get(out.returncode, f"exit {out.returncode}"))
                last = out.stdout.strip().splitlines()[-1:] or [""]
                print(f"  {a} vs {b}: {verdicts[-1]}  {last[0]}")
            hangs = sum(1 for v in verdicts if v.startswith("HUNG"))
            rows.append((f"{a} vs {b}", f"{hangs}/{len(verdicts)}", expected, ", ".join(verdicts)))

        width = max(len(r[0]) for r in rows)
        print()
        print(f"{'pair':<{width}}  hangs  591.86  attempts")
        print(f"{'-' * width}  -----  ------  --------")
        for name, ratio, expected, detail in rows:
            print(f"{name:<{width}}  {ratio:<5}  {expected:<6}  {detail}")

        print()
        if any(r[1][0] != "0" for r in rows):
            print("reproduced - see readme.md")
            return 1
        print("not reproduced here; raise --repeat, or the driver no longer has it")
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
