#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Runs repro.cjs under deno twice per case, rewriting it in between, and says whether deno reused a stale code cache.

Each case writes the script padded to a given size, runs it once to fill deno's V8 code cache, then swaps its two functions in place.
The swap keeps the byte length, so only the content changes.
A stale cache shows as a SyntaxError naming a fragment of an identifier, because a lazy function is parsed from its old offset.

Every case runs against its own empty DENO_DIR, so the verdict does not depend on what this machine cached before.
Exit 1 when the bug reproduces, 0 when it does not, and 2 when deno could not be run.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPRO = HERE / "repro.cjs"

SHORT = "function short_one(index) {\n  return index + 1;\n}\n"
LONG = "function a_longer_one(index, a1, a2) {\n  return index + a1 + a2;\n}\n"
EXPECTED = "result 8"


@dataclass(frozen=True)
class Case:
    label: str
    suffix: str
    size: int
    flags: tuple[str, ...] = ()
    same_path: bool = True
    # Whether a stale verdict here is the bug, rather than a control that must stay clean.
    is_bug: bool = True


CASES = [
    Case(".cjs, 16384 bytes", ".cjs", 16384),
    Case(".js + --unstable-detect-cjs, 16384 bytes (how dev.py ran it)", ".js", 16384, ("--unstable-detect-cjs",)),
    Case(".cjs, 16383 bytes (control: below the size that matters)", ".cjs", 16383, is_bug=False),
    Case(".cjs + --no-code-cache, 16384 bytes (the workaround)", ".cjs", 16384, ("--no-code-cache",), is_bug=False),
    Case(".cjs, 16384 bytes, second run at a new path (control)", ".cjs", 16384, same_path=False, is_bug=False),
    Case(".mjs, 16384 bytes (control: an ES module)", ".mjs", 16384, is_bug=False),
]


def layout(size: int, swapped: bool) -> str:
    """repro.cjs, functions optionally swapped, padded to exactly `size` bytes by a leading comment."""
    src = REPRO.read_text(encoding="utf-8")
    assert SHORT in src and LONG in src, "repro.cjs no longer holds the two functions run.py swaps"
    if swapped:
        src = src.replace(SHORT, "\0").replace(LONG, SHORT).replace("\0", LONG)
    pad = size - len(src.encode("utf-8")) - len("//\n")
    assert pad >= 0, f"repro.cjs is already longer than {size} bytes"
    return "//" + "x" * pad + "\n" + src


def run_once(deno: str, script: Path, flags: tuple[str, ...], deno_dir: Path) -> str:
    env = dict(os.environ, DENO_DIR=str(deno_dir), NO_COLOR="1")
    try:
        p = subprocess.run([deno, "run", "--allow-all", *flags, str(script)],
                           capture_output=True, text=True, env=env, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"could not run: {e}"
    return ((p.stdout or "") + (p.stderr or "")).strip()


def run_case(deno: str, case: Case, work: Path) -> tuple[str, str]:
    """('stale' | 'clean' | 'skip', the second run's output)."""
    deno_dir = work / "deno-dir"
    first = work / f"loader{case.suffix}"
    first.write_bytes(layout(case.size, swapped=False).encode("utf-8"))
    out = run_once(deno, first, case.flags, deno_dir)
    if EXPECTED not in out:
        return "skip", f"first run did not print {EXPECTED!r}: {out}"

    second = first if case.same_path else work / f"fresh{case.suffix}"
    second.write_bytes(layout(case.size, swapped=True).encode("utf-8"))
    out = run_once(deno, second, case.flags, deno_dir)
    if EXPECTED in out:
        return "clean", out
    if "SyntaxError" in out:
        return "stale", out
    return "skip", out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deno", default=shutil.which("deno"))
    args = ap.parse_args()
    if not args.deno:
        print("deno not found; pass --deno or put it on PATH")
        return 2

    try:
        version = subprocess.run([args.deno, "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        version = ""
    print(f"{(version.splitlines() or ['deno (version unknown)'])[0]} at {args.deno}\n")

    verdicts: list[tuple[Case, str]] = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="deno-code-cache-") as tmp:
            verdict, output = run_case(args.deno, case, Path(tmp))
        verdicts.append((case, verdict))
        print(f"== {case.label}: {verdict} ==")
        print(output.splitlines()[0] if output else "(no output)")
        print()

    if all(v == "skip" for _, v in verdicts):
        print("deno could not run the repro at all")
        return 2

    stale = [c.label for c, v in verdicts if v == "stale" and c.is_bug]
    broken_controls = [c.label for c, v in verdicts if v == "stale" and not c.is_bug]
    if broken_controls:
        print(f"note: a control went stale too, so the trigger has moved: {', '.join(broken_controls)}")
    if stale:
        print("REPRODUCED: deno reused its code cache for a rewritten script of the same length and path")
        return 1
    print("gone: deno recompiled every rewritten script")
    return 0


if __name__ == "__main__":
    sys.exit(main())
