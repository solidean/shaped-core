"""Read and query a preset's compilation database (compile_commands.json).

This is the ground truth of what the compiler is actually invoked with, per
translation unit — the same database clangd reads. `info compile-command` uses it
to print the exact command for one source file. CMake writes absolute,
forward-slash `file` paths; matching here is separator- and case-insensitive so a
repo-relative path or a bare filename resolves on Windows too.
"""

from __future__ import annotations

import json
import os
from pathlib import Path


def load_entries(build_dir: Path) -> list[dict]:
    """Parse build/<preset>/compile_commands.json into its list of entries.

    Each entry is `{directory, command, file, output}`. Raises FileNotFoundError
    when the database is missing (the preset hasn't been configured).
    """
    path = build_dir / "compile_commands.json"
    if not path.is_file():
        raise FileNotFoundError(f"No compile_commands.json in {build_dir}; configure the preset first.")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _norm(p: str | Path) -> str:
    """Normalize a path for comparison: absolute-agnostic, case- and separator-folded."""
    return os.path.normcase(os.path.normpath(str(p)))


def find_entry(entries: list[dict], file: Path, root: Path) -> dict | None:
    """Locate the entry for `file`: by absolute/repo-relative path, else a unique
    filename-or-suffix tail. Returns None when nothing matches or the tail is
    ambiguous (the caller surfaces suggestions)."""
    raw = str(file)
    target = file if file.is_absolute() else root / file
    want = _norm(target)
    for entry in entries:
        if _norm(entry.get("file", "")) == want:
            return entry

    tail = _norm(raw)
    matches = [
        entry
        for entry in entries
        if _norm(entry.get("file", "")).endswith(os.sep + tail) or _norm(entry.get("file", "")) == tail
    ]
    return matches[0] if len(matches) == 1 else None


def suggest_files(entries: list[dict], file: Path, limit: int = 10) -> list[str]:
    """Files in the database that look related to `file` (same name, then same
    stem) — for a 'did you mean' hint when find_entry comes up empty."""
    name = _norm(Path(str(file)).name)
    same_name = [e["file"] for e in entries if _norm(Path(e["file"]).name) == name]
    if same_name:
        return same_name[:limit]
    stem = _norm(Path(str(file)).stem)
    return [e["file"] for e in entries if stem and stem in _norm(e["file"])][:limit]
