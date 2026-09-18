#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the vkCreateDevice-against-a-pending-wait reproduction.

Standalone: no shaped-core imports, no dev.py.
It finds a compiler and the Vulkan SDK itself, builds repro.cc, and runs a matrix of configurations
to narrow what the hang actually needs.
The controls run first: a hang on the affected driver can leave a process the OS cannot kill.

    uv run run.py                 # the full matrix, one attempt per case
    uv run run.py --quick         # just the case that reproduces
    uv run run.py --repeat 3      # more attempts per case
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


# Each case removes one variable, so the table says what the hang actually needs.
# The last one is the suspected hang; everything above it is a control.
CASES = [
    ("create B, no pending wait on A",         ["--no-wait"]),
    ("pending wait on A, no create",           ["--no-create"]),
    ("pending wait + create B, host signal",   ["--signal", "host"]),
    ("pending wait + create B, queue signal",  []),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--quick", action="store_true", help="run only the host-signal case, the cleanest hang")
    parser.add_argument("--timeout", type=int, default=30, help="seconds before the repro calls itself hung")
    parser.add_argument("--delay", type=int, default=500, help="ms between starting device B and signalling A")
    parser.add_argument("--repeat", type=int, default=1, help="attempts per case")
    parser.add_argument("--device", type=int, default=0, help="physical device index, to compare two vendors on one box")
    args = parser.parse_args()

    sdk = find_vulkan_sdk()
    if sdk is None:
        print("no Vulkan SDK found (set VULKAN_SDK)", file=sys.stderr)
        return 2
    print(f"Vulkan SDK: {sdk}")

    workdir = Path(tempfile.mkdtemp(prefix="vk-create-vs-wait-"))
    try:
        exe = build(sdk, workdir)
        if exe is None:
            return 2

        cases = CASES[2:3] if args.quick else CASES
        rows: list[tuple[str, str, str]] = []

        for name, flags in cases:
            verdicts: list[str] = []
            for attempt in range(args.repeat):
                cmd = [str(exe), "--timeout", str(args.timeout), "--delay", str(args.delay),
                       "--device", str(args.device), *flags]
                # Output goes to a file and the child is never waited on past the deadline.
                # On the affected driver a hung repro sits in the kernel, where it can neither report nor be killed,
                # and subprocess.run's kill-then-wait would hang this script with it.
                log = workdir / f"case-{len(rows)}-{attempt}.txt"
                with open(log, "w") as sink:
                    proc = subprocess.Popen(cmd, stdout=sink, stderr=subprocess.STDOUT)
                    try:
                        code = proc.wait(timeout=args.timeout + 10)
                    except subprocess.TimeoutExpired:
                        code = None
                output = log.read_text(errors="replace")

                if code is None:
                    verdicts.append("HUNG(hard)")
                elif code == 0:
                    verdicts.append("ok")
                elif code == 1:
                    verdicts.append("HUNG")
                elif code == 2:
                    verdicts.append("no-device")
                else:
                    verdicts.append(f"exit {code}")

                print(f"  [{name}] attempt {attempt + 1}: {verdicts[-1]}", flush=True)
                for line in output.strip().splitlines()[-3:]:
                    print(f"      {line}", flush=True)

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
        print("not reproduced here; try raising --delay or --repeat")
        return 0
    finally:
        # A hung repro keeps its exe mapped, so the directory may outlive this run.
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
