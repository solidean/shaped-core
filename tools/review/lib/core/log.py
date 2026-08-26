"""The review's append-only action log.

Every state change an agent or the maintainer makes lands here, so a review picked up in a later session can be read back in order.
It is a record, never a source of truth — the ledger, the entries and the answers are.
"""

from __future__ import annotations

import time
from pathlib import Path

from .atomic import append_jsonl


def record(log_path: Path, action: str, **fields) -> None:
    """Append one action to the log, stamped with wall-clock time."""
    append_jsonl(log_path, {"at": time.strftime("%Y-%m-%dT%H:%M:%S"), "action": action, **fields})
