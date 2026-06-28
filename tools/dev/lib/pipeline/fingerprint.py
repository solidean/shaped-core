"""Configure fingerprinting — skip CMake configure when nothing relevant changed.

The fingerprint hashes the mtimes of CMakeLists.txt/*.cmake files plus the
sorted listing of .cc/.hh source files across the repo. Content changes within
existing sources are the build system's job; this only detects when configure
itself needs to rerun (cmake input changed, or a source was added/removed).
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path

_SKIP_DIRS = {"build", "extern", ".git", "__pycache__"}
_SOURCE_SUFFIXES = (".cc", ".hh")


def compute(root: Path) -> str:
    """Hash cmake inputs + source-file listing for the whole repo."""
    h = hashlib.sha256()

    cmake_files: list[Path] = []
    source_files: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in _SKIP_DIRS]
        for f in filenames:
            if f == "CMakeLists.txt" or f.endswith(".cmake"):
                cmake_files.append(Path(dirpath) / f)
            elif f.endswith(_SOURCE_SUFFIXES):
                source_files.append(os.path.relpath(os.path.join(dirpath, f), root))
    cmake_files.append(root / "CMakePresets.json")

    for f in sorted(cmake_files):
        try:
            rel = f.relative_to(root)
            h.update(f"{rel}:{f.stat().st_mtime_ns}\n".encode())
        except OSError:
            pass

    for f in sorted(source_files):
        h.update(f"{f}\n".encode())

    return h.hexdigest()[:20]


def path_for(build_dir: Path) -> Path:
    return build_dir / ".configure-fingerprint"


def is_current(build_dir: Path, root: Path) -> bool:
    """True if a saved fingerprint exists and matches the current repo state."""
    fp_path = path_for(build_dir)
    if not fp_path.is_file():
        return False
    return fp_path.read_text(encoding="utf-8").strip() == compute(root)


def save(build_dir: Path, root: Path) -> str:
    """Write the current fingerprint and return it."""
    fp = compute(root)
    fp_path = path_for(build_dir)
    fp_path.parent.mkdir(parents=True, exist_ok=True)
    fp_path.write_text(fp, encoding="utf-8")
    return fp
