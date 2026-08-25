"""Durable writes into the review folder, and the append-only logs.

Every writer here is the *only* writer of the file it touches, which is what makes the discipline cheap.
The review folder splits ownership three ways: `entries/` belongs to the agent, `answers/` and the signal mailbox to the server, and everything else to the CLI.
No lock is taken anywhere below, because none of these paths has a second writer to race.
"""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path

# Windows fails an atomic replace while a reader holds the target without FILE_SHARE_DELETE.
# An editor tab or a virus scanner does exactly that, so the replace is retried rather than surfaced.
_REPLACE_ATTEMPTS = 6
_REPLACE_BACKOFF = 0.02


def write_atomic(path: Path, data: bytes | str) -> None:
    """Replace `path` with `data`, atomically, creating parents as needed.

    A reader either sees the previous content or the new content, never a partial write.
    """
    if isinstance(data, str):
        data = data.encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)

    tmp = path.with_name(f"{path.name}.tmp{os.getpid()}-{threading.get_ident()}")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())

    for attempt in range(_REPLACE_ATTEMPTS):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == _REPLACE_ATTEMPTS - 1:
                tmp.unlink(missing_ok=True)
                raise
            time.sleep(_REPLACE_BACKOFF * (attempt + 1))


def append_jsonl(path: Path, record: dict) -> None:
    """Append one JSON record as a line.

    The whole line is encoded before the write so it reaches the file system as a single call,
    which is what lets a concurrent reader never observe half a record.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    line = (json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")
    with open(path, "ab") as f:
        f.write(line)


def read_jsonl(path: Path) -> list[dict]:
    """Every record in a JSONL file, skipping blank lines; missing file reads as empty.

    A truncated trailing line — a write interrupted by a crash — is dropped rather than raising,
    since the ledger is rebuildable and refusing to load it would strand the review.
    """
    if not path.is_file():
        return []
    records = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


def read_json(path: Path, default: dict | None = None) -> dict:
    """Parse a JSON file, falling back to `default` when it is missing or unreadable."""
    if not path.is_file():
        return dict(default or {})
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return dict(default or {})


def write_json(path: Path, payload: dict) -> None:
    """Write a JSON file atomically, formatted so a human diff of it stays readable."""
    write_atomic(path, json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n")


def stat_key(path: Path) -> tuple[int, int]:
    """A cheap change key for a file: (mtime_ns, size), or (0, 0) when it does not exist."""
    try:
        st = path.stat()
    except OSError:
        return (0, 0)
    return (st.st_mtime_ns, st.st_size)
