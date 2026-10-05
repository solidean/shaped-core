#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Fail a link whose map shows a dispatched kernel's TU supplying code shared with the floor.

cimd_dispatch compiles each kernel above the build's floor in a TU of its own, `<name>-<kernel>.cc`.
That TU takes the kernel's -march, and emits every inline function it uses compiled with the kernel's instructions.
Those copies carry the same names the floor's copies have, and the linker keeps one copy program-wide.
The kernel objects sit in a static library behind the target, so the floor's copy usually wins.
This is the check that it did.
A code symbol the map attributes to a kernel object must carry that object's own kernel type in its mangled name, or be the dispatch entry.
For `q-avx512.cc` that type is `cimd::avx512`.

    uv run check-link-map.py --map app.map --kernel avx2 --kernel avx512

Kernel objects are recognized by that naming, so every cimd_dispatch in the build is covered without a list.
A map naming no kernel object at all fails too: every one contributes its cimd_entry_*, so none means nothing was read.

Reads MSVC / lld-link `/MAP` files and ELF `-Map` files from GNU ld and lld.
Only code is checked: a constant pooled from a kernel TU (`__real@…`) holds the same bytes whichever copy is kept.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The prefixes GCC and clang put between `.text.` and an inline function's name under -ffunction-sections.
_ELF_TEXT = re.compile(r"\.text\.(?:(?:unlikely|hot|startup|exit|split|unknown)\.)?(_Z[^\s)]+)")


def has_kernel_tag(symbol: str, kernel: str) -> bool:
    """Whether a mangled name names `cimd::<kernel>`: MSVC's `U<k>@cimd@@`, or Itanium's `<len><k>E` after `4cimd`.

    Itanium compresses a repeated prefix, so once `cimd` has appeared the tag is spelled `NS0_6avx512E` instead.
    """
    if f"U{kernel}@cimd@@" in symbol:
        return True
    return re.search(rf"(?:4cimd|S[0-9A-Z]*_){len(kernel)}{kernel}E", symbol) is not None


def object_kernel(origin: str, kernels: list[str]) -> str | None:
    """The kernel of the generated `<name>-<kernel>.cc` object `origin` names, or None for any other object."""
    alternatives = "|".join(re.escape(k) for k in kernels)
    m = re.search(rf"(?:^|[\s:(/\\])\w+-({alternatives})\.cc\.o(?:bj)?\b", origin)
    return m.group(1) if m else None


def coff_findings(text: str) -> list[tuple[str, str]]:
    """MSVC / lld-link: a section table naming each section's class, then ` SSSS:OOOOOOOO  symbol  address [f] lib:obj`."""
    code_sections = set()
    for m in re.finditer(r"^\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+[0-9a-fA-F]+H\s+\S+\s+CODE\s*$", text, re.M):
        code_sections.add(m.group(1))
    out = []
    for m in re.finditer(r"^\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+(\S+)\s+[0-9a-fA-F]{16}\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$",
                         text, re.M):
        if m.group(1) in code_sections:
            out.append((m.group(2), m.group(3)))
    return out


def elf_lines(text: str) -> list[str]:
    """The map's lines, with GNU ld's wrapped input sections joined back into one.

    BFD puts a section name of 15 characters or more on a line of its own, and its address, size and object on the next.
    """
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if (re.fullmatch(r"\s*\.\S+\s*", line) and i + 1 < len(lines)
                and re.match(r"\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+\S", lines[i + 1])):
            line = f"{line.rstrip()} {lines[i + 1].strip()}"
            i += 1
        out.append(line)
        i += 1
    return out


def elf_findings(text: str) -> list[tuple[str, str]]:
    """GNU ld and lld: an input section `.text.<mangled>` (an inline function's COMDAT) next to the object it came from."""
    out = []
    for line in elf_lines(text):
        m = _ELF_TEXT.search(line)
        if m:
            out.append((m.group(1), line.strip()))
    return out


def is_coff(text: str) -> bool:
    return re.search(r"^\s*0001:", text, re.M) is not None


def kernel_objects_seen(text: str, kernels: list[str]) -> bool:
    return any(object_kernel(line, kernels) for line in text.splitlines())


def violations(text: str, kernels: list[str]) -> list[tuple[str, str]]:
    """Every code symbol a kernel object supplied without its own kernel's tag, except the dispatch entries."""
    findings = coff_findings(text) if is_coff(text) else elf_findings(text)
    out = []
    for symbol, origin in findings:
        kernel = object_kernel(origin, kernels)
        if kernel is not None and not has_kernel_tag(symbol, kernel) and "cimd_entry_" not in symbol:
            out.append((symbol, origin))
    return out


# A real ld.lld map, clang 22 --target=x86_64-linux-gnu -fuse-ld=lld, linking `main.cc.o libk.a libcore.a`.
# libk.a holds the avx512 kernel TU, and libcore.a a floor TU emitting the same shared_helper: ld.lld pulled libk.a
# first, for the pending cimd_entry_q_avx512, and kept its copy.
_SELF_TEST_LLD = """
             VMA              LMA     Size Align Out     In      Symbol
          201230           201230       7a    16 .text
          201230           201230       15    16         main.cc.o:(.text)
          201230           201230       15     1                 _start
          201248           201248        0     4         libk.a(q-avx512.cc.o):(.text)
          201250           201250       28    16         libk.a(q-avx512.cc.o):(.text._Z5queryIN4cimd6avx512EEiv)
          201250           201250       28     1                 int query<cimd::avx512>()
          201280           201280        4    16         libk.a(q-avx512.cc.o):(.text._Z13shared_helperi)
          201280           201280        4     1                 shared_helper(int)
          201290           201290        5    16         libk.a(q-avx512.cc.o):(.text._Z3putIN4cimd4simdIfLi4ENS0_6avx512EEEEiRKT_)
          201290           201290        5     1                 int put<cimd::simd<float, 4, cimd::avx512>>(cimd::simd<float, 4, cimd::avx512> const&)
          2012a0           2012a0        a    16         libcore.a(core.cc.o):(.text)
          2012a0           2012a0        a     1                 core_fn()
          2022b0           2022b0        8     8 .data.rel.ro
          2022b0           2022b0        8     8         libk.a(q-avx512.cc.o):(.data.rel.ro)
          2022b0           2022b0        8     1                 cimd_entry_q_avx512
"""

