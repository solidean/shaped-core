#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the 64-bit workgroup atomic load and store reproduction on vulkan.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl, and assembles each row of shader.spvasm with the Vulkan SDK's spirv-as, checked by spirv-val.
Each row zeroes a workgroup's 64-bit sum and reads it back its own way; the adds between are the same 64 `OpAtomicIAdd`s.
Windows only; the host runs on the first hardware adapter with 64-bit workgroup atomics, or the first of those `--adapter` names.

    uv run run.py [--adapter <substring>] [--vulkan-sdk <dir>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
Exits 1 when a row whose SPIR-V passes spirv-val reads a wrong sum while the exchange-and-or row reads it right.
0 when every row reads it right, 2 when the run is inconclusive.
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

STORE = "OpAtomicStore %sum %scope %relaxed %ulong_0"
EXCHANGE = "%zeroed_from = OpAtomicExchange %ulong %sum %scope %relaxed %ulong_0"
LOAD = "%total = OpAtomicLoad %ulong %sum %scope %relaxed"
OR = "%total = OpAtomicOr %ulong %sum %scope %relaxed %ulong_0"
DEVICE, WORKGROUP = "1", "2"

# (row, the store that zeroes the sum, the read of the total, the scope); the last is the shape DXC writes
ROWS = [
    ("store, load, Device", STORE, LOAD, DEVICE),
    ("store, load, Workgroup", STORE, LOAD, WORKGROUP),
    ("exchange, load, Workgroup", EXCHANGE, LOAD, WORKGROUP),
    ("store, or 0, Workgroup", STORE, OR, WORKGROUP),
    ("exchange, or 0, Workgroup", EXCHANGE, OR, WORKGROUP),
]
CONTROL = ROWS[-1][0]
RUN_TIMEOUT_SECONDS = 60


def find_vulkan(given: str | None) -> Path | None:
    candidates = [given, os.environ.get("VULKAN_SDK")]
    tools = Path.home() / "tools" / "vulkan-sdk"
    if tools.is_dir():
        candidates += [str(p) for p in sorted(tools.iterdir(), reverse=True)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "Include" / "vulkan" / "vulkan.h").is_file():
            return Path(candidate)
    return None


def run_tool(command: list[str]) -> bool:
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        print(done.stdout + done.stderr, file=sys.stderr)
    return done.returncode == 0


def assemble(sdk: Path, row: tuple[str, str, str, str], output: Path) -> bool:
    _, store, load, scope = row
    text = (HERE / "shader.spvasm").read_text(encoding="utf-8")
    text = text.replace("STORE", store).replace("LOAD", load).replace("SCOPE", scope)
    source = output.with_suffix(".spvasm")
    source.write_text(text, encoding="utf-8")
    bin_dir = sdk / "Bin"
    return run_tool([str(bin_dir / "spirv-as.exe"), "--target-env", "vulkan1.2", str(source), "-o", str(output)]) and run_tool(
        [str(bin_dir / "spirv-val.exe"), "--target-env", "vulkan1.2", str(output)]
    )


def dispatch(command: list[str]) -> tuple[int, str]:
    """The host's exit code, 0 the sum is right, 1 it is wrong, 2 no run; and its line."""
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return 2, "timed out"
    lines = done.stdout.strip().splitlines()
    return (done.returncode if done.returncode in (0, 1) else 2), (lines[-1] if lines else f"exited {done.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adapter", default="", help="a substring of the vulkan adapter's name")
    parser.add_argument("--vulkan-sdk")
    options = parser.parse_args()
    if sys.platform != "win32":
        print("this reproduction runs on Windows alone")
        return 2
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH")
        return 2
    sdk = find_vulkan(options.vulkan_sdk)
    if sdk is None:
        print("no Vulkan SDK found; pass --vulkan-sdk <dir>")
        return 2

    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        exe = work / "repro.exe"
        if not run_tool(["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", f"/I{sdk / 'Include'}",
                         str(HERE / "repro.cc"), f"/Fe:{exe}", f"/Fo:{exe.with_suffix('.obj')}", "/link",
                         f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib"]):
            return 2
        for index, row in enumerate(ROWS):
            shader = work / f"row-{index}.spv"
            if not assemble(sdk, row, shader):
                return 2
            code, line = dispatch([str(exe), str(shader), "--adapter", options.adapter])
            results[row[0]] = code
            print(f"{row[0]:27} {line}")

    if results.get(CONTROL) != 0:
        print(f"\ninconclusive: the control row, {CONTROL}, did not read the sum right")
        return 2
    wrong = [name for name, code in results.items() if code == 1]
    if wrong:
        print(f"\nREPRODUCED: a valid 64-bit workgroup atomic reads a wrong sum, in: {', '.join(wrong)}")
        return 1
    if any(code != 0 for code in results.values()):
        print("\ninconclusive: a row did not run")
        return 2
    print("\nnot reproduced: every row reads the sum right")
    return 0


if __name__ == "__main__":
    sys.exit(main())
