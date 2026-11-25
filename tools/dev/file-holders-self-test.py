#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/core/file_holders.py — who holds an output a linker could not replace.

The log cases parse lld-link's and MSVC link's messages, which name the output relative to the build directory.
The holder case opens a file in a child process, which the Restart Manager must name, and then names nobody once it exits.
That case is Windows' alone, where an open handle blocks a replacement; elsewhere `holders_of` answers None.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/file-holders-self-test.py [-v]
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.dev.lib.core import file_holders  # noqa: E402

VERBOSE = "-v" in sys.argv
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
    if VERBOSE or not condition:
        print(f"{'ok  ' if condition else 'FAIL'} {what}")


build = Path("C:/build/preset") if sys.platform == "win32" else Path("/build/preset")

lld = "lld-link: error: failed to write output 'libs\\graphics\\sgl.exe': permission denied\n"
check(file_holders.unwritable_outputs(lld, build) == [build / "libs\\graphics\\sgl.exe"], "lld-link's output, under the build")
msvc = "LINK : fatal error LNK1104: cannot open file 'bin\\tool.exe'\nLINK : fatal error LNK1104: cannot open file 'bin\\tool.exe'\n"
check(file_holders.unwritable_outputs(msvc, build) == [build / "bin\\tool.exe"], "MSVC link's output, once however often named")
check(file_holders.unwritable_outputs("error: undefined symbol: main\n", build) == [], "an ordinary link error names no output")

if sys.platform == "win32":
    with tempfile.TemporaryDirectory() as tmp:
        held = Path(tmp) / "held.bin"
        held.write_bytes(b"x")
        child = subprocess.Popen(
            [sys.executable, "-c", "import sys; f = open(sys.argv[1], 'rb'); print('open', flush=True); sys.stdin.read()", str(held)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            check(child.stdout.readline().strip() == "open", "the child opened the file")
            # uv's python may be a launcher whose own child holds the file, so the holder is known by its executable
            holders = file_holders.holders_of(held) or []
            check(any(Path(h.path).name.lower().startswith("python") for h in holders),
                  f"a python is named among {holders}")
            check(all(h.ancestors for h in holders), f"each holder names what started it: {holders}")
            # this test is among the child's ancestors, so the chain reaches past the child's own starter
            check(any(any(a.pid == os.getpid() for a in h.ancestors) for h in holders),
                  f"the chain reaches the process that started the child: {holders}")
            check(any(h.command and "held.bin" in h.command for h in holders), f"the holder's command line is named: {holders}")
            described = "\n".join(file_holders.describe(h) for h in holders)
            check(described.count("<-") >= 1, f"describe prints the chain: {described}")
        finally:
            child.stdin.close()
            child.wait()
        check(file_holders.holders_of(held) == [], "nobody holds it once the child exits")
else:
    check(file_holders.holders_of(Path(__file__)) is None, "no holders are asked off Windows")

print("file-holders self-test:", "FAIL" if failures else "ok")
sys.exit(1 if failures else 0)
