"""A disk guard over every dev.py command: none runs while the system drive or the repository's drive is low on free space.

A build, a test's dumps and the page file all grow on disk, and a full system drive takes the machine with it; refusing early is the better outcome.
It checks the system drive (Windows' %SystemDrive%, / elsewhere) and the drive holding the repository, and refuses below the floor, 50 GB by default.
A refusal usually needs a person to free space, so it says so; an agent stops and escalates rather than working around it.

SC_DEV_DISK_FREE_GB overrides the floor in GB, 0 turning it off; on CI (CI set) the default is off, since runners' disks are small and disposable.
"""

from __future__ import annotations

import os
import platform
import shutil
import sys
from pathlib import Path

GB = 1 << 30
DEFAULT_FREE_GB = 50.0


def _floor_gb() -> float:
    raw = os.environ.get("SC_DEV_DISK_FREE_GB")
    if raw is None or raw.strip() == "":
        return 0.0 if os.environ.get("CI") else DEFAULT_FREE_GB
    try:
        return float(raw)
    except ValueError:
        print(f"warning: SC_DEV_DISK_FREE_GB={raw!r} is not a number of GB; using {DEFAULT_FREE_GB:g}", file=sys.stderr)
        return DEFAULT_FREE_GB


def _drives(root: Path) -> list[Path]:
    """The system drive and the repository's drive, each once."""
    system = Path(os.environ.get("SystemDrive", "C:") + "\\") if platform.system() == "Windows" else Path("/")
    out: list[Path] = []
    for d in (system, Path(root.resolve().anchor)):
        if d not in out:
            out.append(d)
    return out


def check(root: Path) -> None:
    """Exits with a message when a watched drive has less free space than the floor."""
    floor = _floor_gb()
    if floor <= 0:
        return
    for drive in _drives(root):
        try:
            free = shutil.disk_usage(drive).free
        except OSError:
            continue
        if free < floor * GB:
            message = (f"{drive} has {free / GB:.1f} GB free, below the {floor:g} GB floor, so dev.py refuses to run.\n"
                       f"Free space on {drive} first; this usually needs the user. SC_DEV_DISK_FREE_GB overrides the floor, 0 turns it off.")
            print(f"\ndev.py disk guard: {message}", file=sys.stderr, flush=True)
            if os.environ.get("GITHUB_ACTIONS") == "true":
                print(f"::error title=dev.py disk guard::{message}", flush=True)
            sys.exit(1)
