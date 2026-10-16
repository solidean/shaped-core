#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the libc++ <float.h> guard reproduction across clangs, SDKs and standards.

Standalone: it does not import anything from shaped-core and does not use dev.py.
It compiles repro.cc with -fsyntax-only, so a verdict is "does FLT_MIN resolve", and prints a table.

    uv run run.py                          # Homebrew llvm@22 and llvm, AppleClang, every macOS SDK found
    uv run run.py --clang /path/clang++    # add another clang, e.g. an unpacked LLVM release

Each configuration that reproduces is retried with tools/cmake/libcxx-float-h, the workaround in the tree, as a `+ shim` row.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"
SHIM = HERE.parents[2] / "tools" / "cmake" / "libcxx-float-h"

HOMEBREW_CLANGS = ["/opt/homebrew/opt/llvm@22/bin/clang++", "/opt/homebrew/opt/llvm/bin/clang++"]
SDK_DIRS = [Path("/Library/Developer/CommandLineTools/SDKs")]


def clang_version(clang: str) -> str:
    out = subprocess.run([clang, "--version"], capture_output=True, text=True).stdout
    return out.splitlines()[0] if out else "?"


def find_sdks() -> list[Path]:
    """Every versioned MacOSX<N>.sdk directory, plus the one xcrun selects; symlinked duplicates are dropped."""
    found: dict[Path, Path] = {}
    xcrun = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
    candidates = [Path(xcrun)] if xcrun else []
    for d in SDK_DIRS:
        if d.is_dir():
            candidates += sorted(p for p in d.glob("MacOSX*.sdk") if p.name != "MacOSX.sdk")
    for c in candidates:
        found.setdefault(c.resolve(), c)
    return list(found.values())


def compile_one(clang: str, sdk: Path, std: str, shim: bool) -> tuple[str, str]:
    cmd = [clang, f"-std={std}", "-isysroot", str(sdk), "-fsyntax-only", str(SOURCE)]
    if shim:
        cmd[1:1] = ["-isystem", str(SHIM)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0:
        return "ok", ""
    errors = [line for line in r.stderr.splitlines() if "error:" in line]
    first = errors[0].split("error:", 1)[1].strip() if errors else "no diagnostic"
    return ("REPRODUCED" if "FLT_MIN" in first else "build-failed"), first


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clang", action="append", default=[], help="another clang++ to include")
    args = parser.parse_args()

    if sys.platform != "darwin":
        print("this reproduction needs macOS and its SDKs")
        return 0

    clangs: list[str] = []
    for c in HOMEBREW_CLANGS + args.clang + [shutil.which("clang++") or ""]:
        if c and Path(c).is_file() and Path(c).resolve() not in {Path(x).resolve() for x in clangs}:
            clangs.append(c)

    rows: list[tuple[str, str, str, str, str]] = []
    for clang in clangs:
        version = clang_version(clang)
        for sdk in find_sdks():
            for std in ("c++17", "c++23"):
                verdict, detail = compile_one(clang, sdk, std, shim=False)
                rows.append((version, sdk.name, std, verdict, detail))
                if verdict == "REPRODUCED":
                    verdict, detail = compile_one(clang, sdk, std, shim=True)
                    rows.append((version + " + shim", sdk.name, std, verdict, detail))

    widths = [max(len(r[i]) for r in rows) for i in range(3)]
    for r in rows:
        print(f"{r[0]:<{widths[0]}}  {r[1]:<{widths[1]}}  {r[2]:<{widths[2]}}  {r[3]:<12}  {r[4]}")

    reproduced = [r for r in rows if r[3] == "REPRODUCED" and "shim" not in r[0]]
    shim_failed = [r for r in rows if r[3] != "ok" and "shim" in r[0]]
    print()
    if shim_failed:
        print(f"the shim does not hold on {len(shim_failed)} configuration(s)")
    if reproduced:
        print(f"reproduced on {len(reproduced)} configuration(s) - see readme.md")
        return 1
    print("not reproduced on any configuration available here")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