# The same toolchain, with the shared helper `[[gnu::cold]]`: clang puts it in `.text.unlikely.`.
_SELF_TEST_LLD_UNLIKELY = """
             VMA              LMA     Size Align Out     In      Symbol
          201210           201210       35    16 .text
          201210           201210       1f    16         main2.o:(.text)
          201230           201230        b    16         libk2.a(q2-avx512.cc.o):(.text._Z5queryIN4cimd6avx512EEii)
          201230           201230        b     1                 int query<cimd::avx512>(int)
          20123b           20123b        4     1         libk2.a(q2-avx512.cc.o):(.text.unlikely._Z11cold_helperi)
          20123b           20123b        4     1                 cold_helper(int)
          201240           201240        5    16         libcold.a(cold.o):(.text.unlikely.)
          201240           201240        5     1                 use_cold(int)
          202248           202248        8     8         libk2.a(q2-avx512.cc.o):(.data.rel.ro)
"""

# GNU ld (BFD)'s layout: a long section name on a line of its own, its address, size and object on the next.
_SELF_TEST_BFD = """
 .text          0x0000000000401000       0x15 main.cc.o
                0x0000000000401000                _start
 .text._Z5queryIN4cimd6avx512EEiv
                0x0000000000401020       0x28 libapp-cimd-q.a(q-avx512.cc.o)
                0x0000000000401020                int query<cimd::avx512>()
 .text._Z13shared_helperi
                0x0000000000401050        0x4 libapp-cimd-q.a(q-avx512.cc.o)
                0x0000000000401050                shared_helper(int)
 .text._Z14shared_helper2i
                0x0000000000401060        0x4 core.cc.o
"""

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

# A K-templated function, but the avx2 kernel's, supplied by the avx512 object: some other kernel's tag is not enough.
_SELF_TEST_WRONG_TAG = """
          201250           201250       28    16         libk.a(q-avx512.cc.o):(.text._Z5queryIN4cimd4avx2EEiv)
          201280           201280       28    16         libk.a(q-avx2.cc.o):(.text._Z5queryIN4cimd4avx2EEiv)
"""

# A map from an executable that links no kernel object, or one this parser misread.
_SELF_TEST_NO_OBJECT = """
          201230           201230       15    16         main.cc.o:(.text)
          2012a0           2012a0        a    16         libcore.a(core.cc.o):(.text._Z13shared_helperi)
"""


def check(text: str, kernels: list[str]) -> tuple[bool, list[tuple[str, str]]]:
    """Whether any kernel object was seen, and the violations."""
    return kernel_objects_seen(text, kernels), violations(text, kernels)


def self_test() -> int:
    kernels = ["avx2", "avx512"]
    cases = (
        ("lld", _SELF_TEST_LLD, True, ["_Z13shared_helperi"]),
        ("lld .text.unlikely", _SELF_TEST_LLD_UNLIKELY, True, ["_Z11cold_helperi"]),
        ("bfd wrapped", _SELF_TEST_BFD, True, ["_Z13shared_helperi"]),
        ("coff", _SELF_TEST_COFF, True, ["?shared_helper@@YAHXZ"]),
        ("wrong kernel tag", _SELF_TEST_WRONG_TAG, True, ["_Z5queryIN4cimd4avx2EEiv"]),
        ("no kernel object", _SELF_TEST_NO_OBJECT, False, []),
    )
    for name, text, expected_seen, expected in cases:
        seen, bad = check(text, kernels)
        got = [s for s, _ in bad]
        if seen != expected_seen or got != expected:
            print(f"check-link-map self-test ({name}): expected {expected_seen} {expected}, got {seen} {got}")
            return 1
    print("check-link-map self-test: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", type=Path)
    parser.add_argument("--kernel", action="append", default=[], help="a dispatched kernel, e.g. avx512")
    parser.add_argument("--self-test", action="store_true", help="check the parsers against built-in maps")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.map is None:
        parser.error("--map is required")
    if not args.kernel:
        parser.error("at least one --kernel is required")

    text = args.map.read_text(encoding="utf-8", errors="replace")
    seen, bad = check(text, args.kernel)
    if not seen:
        print(f"check-link-map: {args.map.name}: names no kernel object (`<name>-<kernel>.cc`) at all;")
        print("  either this executable links no cimd_dispatch, or the map's layout is one this parser cannot read.")
        return 1
    if not bad:
        return 0

    print(f"check-link-map: {args.map.name}: the linker kept a dispatched kernel's copy of code the floor shares;")
    print("  every CPU that reaches it runs that kernel's instructions. Kernel TUs must instantiate only K-templated code.")
    for symbol, origin in bad:
        print(f"  {symbol}  <-  {origin}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
