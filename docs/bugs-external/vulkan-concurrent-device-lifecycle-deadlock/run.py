#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the concurrent VkDevice create/destroy reproduction.

Standalone: no shaped-core imports, no dev.py.
It finds a compiler and the Vulkan SDK itself, builds repro.cc, and runs a matrix of configurations
to narrow what the hang actually needs.

    uv run run.py                 # the full matrix
    uv run run.py --quick         # just the configuration that reproduces fastest
    uv run run.py --threads 16    # override the thread count for every case
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
    """The Vulkan SDK, from VULKAN_SDK or the usual install root."""
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
    """Compile repro.cc against the SDK with whatever compiler is on PATH.

    Returns the exe, or None when it could not be built.
    """
    exe = workdir / "repro.exe"

    if shutil.which("clang++"):
        cmd = [
            "clang++", "-O2", "-std=c++20",
            f"-I{sdk / 'Include'}", str(SOURCE),
            f"-L{sdk / 'Lib'}", "-lvulkan-1",
            "-o", str(exe),
        ]
    elif shutil.which("clang-cl"):
        cmd = [
            "clang-cl", "/O2", "/std:c++20", "/EHsc",
            f"/I{sdk / 'Include'}", str(SOURCE),
            f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}",
            "/link", f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib",
        ]
    else:
        print("no clang++ or clang-cl on PATH", file=sys.stderr)
        return None

    print("building:", " ".join(cmd[:2]), "...")
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return None
    return exe


# Each case narrows one variable, so the table says what the hang actually needs.
CASES = [
    ("churn only",                  []),
    ("churn + compute pipelines",   ["--pipelines"]),
    ("churn + validation",          ["--validation"]),
    ("churn + raytracing pipelines", ["--raytracing"]),
    ("churn + raytracing + valid.",  ["--validation", "--raytracing"]),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true", help="run only the last (most loaded) case")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--iterations", type=int, default=6)
    parser.add_argument("--timeout", type=int, default=60, help="seconds before the repro calls itself hung")
    parser.add_argument("--repeat", type=int, default=3, help="attempts per case; the hang is intermittent")
    parser.add_argument("--device", type=int, default=0, help="physical device index, to compare two vendors on one box")
    args = parser.parse_args()

    sdk = find_vulkan_sdk()
    if sdk is None:
        print("no Vulkan SDK found (set VULKAN_SDK)", file=sys.stderr)
        return 2
    print(f"Vulkan SDK: {sdk}")

    workdir = Path(tempfile.mkdtemp(prefix="vk-device-churn-"))
    try:
        exe = build(sdk, workdir)
        if exe is None:
            return 2

        cases = CASES[-1:] if args.quick else CASES
        rows: list[tuple[str, str, str]] = []

        for name, flags in cases:
            verdicts: list[str] = []
            for attempt in range(args.repeat):
                cmd = [str(exe), "--threads", str(args.threads),
                       "--iterations", str(args.iterations),
                       "--timeout", str(args.timeout),
                       "--device", str(args.device), *flags]
                try:
                    # The repro self-reports a hang and quick_exit(1)s, so its own timeout fires first.
                    # This one is the backstop for a process wedged so hard it cannot even report.
                    out = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout + 30)
                except subprocess.TimeoutExpired:
                    verdicts.append("HUNG(hard)")
                    continue

                if out.returncode == 0:
                    verdicts.append("ok")
                elif out.returncode == 1:
                    verdicts.append("HUNG")
                elif out.returncode == 2:
                    verdicts.append("no-device")
                else:
                    verdicts.append(f"exit {out.returncode}")

                print(f"  [{name}] attempt {attempt + 1}: {verdicts[-1]}")
                for line in out.stdout.strip().splitlines()[-3:]:
                    print(f"      {line}")

            hangs = sum(1 for v in verdicts if v.startswith("HUNG"))
            rows.append((name, f"{hangs}/{len(verdicts)}", ", ".join(verdicts)))

        width = max(len(r[0]) for r in rows)
        print()
        print(f"{'case':<{width}}  hangs  attempts")
        print(f"{'-' * width}  -----  --------")
        for name, ratio, detail in rows:
            print(f"{name:<{width}}  {ratio:<5}  {detail}")

        reproduced = any(int(r[1].split('/')[0]) > 0 for r in rows)
        print()
        if reproduced:
            print("reproduced - see readme.md")
            return 1
        print("not reproduced here; try raising --threads, --iterations or --repeat")
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
