"""The review's append-only action log.

Every state change an agent or the maintainer makes lands here, so a review picked up in a later session can be read back in order.
It is a record, never a source of truth — the ledger, the entries and the answers are.
"""

from __future__ import annotations

import time
from pathlib import Path

from .atomic import append_jsonl, read_jsonl


def record(log_path: Path, action: str, **fields) -> None:
    """Append one action to the log, stamped with wall-clock time."""
    append_jsonl(log_path, {"at": time.strftime("%Y-%m-%dT%H:%M:%S"), "action": action, **fields})


def heads_at_finalize(log_path: Path) -> dict[int, str]:
    """The head each round was finalized at, as far as the log can say.

    Recovery for a review whose early rounds predate `round_heads`: `init` and every `sync` record the head they set,
    and a `finalize` happens under whichever came last.
    Read once to backfill the config, which is then the record; the log never answers this question twice.
    """
    heads: dict[int, str] = {}
    current = ""
    for event in read_jsonl(log_path):
        action = event.get("action")
        if action in ("init", "sync") and event.get("head"):
            current = str(event["head"])
        elif action == "finalize" and current:
            heads[int(event.get("round", 0))] = current
    return heads
