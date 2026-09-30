#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Finds every name a program's struct or function may not take in MSL, and prints the block of reserved_words.cc.

Each identifier of the installed Metal toolchain's headers is declared once as a struct and once as a function in a
text that says `using namespace metal;`, as SGL's does, and a name either declaration fails for is reserved.
The names k_msl already holds above the generated block are left out, so the output replaces that block as it is.
Over-reserving costs an underscore and nothing else, so a new toolchain can only add names.
Exit 2 when no Metal compiler can be found.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
RESERVED_WORDS = REPO / "libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/reserved_words.cc"
# The first line of the generated block; everything of k_msl above it is written by hand.
BLOCK_MARKER = "// Everything else the Metal toolchain declares"

IDENTIFIER = re.compile(r"\b[A-Za-z][A-Za-z0-9_]*\b")

# The probe's own names carry a prefix no header uses, so a candidate never collides with the probe itself.
STRUCT_PROBE = """#include <metal_stdlib>
using namespace metal;
struct {name} {{ float sgl_probe_a; }};
static {name} sgl_probe_pass({name} sgl_probe_x) {{ return sgl_probe_x; }}
kernel void sgl_probe_k(device {name}* sgl_probe_p [[buffer(0)]]) {{ sgl_probe_p[0] = sgl_probe_pass(sgl_probe_p[0]); }}
"""
FUNCTION_PROBE = """#include <metal_stdlib>
using namespace metal;
static float {name}(float sgl_probe_x) {{ return sgl_probe_x * 2.0f; }}
kernel void sgl_probe_k(device float* sgl_probe_p [[buffer(0)]]) {{ sgl_probe_p[0] = {name}(sgl_probe_p[0]); }}
"""


def xcrun(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["xcrun", *args], capture_output=True, text=True)


def toolchain() -> tuple[str, Path]:
    """The compiler's version line and the directory of its `metal_stdlib` headers."""
    found = xcrun("-f", "metal")
    if found.returncode != 0:
        raise SystemExit(2)
    version = xcrun("metal", "--version").stdout.splitlines()[0]
    # The driver sits in usr/bin, and the headers under usr/metal/<n>/lib/clang/<version>/include/metal.
    root = Path(os.path.realpath(found.stdout.strip())).parents[1]
    headers = sorted(root.glob("metal/*/lib/clang/*/include/metal"))
    if not headers:
        raise SystemExit(f"no metal headers under {root}")
    return version, headers[-1]


def candidates(headers: Path) -> list[str]:
    names = set()
    for path in headers.rglob("*"):
        if path.is_file():
            names.update(IDENTIFIER.findall(path.read_text(encoding="utf-8", errors="replace")))
    return sorted(names)


def compiles(text: str) -> bool:
    result = subprocess.run(["xcrun", "metal", "-c", "-o", os.devnull, "-x", "metal", "-"],
                            input=text, capture_output=True, text=True)
    return result.returncode == 0


def is_taken(name: str) -> bool:
    return not compiles(STRUCT_PROBE.format(name=name)) or not compiles(FUNCTION_PROBE.format(name=name))


def hand_written() -> set[str]:
    """The names k_msl holds above the generated block."""
    text = RESERVED_WORDS.read_text(encoding="utf-8")
    start = text.index("constexpr cc::string_view k_msl[] = {")
    end = text.index(BLOCK_MARKER, start)
    return set(re.findall(r'"([^"]+)"', text[start:end]))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    version, headers = toolchain()
    names = candidates(headers)
    print(f"probing {len(names)} identifiers against {version}", file=sys.stderr)

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        taken = [n for n, t in zip(names, pool.map(is_taken, names)) if t]
    # A compiler that is starved of resources fails now and then, so a name counts only when it fails again alone.
    taken = [n for n in taken if is_taken(n)]

    known = hand_written()
    fresh = [n for n in taken if n not in known]
    lower = [n for n in fresh if not n[0].isupper()]
    upper = [n for n in fresh if n[0].isupper()]

    print(f"    {BLOCK_MARKER} where the program's structs are: at global scope, or in `metal`.")
    print(f"    // Taken from {version} by tools/msl-probe/run.py, which regenerates this block.")
    for n in lower:
        print(f'    "{n}",')
    print()
    print("    // the macros of those headers, which would replace a name of the program before the compiler reads it")
    for n in upper:
        print(f'    "{n}",')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
