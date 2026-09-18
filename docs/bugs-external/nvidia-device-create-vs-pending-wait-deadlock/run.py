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

# Each binary: its source, and the libraries beyond vulkan-1 it links.
BINARIES = {
    "repro": (HERE / "repro.cc", []),
    "cross": (HERE / "cross-api" / "repro.cc", ["d3d12", "dxgi"]),
}


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


def build(sdk: Path, workdir: Path, name: str) -> Path | None:
    """Compile one of BINARIES against the SDK with whatever compiler is on PATH.

    Returns the exe, or None when it could not be built.
    """
    source, libs = BINARIES[name]
    exe = workdir / f"{name}.exe"

    if shutil.which("clang++"):
        cmd = [
            "clang++", "-O2", "-std=c++20",
            f"-I{sdk / 'Include'}", str(source),
            f"-L{sdk / 'Lib'}", "-lvulkan-1", *(f"-l{lib}" for lib in libs),
            "-o", str(exe),
        ]
    elif shutil.which("clang-cl"):
        cmd = [
            "clang-cl", "/O2", "/std:c++20", "/EHsc",
            f"/I{sdk / 'Include'}", str(source),
            f"/Fe:{exe}", f"/Fo:{workdir / (name + '.obj')}",
            "/link", f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib", *(f"{lib}.lib" for lib in libs),
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
# The cross-API cases come first: none of them hangs, so they cost nothing to run before the ones that do.
CASES = [
    ("cross", "D3D12 wait + vkCreateDevice, host signal",   ["--wait", "dx", "--create", "vk", "--signal", "host"]),
    ("cross", "D3D12 wait + vkCreateDevice, queue signal",  ["--wait", "dx", "--create", "vk", "--signal", "queue"]),
    ("cross", "Vulkan wait + D3D12CreateDevice, host",      ["--wait", "vk", "--create", "dx", "--signal", "host"]),
    ("cross", "Vulkan wait + D3D12CreateDevice, queue",     ["--wait", "vk", "--create", "dx", "--signal", "queue"]),
    ("repro", "create B, no pending wait on A",             ["--no-wait"]),
    ("repro", "pending wait on A, no create",               ["--no-create"]),
    ("repro", "pending wait + create B, host signal",       ["--signal", "host"]),
    ("repro", "pending wait + create B, queue signal",      []),
]
QUICK = "pending wait + create B, host signal"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cross-only", action="store_true", help="run only the cross-API cases, none of which hangs")
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
        exes = {name: build(sdk, workdir, name) for name in BINARIES}
        if any(exe is None for exe in exes.values()):
            return 2

        cases = CASES
        if args.quick:
            cases = [c for c in CASES if c[1] == QUICK]
        elif args.cross_only:
            cases = [c for c in CASES if c[0] == "cross"]
        rows: list[tuple[str, str, str]] = []

        for binary, name, flags in cases:
            exe = exes[binary]
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
        if args.cross_only:
            print("cross-API cases done; none is expected to hang - see readme.md")
            return 1 if reproduced else 0
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
