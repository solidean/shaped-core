#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the MSVC co_await-in-aggregate-initializer crash reproduction.

Standalone: it does not import anything from shaped-core and does not use dev.py.
It finds the compilers itself, compiles repro.cc once per case, and prints a table.

    uv run run.py                   # every toolset it can find, targeting x64
    uv run run.py --arch x64_arm64  # the same, targeting arm64

Exit 1 when the bug reproduces on any toolset, 0 when it does not.
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

VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"

# case, the extra flags, and what the case isolates
CASES = [
    ("CRASH", "/EHsc", "a built member with a destructor, across a co_await"),
    ("CRASH", "/EHsc /Od", "the same, with the optimizer off"),
    ("CRASH", "", "the same, without exception handling"),
    ("ARRAY", "/EHsc", "the same in an array"),
    ("AWAIT_FIRST", "/EHsc", "nothing built yet when it suspends"),
    ("NAMED", "/EHsc", "the workaround: await into a local first"),
    ("INT_FIRST", "/EHsc", "what is built has no destructor"),
    ("CONSTRUCTOR", "/EHsc", "a constructor's arguments, not an aggregate"),
    ("CALL", "/EHsc", "a function's arguments"),
]


def find_vs_install() -> Path | None:
    """The newest Visual Studio installation, previews included, via vswhere."""
    if not VSWHERE.is_file():
        return None
    out = subprocess.run(
        [str(VSWHERE), "-latest", "-prerelease", "-products", "*", "-property", "installationPath"],
        capture_output=True, text=True,
    )
    path = out.stdout.strip()
    return Path(path) if path else None


def find_msvc_toolsets(vs: Path) -> list[str]:
    """Every MSVC toolset version installed under this VS, oldest first."""
    root = vs / "VC" / "Tools" / "MSVC"
    if not root.is_dir():
        return []
    # 14.51.36231 -> the 14.51 that -vcvars_ver wants
    versions = {".".join(d.name.split(".")[:2]) for d in root.iterdir() if d.is_dir()}
    return sorted(versions, key=lambda v: [int(p) for p in v.split(".")])


def compile_case(vs: Path, toolset: str, arch: str, case: str, flags: str, workdir: Path) -> str:
    """One verdict: CRASH (the C1001), ok, or why the toolset says nothing about this bug."""
    vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    script = workdir / "build.bat"
    script.write_text(
        "@echo off\r\n"
        f'cd /d "{workdir}"\r\n'
        f'call "{vcvars}" {arch} -vcvars_ver={toolset} >nul 2>&1\r\n'
        "if errorlevel 1 exit /b 90\r\n"
        f'cl /nologo /std:c++20 {flags} /D{case} /c "{SOURCE}" /Fo:out.obj\r\n',
        encoding="ascii",
    )
    build = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True)
    output = build.stdout + build.stderr
    if build.returncode == 90:
        return "no-toolset"
    if "C1001" in output:
        return "CRASH"
    if build.returncode == 0:
        return "ok"
    # Toolsets without C++20 coroutines cannot say anything about this bug.
    if "D9002" in output or "C1083" in output or "C3861" in output or "C2039" in output:
        return "too-old"
    return "build-failed: " + next((line for line in output.splitlines() if "error" in line), "no output")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", default="x64", help="the vcvarsall architecture, x64 by default")
    args = parser.parse_args()

    if not SOURCE.is_file():
        print(f"missing {SOURCE}", file=sys.stderr)
        return 2

    vs = find_vs_install()
    if vs is None:
        print("no Visual Studio installation found (vswhere is missing)")
        return 2
    toolsets = find_msvc_toolsets(vs)
    print(f"Visual Studio: {vs}")
    print(f"toolsets:      {', '.join(toolsets) or '(none)'}\n")

    workdir = Path(tempfile.mkdtemp(prefix="msvc-await-init-"))
    try:
        results = [[compile_case(vs, toolset, args.arch, case, flags, workdir) for toolset in toolsets] for case, flags, _ in CASES]
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    headers = toolsets
    widths = [max(len(h), 10) for h in headers]
    print(f"{'case':<12}  {'flags':<10}  " + "  ".join(f"{h:<{w}}" for h, w in zip(headers, widths)) + "  isolates")
    for (case, flags, what), row in zip(CASES, results):
        print(f"{case:<12}  {flags:<10}  " + "  ".join(f"{v:<{w}}" for v, w in zip(row, widths)) + f"  {what}")

    control = shutil.which("clang++")
    if control:
        build = subprocess.run([control, "-std=c++20", "-DCRASH", "-c", str(SOURCE), "-o", os.devnull], capture_output=True, text=True)
        print(f"\nclang++ on CRASH: {'ok' if build.returncode == 0 else 'failed'}")

    crashed = sorted({headers[i] for row in results for i, verdict in enumerate(row) if verdict == "CRASH"})
    print()
    if crashed:
        print(f"reproduced on {', '.join(crashed)} - see readme.md")
        return 1
    print("not reproduced on any toolset available here")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
