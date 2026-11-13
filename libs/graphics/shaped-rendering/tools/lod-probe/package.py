#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Packs lod-probe into one zip that runs on a machine without the source tree or a build.

    uv run libs/graphics/shaped-rendering/tools/lod-probe/package.py [--out lod-probe.zip]

It builds the frozen preset, whose shaders come from the binary's embedded copy rather than from the source tree.
The zip holds the exe, every DLL it loads — DXC's among them, since SGL still compiles at run time — and the readme.
"""

import argparse
import json
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
PRESET = "frozen-clang"
CONFIGURE_PRESET = "x64-windows-clang-ninja-frozen"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "lod-probe.zip", help="the zip to write")
    args = parser.parse_args()

    build = subprocess.run(["uv", "run", "dev.py", "build", "-t", "lod-probe", "--preset", PRESET], cwd=ROOT)
    if build.returncode != 0:
        return build.returncode

    # The manifest configure writes is how a tool outside dev.py finds a nexus binary.
    manifest = json.loads((ROOT / "build" / CONFIGURE_PRESET / "nexus-binaries.json").read_text(encoding="utf-8"))
    exe = Path(manifest["paths"]["lod-probe"])
    # The build writes the DLL list beside the exe, one absolute path per line.
    # DXC's import library is in both lists the build joins, so names are deduplicated.
    listed = [Path(line.strip()) for line in exe.with_name("lod-probe.runtime-dlls").read_text().splitlines() if line.strip()]
    dlls = list({dll.name.lower(): dll for dll in listed}.values())
    readme = Path(__file__).with_name("readme.md")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.write(exe, f"lod-probe/{exe.name}")
        for dll in dlls:
            z.write(dll, f"lod-probe/{dll.name}")
        z.write(readme, "lod-probe/readme.md")
    print(f"wrote {args.out} ({len(dlls)} DLLs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
