#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the MSVC const-reference miscompilation reproduction.

Standalone: it does not import anything from shaped-core and does not use dev.py.
It finds the compilers itself, builds repro.cc under each, runs it, and prints a table.

    uv run run.py              # every toolset it can find, at /O2 and /Od
    uv run run.py --keep       # leave the binaries behind for disassembly
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


def find_vs_install() -> Path | None:
    """The newest Visual Studio installation, via vswhere."""
    if not VSWHERE.is_file():
        return None
    out = subprocess.run(
        [str(VSWHERE), "-latest", "-products", "*", "-property", "installationPath"],
        capture_output=True, text=True,
    )
    path = out.stdout.strip()
    return Path(path) if path else None


def find_msvc_toolsets(vs: Path) -> list[str]:
    """Every MSVC toolset version installed under this VS, newest first."""
    root = vs / "VC" / "Tools" / "MSVC"
    if not root.is_dir():
        return []
    # 14.51.36231 -> the 14.51 that -vcvars_ver wants
    versions = {".".join(d.name.split(".")[:2]) for d in root.iterdir() if d.is_dir()}
    return sorted(versions, key=lambda v: [int(p) for p in v.split(".")], reverse=True)


def run_cl(vs: Path, toolset: str, opt: str, workdir: Path, keep: bool) -> tuple[str, str]:
    """Compile and run repro.cc under one toolset at one optimization level.

    Returns (verdict, detail). The verdict is one of ok / MISCOMPILED / build-failed / crashed.
    """
    exe = workdir / f"repro_{toolset.replace('.', '_')}_{opt.lstrip('/')}.exe"
    vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"

    # One .bat, because quoting vcvarsall through a shell is its own small nightmare and this has to be readable.
    # It cds into the work directory first, so /Fo and /Fe take bare relative names — a quoted path ending in a
    # backslash would escape its own closing quote, which cl reports as an unopenable .obj rather than a quoting error.
    script = workdir / f"build_{toolset.replace('.', '_')}_{opt.lstrip('/')}.bat"
    script.write_text(
        "@echo off\r\n"
        f'cd /d "{workdir}"\r\n'
        f'call "{vcvars}" x64 -vcvars_ver={toolset} >nul 2>&1\r\n'
        "if errorlevel 1 exit /b 90\r\n"
        f'cl /nologo {opt} /std:c++20 /EHsc "{SOURCE}" /Fe:"{exe.name}" /Fo:"{exe.stem}.obj"\r\n',
        encoding="ascii",
    )

    build = subprocess.run(["cmd", "/c", str(script)], capture_output=True, text=True)
    output = (build.stdout + build.stderr).strip()
    if build.returncode == 90:
        return "no-toolset", f"vcvarsall could not select {toolset}"
    if "D9002" in output and "/std:c++20" in output:
        # Toolsets older than 14.28 have no /std:c++20 at all, so they cannot say anything about this bug.
        return "too-old", "no /std:c++20 in this toolset"
    if build.returncode != 0 or not exe.is_file():
        return "build-failed", (output.splitlines() or ["no output"])[-1]

    try:
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "hung", "the repro did not terminate in 60 s"
    finally:
        if not keep:
            exe.unlink(missing_ok=True)

    observed = run.stdout.strip().splitlines()
    detail = observed[0] if observed else f"no output (exit {run.returncode})"
    if run.returncode == 0:
        return "ok", detail
    if run.returncode == 1:
        return "MISCOMPILED", detail
    return "crashed", f"exit {run.returncode}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the built binaries for disassembly")
    args = parser.parse_args()

    if not SOURCE.is_file():
        print(f"missing {SOURCE}", file=sys.stderr)
        return 2

    rows: list[tuple[str, str, str, str]] = []

    vs = find_vs_install()
    if vs is None:
        print("no Visual Studio installation found (vswhere is missing) — skipping the MSVC legs")
    else:
        print(f"Visual Studio: {vs}")
        toolsets = find_msvc_toolsets(vs)
        print(f"toolsets:      {', '.join(toolsets) or '(none)'}\n")

        workdir = Path(tempfile.mkdtemp(prefix="msvc-constref-"))
        try:
            for toolset in toolsets:
                for opt in ("/O2", "/Od"):
                    verdict, detail = run_cl(vs, toolset, opt, workdir, args.keep)
                    rows.append((f"cl {toolset}", opt, verdict, detail))
                    if args.keep:
                        print(f"  binaries kept in {workdir}")
        finally:
            if not args.keep:
                shutil.rmtree(workdir, ignore_errors=True)

    # The control: any non-MSVC compiler on PATH should be correct at every level.
    for name in ("clang-cl", "clang++", "g++"):
        exe_path = shutil.which(name)
        if exe_path is None:
            continue
        workdir = Path(tempfile.mkdtemp(prefix="msvc-constref-alt-"))
        try:
            for opt in ("-O2", "-O0"):
                out = workdir / "repro.exe"
                flags = ["/O2" if opt == "-O2" else "/Od", "/std:c++20", "/EHsc", str(SOURCE), f"/Fe:{out}", f"/Fo:{workdir}\\"] \
                    if name == "clang-cl" else [opt, "-std=c++20", str(SOURCE), "-o", str(out)]
                build = subprocess.run([exe_path, *flags], capture_output=True, text=True)
                if build.returncode != 0:
                    rows.append((name, opt, "build-failed", (build.stderr.strip().splitlines() or ["no output"])[-1]))
                    continue
                run = subprocess.run([str(out)], capture_output=True, text=True, timeout=60)
                observed = run.stdout.strip().splitlines()
                rows.append((name, opt, "ok" if run.returncode == 0 else "MISCOMPILED",
                             observed[0] if observed else f"exit {run.returncode}"))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    width = max((len(r[0]) for r in rows), default=10)
    print(f"{'compiler':<{width}}  {'opt':<4}  {'verdict':<12}  detail")
    print(f"{'-' * width}  ----  ------------  ------")
    for compiler, opt, verdict, detail in rows:
        print(f"{compiler:<{width}}  {opt:<4}  {verdict:<12}  {detail}")

    miscompiled = [r for r in rows if r[2] == "MISCOMPILED"]
    print()
    if miscompiled:
        print(f"reproduced on {len(miscompiled)} configuration(s) - see readme.md")
        return 1
    print("not reproduced on any configuration available here")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
