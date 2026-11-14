#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/core/memory_guard.py — the caps over what dev.py starts, and the free-RAM floor.

Each case runs in a child Python of its own, since the guard holds for the rest of the process that installs it.
The caps are a few hundred MB here, so the test itself never takes much: a 1 GB cap refuses 2 GB and grants 256 MB,
and a floor above the machine's RAM ends the process at once, with exit code 3.
The caps are Windows Job Object limits; elsewhere only the floor case runs.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/memory-guard-self-test.py [-v]
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERBOSE = "-v" in sys.argv

CHILD = """
import sys, time
sys.path.insert(0, {root!r})
from tools.dev.lib.core import memory_guard
memory_guard.install()
mode = sys.argv[1]
if mode == "allocate":
    size = int(sys.argv[2]) << 20
    try:
        block = bytearray(size)
        print("allocated")
    except MemoryError:
        print("refused")
elif mode == "grandchild":
    import subprocess
    out = subprocess.run([sys.executable, "-c", "b = bytearray(2 << 30); print('allocated')"], capture_output=True, text=True)
    print("refused" if out.returncode != 0 or "allocated" not in out.stdout else "allocated")
elif mode == "breakaway":
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32")
    k.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
    def flags():
        info = memory_guard._ExtendedLimits()
        k.QueryInformationJobObject(memory_guard._job_state[0], 9, ctypes.byref(info), ctypes.sizeof(info), None)
        return info.BasicLimitInformation.LimitFlags
    before = bool(flags() & 0x800)
    widened = memory_guard.allow_breakaway()
    after = bool(flags() & 0x800)
    keeps_cap = bool(flags() & 0x100)
    print(before, widened, after, keeps_cap)
elif mode == "wait":
    time.sleep(10)
    print("survived")
"""


def child(env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    code = CHILD.format(root=str(ROOT))
    return subprocess.run([sys.executable, "-c", code, *args], env={**os.environ, **env}, capture_output=True, text=True,
                          timeout=60)


failures = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global failures
    if not ok:
        failures += 1
    if VERBOSE or not ok:
        print(f"{'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail and not ok else ''}")


caps = {"SC_DEV_MEMORY_PROCESS_GB": "1", "SC_DEV_MEMORY_TOTAL_GB": "0", "SC_DEV_MEMORY_FREE_GB": "0"}
if platform.system() == "Windows":
    r = child(caps, "allocate", "2048")
    check("a 1 GB cap refuses 2 GB", r.stdout.strip() == "refused", r.stdout + r.stderr)
    r = child(caps, "allocate", "256")
    check("a 1 GB cap grants 256 MB", r.stdout.strip() == "allocated", r.stdout + r.stderr)
    r = child(caps, "grandchild")
    check("a process the guarded one starts inherits the cap", r.stdout.strip() == "refused", r.stdout + r.stderr)
    r = child(caps, "breakaway")
    check("the job refuses breaking away until allow_breakaway, then allows it and keeps the cap",
          r.stdout.strip() == "False True True True", r.stdout + r.stderr)
    r = child({**caps, "SC_DEV_MEMORY_PROCESS_GB": "0"}, "allocate", "256")
    check("0 turns the cap off", r.stdout.strip() == "allocated", r.stdout + r.stderr)

start = time.monotonic()
r = child({"SC_DEV_MEMORY_PROCESS_GB": "0", "SC_DEV_MEMORY_TOTAL_GB": "0", "SC_DEV_MEMORY_FREE_GB": "100000"}, "wait")
elapsed = time.monotonic() - start
check("a floor above the RAM there is ends the process at once", r.returncode == 3 and "survived" not in r.stdout and elapsed < 5,
      f"exit {r.returncode} after {elapsed:.1f} s: {r.stdout}{r.stderr}")
check("the floor says why", "memory guard" in r.stderr, r.stderr)

if failures:
    print(f"{failures} memory guard check(s) failed")
    sys.exit(1)
print("memory guard: all checks passed")
