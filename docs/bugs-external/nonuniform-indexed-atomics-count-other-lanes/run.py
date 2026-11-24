#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the non-uniformly indexed atomics reproduction, on D3D12 and on vulkan.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc (D3D12) and vulkan/repro.cc with clang-cl.
shader.hlsl is compiled once per configuration: to DXIL by the newest Windows SDK's dxc, to SPIR-V by the Vulkan SDK's.
Then each configuration dispatches once on every adapter whose name holds one of the vendors below, and on WARP.
Windows only; the vulkan arm is skipped without a Vulkan SDK.

    uv run run.py [--vulkan-sdk <dir>]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.

Exits 1 when an adapter's words differ from the expected ones, 0 when every adapter is right, 2 when the run is inconclusive.
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
RUN_TIMEOUT_SECONDS = 30
ADAPTERS = ["intel", "amd", "nvidia", "warp"]

# (name, dxc arguments, expected `even add min, odd add min`);
# the first is the shader SGL writes, and the bug rows after it each change one thing about it;
# the workaround rows are shapes that might avoid the bug, so their being wrong is no verdict on the bug
NONE = 0xFFFFFFFF
# SPIR-V's MaximallyReconvergesKHR execution mode; DXIL has no such flag, so its rows are the plain ones there
MAXIMAL = "-fspv-enable-maximal-reconvergence"
CONFIGURATIONS = [
    ("add and min, results unread", [], (32, 0, 32, 1)),
    ("add alone", ["-DNO_MIN"], (32, NONE, 32, NONE)),
    ("min alone", ["-DNO_ADD"], (0, 0, 0, 1)),
    ("add and min, results read", ["-DUSE_RESULT"], (32, 0, 32, 1)),
    ("add alone, its result read", ["-DNO_MIN", "-DUSE_RESULT"], (32, NONE, 32, NONE)),
    ("add and min, at shader model 6.8", ["-T", "cs_6_8"], (32, 0, 32, 1)),
    ("add and min, unoptimized", ["-Od"], (32, 0, 32, 1)),
    ("the buffer picked by a branch, literal indices", ["-DBRANCH"], (32, 0, 32, 1)),
    ("workaround: a loop over indices, leaving by break", ["-DBREAK_LOOP"], (32, 0, 32, 1)),
    ("  as above, unoptimized", ["-DBREAK_LOOP", "-Od"], (32, 0, 32, 1)),
    ("  as above, maximally reconverging", ["-DBREAK_LOOP", MAXIMAL], (32, 0, 32, 1)),
    ("workaround: a loop over indices, leaving by a flag", ["-DFLAG_LOOP"], (32, 0, 32, 1)),
    ("  as above, maximally reconverging", ["-DFLAG_LOOP", MAXIMAL], (32, 0, 32, 1)),
    ("  as above, the round's index unmarked", ["-DFLAG_LOOP_UNMARKED"], (32, 0, 32, 1)),
    ("  as above, every round on the even buffer", ["-DFLAG_LOOP_ONE_BUFFER", "-DNO_MIN"], (64, NONE, 0, NONE)),
    ("workaround: one atomic per index, by a leader", ["-DLEADER"], (32, 0, 32, 1)),
    ("  as above, maximally reconverging", ["-DLEADER", MAXIMAL], (32, 0, 32, 1)),
    ("workaround: a compare-exchange by a leader", ["-DLEADER_CAS"], (32, 0, 32, 1)),
    ("  as above, unoptimized", ["-DLEADER_CAS", "-Od"], (32, 0, 32, 1)),
    ("  as above, maximally reconverging", ["-DLEADER_CAS", MAXIMAL], (32, 0, 32, 1)),
    ("workaround: the add by a leader, results read, in the round", ["-DROUND_CAS"], (32, 0, 32, 0)),
    ("  as above, maximally reconverging", ["-DROUND_CAS", MAXIMAL], (32, 0, 32, 0)),
    ("workaround: the add by a leader, natively, results read", ["-DROUND_NATIVE"], (32, 0, 32, 0)),
    ("  as above, maximally reconverging", ["-DROUND_NATIVE", MAXIMAL], (32, 0, 32, 0)),
    ("workaround: the add by a leader, results read, shuffled", ["-DSHUFFLE_CAS"], (32, 0, 32, 0)),
    ("  as above, maximally reconverging", ["-DSHUFFLE_CAS", MAXIMAL], (32, 0, 32, 0)),
    ("workaround: one lane at a time, natively", ["-DTURN_NATIVE"], (32, 0, 32, 1)),
    ("workaround: one lane at a time within a round, natively", ["-DROUND_TURN_NATIVE"], (32, 0, 32, 1)),
    ("workaround: a compare-exchange per lane", ["-DLANE_CAS"], (32, 0, 32, 1)),
    ("  as above, maximally reconverging", ["-DLANE_CAS", MAXIMAL], (32, 0, 32, 1)),
]
BUG_ROWS = 8


