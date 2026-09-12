"""Pinned-install state for the deps under extern/: fetched, at the wrong pin, or absent.

An `extern/<dep>/` fetched by script records what it installed in `.install/pin.txt`, and `dependency.yml` declares the pin it should carry.
Comparing the two is the whole question, and it has three answers rather than two — a missing install and a stale one need different fixes.

This lives in `project/` because two groups above it ask the same question and neither may import the other:
`pipeline/prereqs.py` re-fetches on a mismatch during configure, and `toolchain/graphics.py` reports one in `dev.py doctor`.
"""

from __future__ import annotations

import sys
from pathlib import Path


def pinned_hash(manifest: Path) -> str | None:
    """Read the first `pin_hash` from a dependency.yml (the authority its install is matched against).

    Scanned by line rather than parsed, so this stays stdlib-only — dev.py declares no dependencies, and this runs on the fast path of every configure.
    The first entry is the one whose pin `.install/pin.txt` carries, which is why zydis declares Zydis before the Zycore it vendors.

    An upstream shipping one asset per platform declares `pin_hash_<os>` instead, and the host's key is what counts.
    Taking the bare key there — or another platform's — would make every configure believe the install is stale and
    re-fetch it, which is exactly the fast path this function exists to keep fast.
    """
    if not manifest.is_file():
        return None
    host_key = f"pin_hash_{'windows' if sys.platform == 'win32' else 'macos' if sys.platform == 'darwin' else 'linux'}:"
    fallback = None
    for line in manifest.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith(host_key):
            return s.split(":", 1)[1].strip().strip('"').strip("'")
        if fallback is None and s.startswith("pin_hash:"):
            fallback = s.split(":", 1)[1].strip().strip('"').strip("'")
    return fallback


def is_current(manifest: Path, pin: Path) -> bool:
    """True when the install's pin.txt already matches the manifest's pin_hash."""
    expected = pinned_hash(manifest)
    return bool(expected) and pin.is_file() and pin.read_text(encoding="utf-8").strip() == expected


def install_state(root: Path, directory: str) -> str:
    """State of `extern/<directory>`'s install: "missing", "stale" or "current".

    "stale" is the one a bare `.install/pin.txt` existence test cannot see, and it is the state that makes a build
    quietly use a version nobody chose — so a reporting caller must tell it apart from "missing", whose fix is the same
    command for a different reason.
    """
    base = root / "extern" / directory
    pin = base / ".install" / "pin.txt"
    if not pin.is_file():
        return "missing"
    return "current" if is_current(base / "dependency.yml", pin) else "stale"
