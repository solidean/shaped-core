#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Fail a link whose map shows a dispatched kernel's TU supplying code shared with the floor.

cimd_dispatch compiles each kernel above the build's floor in a TU of its own, with that kernel's -march.
Every inline function such a TU uses is emitted there too, compiled with the kernel's instructions, under the same name
the floor's copy has; the linker keeps one copy program-wide.
The kernel objects sit in a static library behind the target, so normally the floor's copy wins.
This is the check that it did: a code symbol the map attributes to a kernel object must carry that kernel's type in its
mangled name (`cimd::avx512`), or be the dispatch entry itself.

    uv run check-link-map.py --map app.map --kernel avx512 --object cimd_battery-avx512.cc

Reads MSVC / lld-link `/MAP` files and ELF `-Map` files from GNU ld and lld.
Only code is checked: a constant pooled from a kernel TU (`__real@…`) holds the same bytes whichever copy is kept.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def kernel_tags(kernel: str) -> tuple[str, ...]:
    """How a mangled name spells `cimd::<kernel>`: MSVC's `U<k>@cimd@@` and Itanium's `4cimd<len><k>E`."""
    return (f"U{kernel}@cimd@@", f"4cimd{len(kernel)}{kernel}E")


def is_kernel_object(origin: str, objects: list[str]) -> bool:
    return any(o in origin for o in objects)


def coff_findings(text: str, objects: list[str]) -> list[tuple[str, str]]:
    """MSVC / lld-link: a section table naming each section's class, then ` SSSS:OOOOOOOO  symbol  address [f] lib:obj`."""
    code_sections = set()
    for m in re.finditer(r"^\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+[0-9a-fA-F]+H\s+\S+\s+CODE\s*$", text, re.M):
        code_sections.add(m.group(1))
    out = []
    for m in re.finditer(r"^\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+(\S+)\s+[0-9a-fA-F]{16}\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$",
                         text, re.M):
        section, symbol, origin = m.group(1), m.group(2), m.group(3)
        if section in code_sections and is_kernel_object(origin, objects):
            out.append((symbol, origin))
    return out


def elf_findings(text: str, objects: list[str]) -> list[tuple[str, str]]:
    """GNU ld and lld: an input section `.text.<mangled>` (an inline function's COMDAT) next to the object it came from."""
    out = []
    for line in text.splitlines():
        m = re.search(r"\.text\.(_Z\S+?)[\s)]", line + " ")
        if m and is_kernel_object(line, objects):
            out.append((m.group(1), line.strip()))
    return out


def violations(text: str, kernels: list[str], objects: list[str]) -> list[tuple[str, str]]:
    findings = coff_findings(text, objects) if re.search(r"^\s*0001:", text, re.M) else elf_findings(text, objects)
    tags = [t for k in kernels for t in kernel_tags(k)]
    return [(s, o) for s, o in findings if not any(t in s for t in tags) and "cimd_entry_" not in s]


# One kept copy of a K-templated function (fine) and one of a shared inline helper (the finding), per map format.
_SELF_TEST_COFF = """
 Start         Length     Name                   Class
 0001:00000000 00234d38H .text                   CODE
 0002:00000000 00012000H .rdata                  DATA

  Address         Publics by Value              Rva+Base               Lib:Object
 0001:00127ba0       ??$query@Uavx512@cimd@@@@YAHXZ 0000000140128ba0 f   app-cimd-q:q-avx512.cc.obj
 0001:00127c30       ?shared_helper@@YAHXZ 0000000140128c30 f   app-cimd-q:q-avx512.cc.obj
 0001:00127d00       ?shared_helper2@@YAHXZ 0000000140128d00 f   floor.cc.obj
 0002:00012148       __real@7fffffff7fffffff    000000014028a148     app-cimd-q:q-avx512.cc.obj
"""
_SELF_TEST_ELF = """
 .text._Z5queryIN4cimd6avx512EEiv  0x0000000000401000  0x20 libapp-cimd-q.a(q-avx512.cc.o)
 .text._Z13shared_helperv  0x0000000000401020  0x10 libapp-cimd-q.a(q-avx512.cc.o)
 .text._Z14shared_helper2v  0x0000000000401030  0x10 floor.cc.o
"""


def self_test() -> int:
    for name, text, expected in (("coff", _SELF_TEST_COFF, "?shared_helper@@YAHXZ"),
                                 ("elf", _SELF_TEST_ELF, "_Z13shared_helperv")):
        got = [s for s, _ in violations(text, ["avx512"], ["q-avx512.cc"])]
        if got != [expected]:
            print(f"check-link-map self-test ({name}): expected [{expected}], got {got}")
            return 1
    print("check-link-map self-test: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", type=Path)
    parser.add_argument("--kernel", action="append", default=[], help="a dispatched kernel, e.g. avx512")
    parser.add_argument("--object", action="append", default=[], help="a kernel TU's source name, e.g. x-avx512.cc")
    parser.add_argument("--self-test", action="store_true", help="check the parsers against built-in maps")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.map is None:
        parser.error("--map is required")

    text = args.map.read_text(encoding="utf-8", errors="replace")
    bad = violations(text, args.kernel, [Path(o).name for o in args.object])
    if not bad:
        return 0

    print(f"check-link-map: {args.map.name}: the linker kept a dispatched kernel's copy of code the floor shares;")
    print("  every CPU that reaches it runs that kernel's instructions. Kernel TUs must instantiate only K-templated code.")
    for symbol, origin in bad:
        print(f"  {symbol}  <-  {origin}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
