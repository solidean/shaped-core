#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Drive the float-to-half round-trip reproduction, on D3D12 and on vulkan.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc (D3D12) and vulkan/repro.cc with clang-cl, and compiles shader.hlsl with -enable-16bit-types, with -Gis and
without: DXIL with the newest Windows SDK's dxc, SPIR-V with the Vulkan SDK's.
D3D12 runs on the high-performance adapter and on WARP, vulkan on the first hardware adapter, or the one `--adapter` names.
Windows only; the vulkan arm is skipped without a Vulkan SDK.

    uv run run.py [--adapter <substring>] [--vulkan-sdk <dir>] [--dxc <dxc.exe>] [--profile cs_6_2]

`--vulkan-sdk` defaults to $VULKAN_SDK, then to the newest `~/tools/vulkan-sdk/<version>`.
`--dxc` compiles both arms with one dxc in place of the SDKs' own, as shaped-core's pinned one.
Exits 1 when a hardware adapter returns a round trip that is no half: on D3D12 where WARP returns halves from the
same DXIL, on vulkan by the rule alone.
0 when every round trip is a half, 2 when the run is inconclusive.
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

# (row, dxc flags); no fast-math flag licenses folding a conversion away, so both rows ask for a half
SHADERS = [
    ("strict (-Gis)", ["-Gis"]),
    ("fast", []),
]
RUN_TIMEOUT_SECONDS = 60


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


def compile_shader(dxc: Path, flags: list[str], output: Path, spirv: bool, profile: str) -> bool:
    target = ["-spirv", "-fspv-target-env=vulkan1.2"] if spirv else []
    result = subprocess.run([str(dxc), "-nologo", "-T", profile, "-E", "cs", "-O3", "-enable-16bit-types", *target,
                             *flags, "-Fo", str(output), str(HERE / "shader.hlsl")], capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode == 0


def dispatch(command: list[str]) -> tuple[int, str]:
    """The host's exit code, 0 every result a half, 1 one is not, 2 no run; and its line."""
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
    parser.add_argument("--dxc", help="one dxc for both arms, in place of the SDKs' own")
    parser.add_argument("--profile", default="cs_6_2")
    options = parser.parse_args()
    if sys.platform != "win32":
        print("this reproduction runs on Windows alone")
        return 2
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH")
        return 2
    windows_dxc = Path(options.dxc) if options.dxc else find_windows_dxc()
    if windows_dxc is None:
        print("no Windows SDK dxc found")
        return 2

    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        arms = []
        if build("repro.cc", work / "d3d12.exe", [], []):
            arms.append(("d3d12", "hardware", windows_dxc, False, [str(work / "d3d12.exe")]))
            arms.append(("d3d12", "WARP", windows_dxc, False, [str(work / "d3d12.exe"), "--warp"]))
        sdk = find_vulkan(options.vulkan_sdk)
        if sdk is None:
            print("no Vulkan SDK found, so no vulkan arm; pass --vulkan-sdk <dir>\n")
        elif build("vulkan/repro.cc", work / "vulkan.exe", [f"/I{sdk / 'Include'}"],
                   [f"/LIBPATH:{sdk / 'Lib'}", "vulkan-1.lib"]):
            arms.append(("vulkan", "hardware", Path(options.dxc) if options.dxc else sdk / "Bin" / "dxc.exe", True,
                         [str(work / "vulkan.exe"), "--adapter", options.adapter]))
        if not arms:
            return 2
        for api, kind, dxc, spirv, command in arms:
            for index, (name, flags) in enumerate(SHADERS):
                shader = work / f"cs-{api}-{index}.{'spv' if spirv else 'cso'}"
                if not shader.is_file() and not compile_shader(dxc, flags, shader, spirv, options.profile):
                    return 2
                code, line = dispatch([command[0], str(shader), *command[1:]])
                results[(api, kind, name)] = code
                print(f"{api:7} {kind:9} {name:14} {line}")

    # D3D12 is held against WARP on the same DXIL; vulkan against the rule alone, since no half is right by any rounding
    reproduced = [f"d3d12 {name}" for name, _ in SHADERS
                  if results.get(("d3d12", "hardware", name)) == 1 and results.get(("d3d12", "WARP", name)) == 0]
    reproduced += [f"vulkan {name}" for name, _ in SHADERS if results.get(("vulkan", "hardware", name)) == 1]
    if reproduced:
        print(f"\nREPRODUCED: round trips through a half that are no half, on {', '.join(reproduced)}")
        return 1
    if any(results.get(("d3d12", kind, name)) != 0 for name, _ in SHADERS for kind in ("hardware", "WARP")):
        print("\ninconclusive: a D3D12 run did not complete, or WARP, the reference, returned no half")
        return 2
    print("\nnot reproduced: every round trip is a half")
    return 0


if __name__ == "__main__":
    sys.exit(main())
