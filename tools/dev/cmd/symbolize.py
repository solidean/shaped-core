"""`symbolize` — turn wasm module offsets back into function names and source lines.

A wasm stack is captured as byte offsets into the module file, not into its code section (see
`libs/base/clean-core/src/clean-core/platform/impl/wasm_frames.hh`), and a build that shipped no name section reports
nothing else.
Those offsets are still resolvable — against the build's own debug info, after the fact, by a tool rather than by the
program.

This is that tool.
It reads offsets from text and resolves each through the emsdk's `llvm-symbolizer`, which takes a wasm module offset
directly and answers with `function` plus `file:line:column`.

The intended shape is a pipe, because the text it reads is what clean-core already prints:

    uv run dev.py test ... 2> report.txt
    uv run dev.py symbolize --obj build/wasm-emscripten-release/.../foo.debug.wasm report.txt

**Nothing here checks that the offsets and the binary came from the same build**, and that is worth knowing rather
than assuming: resolving a stack against the wrong build of the same source produces plausible, confident, wrong
names, and neither this nor llvm-symbolizer can tell.
Until a build identity is recorded alongside a capture, keeping the sidecar beside the artifact it was built with is
the only thing protecting you.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from tools.dev.lib.core import process

from .context import Context

NAME = "symbolize"

# What a wasm frame looks like in the two places clean-core writes one, plus a bare offset on a line of its own.
#
# `wasm+0x1234` is cc::stacktrace's rendering; `wasm-function[7]:0x1234` is the engine's own frame text, which turns
# up in anything that captured before the parser saw it.
_FRAME_PATTERNS = [
    re.compile(r"wasm-function\[\d+\]:(0x[0-9a-fA-F]+)"),
    re.compile(r"wasm\+(0x[0-9a-fA-F]+)"),
    re.compile(r"^\s*(0x[0-9a-fA-F]+)\s*$"),
]

# A JS frame's "address" is a line number with the top bit set, and resolving one against the module would be
# nonsense — see cc::impl::wasm_js_frame_bit.
_JS_FRAME_BIT = 0x8000_0000


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Resolve wasm module offsets to names and source lines")
    p.add_argument(
        "input",
        nargs="?",
        help="File holding the offsets, or a report containing them; reads stdin when omitted",
    )
    p.add_argument(
        "--obj",
        required=True,
        metavar="PATH",
        help="The wasm binary or its separate DWARF file (-gseparate-dwarf) to resolve against",
    )
    p.add_argument(
        "--offsets",
        nargs="*",
        default=None,
        metavar="OFF",
        help="Resolve these offsets instead of reading any input",
    )
    p.add_argument(
        "--emsdk-path",
        default=None,
        metavar="PATH",
        help="An emsdk install to take llvm-symbolizer from; falls back to SC_EMSDK_PATH, EMSDK, then PATH",
    )
    return p


def _find_symbolizer(emsdk_path: str | None) -> Path | None:
    """llvm-symbolizer from the emsdk, else whatever is on PATH.

    The emsdk's own copy is preferred because it is the one that matches the toolchain that produced the DWARF.
    """
    root = process.find_emsdk_root(emsdk_path)
    if root is not None:
        for name in ("llvm-symbolizer.exe", "llvm-symbolizer"):
            candidate = root / "upstream" / "bin" / name
            if candidate.is_file():
                return candidate

    on_path = shutil.which("llvm-symbolizer")
    return Path(on_path) if on_path else None


def _offsets_in(text: str) -> list[int]:
    """Every wasm module offset the text mentions, in the order it mentions them.

    Order matters: this is a stack, and printing it back out of order would be worse than not printing it.
    Duplicates are kept for the same reason — a recursive call site appears once per level, and collapsing it would
    lose the depth.
    """
    found: list[int] = []
    for line in text.splitlines():
        for pattern in _FRAME_PATTERNS:
            m = pattern.search(line)
            if m:
                found.append(int(m.group(1), 16))
                break
    return found


def run(args: argparse.Namespace, ctx: Context) -> None:
    obj = Path(args.obj)
    if not obj.is_file():
        print(f"symbolize: no such file: {obj}", file=sys.stderr)
        raise SystemExit(2)

    symbolizer = _find_symbolizer(args.emsdk_path)
    if symbolizer is None:
        print(
            "symbolize: no llvm-symbolizer found; pass --emsdk-path, set SC_EMSDK_PATH, or put it on PATH",
            file=sys.stderr,
        )
        raise SystemExit(2)

    if args.offsets:
        offsets = [int(o, 16) if o.lower().startswith("0x") else int(o) for o in args.offsets]
    else:
        text = Path(args.input).read_text(encoding="utf-8", errors="replace") if args.input else sys.stdin.read()
        offsets = _offsets_in(text)

    if not offsets:
        print("symbolize: no wasm module offsets found in the input", file=sys.stderr)
        raise SystemExit(1)

    # One process for the whole stack rather than one per frame: llvm-symbolizer reads addresses from stdin and
    # answers in order, separated by blank lines, which is what makes a deep stack cheap to resolve.
    query = "".join(f"{off:#x}\n" for off in offsets if not off & _JS_FRAME_BIT)
    skipped = sum(1 for off in offsets if off & _JS_FRAME_BIT)

    completed = subprocess.run(
        [str(symbolizer), f"--obj={obj}", "--demangle", "--functions=linkage", "--inlining"],
        input=query,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        print(f"symbolize: llvm-symbolizer failed: {completed.stderr.strip()}", file=sys.stderr)
        raise SystemExit(completed.returncode)

    # Answers are separated by a blank line, one block per address, in the order they were asked.
    blocks = [b for b in completed.stdout.split("\n\n") if b.strip()]
    resolvable = [off for off in offsets if not off & _JS_FRAME_BIT]

    for off, block in zip(resolvable, blocks):
        lines = [ln for ln in block.splitlines() if ln.strip()]
        head = lines[0] if lines else "??"
        where = lines[1] if len(lines) > 1 else ""
        print(f"{off:#010x}  {head}" + (f"  ({where})" if where else ""))

    if len(blocks) < len(resolvable):
        print(
            f"symbolize: {len(resolvable) - len(blocks)} offset(s) went unanswered, which usually means the binary "
            f"carries no debug info for them",
            file=sys.stderr,
        )

    if skipped:
        print(f"symbolize: skipped {skipped} JS frame(s), which name a script line rather than a place in the module")
