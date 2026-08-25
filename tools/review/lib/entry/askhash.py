"""The hash that makes a finalized question immutable.

An answer is only meaningful against the question it was given to, so the answer file records a hash of that question.
What goes into the hash is therefore exactly what the maintainer *saw*: the ask's name, its prose, and its options in order.

What stays out matters just as much.
Stamping a round, or adding a change id to `discharges:`, is bookkeeping the maintainer never read,
so neither may orphan an answer that was already given.
"""

from __future__ import annotations

import hashlib

from .parse import Block

_DIGEST_CHARS = 16


def canonical(block: Block) -> str:
    """The exact text an ask's identity is taken over."""
    prose_lines = [line.rstrip() for line in block.prose.splitlines()]

    collapsed: list[str] = []
    for line in prose_lines:
        if not line and collapsed and not collapsed[-1]:
            continue
        collapsed.append(line)
    while collapsed and not collapsed[0]:
        collapsed.pop(0)
    while collapsed and not collapsed[-1]:
        collapsed.pop()

    parts = [block.name, "\n".join(collapsed)]
    parts.extend(option.canonical() for option in block.options)
    return "\x1f".join(parts)


def hash_ask(block: Block) -> str:
    return hashlib.blake2b(canonical(block).encode("utf-8"), digest_size=8).hexdigest()[:_DIGEST_CHARS]


def changed(block: Block, recorded_hash: str) -> bool:
    """Whether an ask has moved away from the question an answer was recorded against."""
    return bool(recorded_hash) and hash_ask(block) != recorded_hash
