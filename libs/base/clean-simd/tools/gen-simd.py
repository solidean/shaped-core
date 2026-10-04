#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Generate clean-simd's register layer per kernel and its fixed layer per element.

Run it as an explicit step after changing a kernel table or an emitter, never from the build:

    uv run libs/base/clean-simd/tools/gen-simd.py --write
    uv run libs/base/clean-simd/tools/gen-simd.py --check    # exit 2 when a committed file differs

`dev.py check` runs --check (and --write under --fix), so a hand edit to a generated file or a table change nobody
re-ran is caught before commit.
The output is clang-formatted here, so `dev.py format` leaves it alone.

The kernel tables live in gen_simd/ beside this script, one module per instruction-set family.
Each operation carries a cost class, and an operator exists only where every reference kernel (AVX2, NEON, SIMD128)
implements it as one instruction or a short lane-wise sequence; libs/base/clean-simd/docs/design.md has the rule.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from gen_simd.emit import COMMAND, emit_aliases, emit_fixed, emit_fixed_masks, emit_kernel, emit_op_matrix  # noqa: E402
from gen_simd.model import ELEMS, KERNELS  # noqa: E402

LIB = HERE.parent
REPO = LIB.parents[2]
GENERATED = LIB / "src" / "clean-simd" / "generated"


def find_clang_format(explicit: str | None) -> str:
    if explicit:
        return explicit
    pinned = REPO / "tools" / "bin" / ("clang-format.exe" if os.name == "nt" else "clang-format")
    if pinned.exists():
        return str(pinned)
    found = shutil.which("clang-format")
    if found:
        return found
    sys.exit("gen-simd: no clang-format found; pass --clang-format PATH")


def clang_format(text: str, assume: Path, exe: str) -> str:
    result = subprocess.run(
        [exe, f"--assume-filename={assume}", f"--style=file:{REPO / '.clang-format'}"],
        input=text.encode("utf-8"),
        capture_output=True,
        check=True,
    )
    return result.stdout.decode("utf-8").replace("\r\n", "\n")


def generate(exe: str) -> dict[Path, str]:
    texts: dict[Path, str] = {}
    for k in KERNELS:
        texts[GENERATED / f"{k.name}.hh"] = emit_kernel(k)
    texts[GENERATED / "aliases.hh"] = emit_aliases()
    texts[GENERATED / "fixed" / "masks.hh"] = emit_fixed_masks()
    for e in ELEMS:
        texts[GENERATED / "fixed" / f"{e.name}.hh"] = emit_fixed(e)
    out = {path: clang_format(text, path, exe) for path, text in texts.items()}
    # Markdown is not C++, so it skips clang-format.
    out[LIB / "docs" / "op-matrix.md"] = emit_op_matrix()
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="write the generated files")
    mode.add_argument("--check", action="store_true", help="exit 2 when a committed file differs from the output")
    parser.add_argument("--clang-format", help="the clang-format to format the output with")
    args = parser.parse_args()

    files = generate(find_clang_format(args.clang_format))
    stale = sorted(p for p in GENERATED.rglob("*.hh") if p not in files)

    # Line endings are the checkout's business, so a file differs only in its content.
    differs = [p for p, text in files.items() if not p.exists() or p.read_bytes().decode("utf-8").replace("\r\n", "\n") != text]

    if args.write:
        # Only what changed is written, so a no-op --fix touches no file and triggers no rebuild.
        for path in differs:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(files[path].encode("utf-8"))
        for path in stale:
            path.unlink()
        print(f"gen-simd: wrote {len(differs)} of {len(files)} files, removed {len(stale)}")
        return 0

    differs += stale
    for p in differs:
        print(f"gen-simd: {p.relative_to(REPO).as_posix()} differs from the generator's output")
    if differs:
        print(f"gen-simd: run `{COMMAND}`")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