def find_windows_dxc() -> Path | None:
    """The newest Windows SDK's dxc, which signs DXIL through the dxil.dll beside it."""
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def find_vulkan(given: str | None) -> Path | None:
    candidates = [given, os.environ.get("VULKAN_SDK")]
    tools = Path.home() / "tools" / "vulkan-sdk"
    if tools.is_dir():
        candidates += [str(p) for p in sorted(tools.iterdir(), reverse=True)]
    for candidate in candidates:
        if candidate and (Path(candidate) / "Include" / "vulkan" / "vulkan.h").is_file():
            return Path(candidate)
    return None


def build(source: str, exe: Path, extra: list[str], link: list[str]) -> bool:
    command = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", *extra, str(HERE / source), f"/Fe:{exe}",
               f"/Fo:{exe.with_suffix('.obj')}"]
    built = subprocess.run([*command, *(["/link", *link] if link else [])], capture_output=True, text=True, cwd=exe.parent)
    if built.returncode != 0:
        print(built.stdout + built.stderr, file=sys.stderr)
    return built.returncode == 0


def compile_shader(dxc: Path, arguments: list[str], output: Path, spirv: bool) -> bool:
    profile = [] if "-T" in arguments else ["-T", "cs_6_0"]
    target = ["-spirv", "-fspv-target-env=vulkan1.2"] if spirv else []
    if not spirv:
        arguments = [a for a in arguments if a != MAXIMAL]
    result = subprocess.run([str(dxc), "-nologo", "-E", "main", *target, *profile, *arguments, "-Fo", str(output),
                             str(HERE / "shader.hlsl")], capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode == 0


def dispatch(exe: Path, shader: Path, adapter: str) -> tuple[tuple[int, ...] | str | None, str]:
    """The four words, what stopped the run, or None without such an adapter; and the adapter's line."""
    try:
        done = subprocess.run([str(exe), str(shader), "--adapter", adapter], capture_output=True, timeout=RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return "hung", ""
    lines = done.stdout.decode(errors="replace").splitlines()
    name = next((line for line in lines if line.startswith("adapter:")), "")
    for line in lines:
        if line.startswith("even "):
            parts = line.replace(",", "").split()
            return (int(parts[1]), int(parts[2]), int(parts[4]), int(parts[5])), name
        if "no adapter matches" in line:
            return None, name
        if line.startswith("error:"):
            return line[7:], name
    return f"exited 0x{done.returncode & 0xFFFFFFFF:08x}", name


def show(words: tuple[int, ...]) -> str:
    text = [("-" if w == NONE else str(w)) for w in words]
    return f"even {text[0]} {text[1]}, odd {text[2]} {text[3]}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vulkan-sdk")
    options = parser.parse_args()
    if os.name != "nt":
        print("Windows only", file=sys.stderr)
        return 2
    windows_dxc = find_windows_dxc()
    if windows_dxc is None or shutil.which("clang-cl") is None:
        print("needs clang-cl on PATH and a Windows SDK's dxc", file=sys.stderr)
        return 2
    sdk = find_vulkan(options.vulkan_sdk)

    reproduced = False
    inconclusive = False
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        # (API, the repro, the dxc, whether it writes SPIR-V)
        arms = []
        if build("repro.cc", work / "d3d12.exe", [], []):
            arms.append(("d3d12", work / "d3d12.exe", windows_dxc, False))
        else:
            return 2
        if sdk is None:
            print("no Vulkan SDK found, so no vulkan arm; pass --vulkan-sdk <dir>\n")
        elif build("vulkan/repro.cc", work / "vulkan.exe", [f"/I{sdk / 'Include'}"], [f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib"]):
            arms.append(("vulkan", work / "vulkan.exe", sdk / "Bin" / "dxc.exe", True))
        else:
            return 2

        for api, exe, dxc, spirv in arms:
            shaders = []
            for index, (_, arguments, _) in enumerate(CONFIGURATIONS):
                shader = work / f"{api}-{index}.bin"
                if not compile_shader(dxc, arguments, shader, spirv):
                    return 2
                shaders.append(shader)
            for adapter in ADAPTERS:
                if adapter == "warp" and api != "d3d12":
                    continue
                rows = []
                header = ""
                for row, ((name, _, expected), shader) in enumerate(zip(CONFIGURATIONS, shaders)):
                    got, header = dispatch(exe, shader, adapter)
                    if got is None:
                        break
                    if isinstance(got, str):
                        inconclusive = True
                        rows.append(f"  {name:<60} {got}")
                        continue
                    wrong = got != expected
                    reproduced = reproduced or (wrong and row < BUG_ROWS)
                    rows.append(f"  {name:<60} {show(got):<26} {'WRONG, expected ' + show(expected) if wrong else 'right'}")
                if rows:
                    print(f"{api}, {header}")
                    print("\n".join(rows), flush=True)
                    print()

    if reproduced:
        print("bug reproduced: an adapter's atomics reached the wrong buffer")
        return 1
    if inconclusive:
        print("inconclusive: a run stopped before its dispatch finished")
        return 2
    print("not reproduced: every adapter's words are the expected ones")
    return 0


if __name__ == "__main__":
    sys.exit(main())
