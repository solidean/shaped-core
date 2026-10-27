"""Crash reports a test binary leaves behind, kept with what reads them, and rotated; and the logs of a run that failed without one.

A nexus binary run with NX_CRASH_DIR set keeps each crash or hang report there in files of its own.
They are the text report, a Windows minidump `.dmp`, and the run's recording `.ccrec`.
After the run, collect() gives them a folder under build/<preset>/crashes/, with the run's captured output and the binary and PDB that wrote them.
A rebuild replaces the binary, and a minidump reads only against the exact one, so the copies are what keep a report readable.
Copies of one binary are hard links into a store keyed by content, so many crashes of one build cost one binary.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import time
from pathlib import Path

# The newest reports kept per preset, and the most disk they may take together; the user chose these (2026-10-25).
KEEP_REPORTS = 20
KEEP_BYTES = 2 << 30

_STORE = ".binaries"
_INCOMING = "incoming"


def crash_root(build_dir: Path) -> Path:
    return build_dir / "crashes"


def incoming_dir(build_dir: Path) -> Path:
    """Where a binary writes its reports: NX_CRASH_DIR for every test binary of the preset."""
    return crash_root(build_dir) / _INCOMING


def _digest(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()[:20]


def _keep_binary(root: Path, source: Path, folder: Path) -> None:
    """Puts `source` into `folder` under its own name, as a hard link into the store when the filesystem allows."""
    if not source.is_file():
        return
    store = root / _STORE
    store.mkdir(parents=True, exist_ok=True)
    stored = store / f"{_digest(source)}{source.suffix}"
    if not stored.exists():
        shutil.copy2(source, stored)
    target = folder / source.name
    try:
        os.link(stored, target)
    except OSError:
        shutil.copy2(stored, target)


def collect(build_dir: Path, artifact: Path, stdout_log: Path | None, stderr_log: Path | None) -> Path | None:
    """Files the reports `artifact` just left, with its output and its binary, and returns their folder; None when it left none.

    Called after every run of a test binary, so everything in the incoming directory named for it is this run's.
    """
    incoming = incoming_dir(build_dir)
    if not incoming.is_dir():
        return None
    # nx::program_name(): the binary's name without directory or suffix
    prefix = f"{artifact.stem}-"
    files = sorted(f for f in incoming.iterdir() if f.is_file() and f.name.startswith(prefix))
    if not files:
        return None
    reports = [f for f in files if f.suffix == ".txt"]
    # a recording alone is a run that dumped without a report: a hang the watchdog caught, say
    stem = (reports[0] if reports else files[0]).name.split(".")[0]
    root = crash_root(build_dir)
    folder = root / stem
    folder.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.move(str(f), str(folder / f.name))
    for log, name in ((stdout_log, "stdout.txt"), (stderr_log, "stderr.txt")):
        if log is not None and log.is_file():
            shutil.copy2(log, folder / name)
    _keep_binary(root, artifact, folder)
    _keep_binary(root, artifact.with_suffix(".pdb"), folder)
    rotate(build_dir)
    return folder


def keep_failure(build_dir: Path, artifact: Path, logs: list[Path | None]) -> Path:
    """Copies the logs of a run of `artifact` that failed without a crash report into a folder of their own, and returns it.

    The run logs and the JUnit report are overwritten by the next run, so a test that failed once and then passed leaves nothing else to read.
    The folder is `<binary>-<date>-<time>-failed`, rotated with the crash reports.
    """
    root = crash_root(build_dir)
    stem = f"{artifact.stem}-{time.strftime('%Y%m%d-%H%M%S')}-failed"
    folder = root / stem
    n = 1
    while folder.exists():
        folder = root / f"{stem}-{n}"
        n += 1
    folder.mkdir(parents=True)
    for log in logs:
        if log is not None and log.is_file():
            shutil.copy2(log, folder / log.name)
    rotate(build_dir)
    return folder


def _size_of(paths: list[Path], seen: set[tuple[int, int]]) -> int:
    """Bytes of `paths`, a hard-linked file counted once across every call sharing `seen`."""
    total = 0
    for p in paths:
        try:
            st = p.stat()
        except OSError:
            continue
        key = (st.st_dev, st.st_ino)
        if key in seen:
            continue
        seen.add(key)
        total += st.st_size
    return total


def rotate(build_dir: Path, keep_reports: int = KEEP_REPORTS, keep_bytes: int = KEEP_BYTES) -> list[Path]:
    """Deletes the oldest report folders past `keep_reports`, then past `keep_bytes`, the newest always kept; returns what went.

    A stored binary no report links to any more goes with them.
    """
    root = crash_root(build_dir)
    if not root.is_dir():
        return []
    folders = sorted((d for d in root.iterdir() if d.is_dir() and d.name not in (_STORE, _INCOMING)),
                     key=lambda d: d.stat().st_mtime, reverse=True)
    removed = folders[keep_reports:]
    kept = folders[:keep_reports]
    seen: set[tuple[int, int]] = set()
    total = 0
    for i, d in enumerate(kept):
        total += _size_of([f for f in d.rglob("*") if f.is_file()], seen)
        if i > 0 and total > keep_bytes:
            removed += kept[i:]
            break
    for d in removed:
        shutil.rmtree(d, ignore_errors=True)
    store = root / _STORE
    if store.is_dir():
        for f in store.iterdir():
            try:
                if f.is_file() and f.stat().st_nlink <= 1:
                    f.unlink()
            except OSError:
                pass
    return removed
