"""Change ids: short, stable, and derived from content rather than from a counter.

A counter cannot survive a re-ingest.
Deriving the id from the hunk's own bytes is what lets `review sync` say "this is the same hunk" after the author pushes a fixup,
and what lets a deleted review folder be rebuilt with the ids it had.

The hash input deliberately excludes the `@@` line numbers and the trailing function context, since both move when unrelated code above shifts.
"""

from __future__ import annotations

import hashlib

# Crockford's alphabet: no I, L, O or U, so an id read aloud or retyped does not turn into a different one.
_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

PREFIX = "CHANGE-"
_MIN_LEN = 5
_MAX_LEN = 8


def digest_of(*parts: str) -> str:
    """The content digest a change id is derived from."""
    h = hashlib.blake2b(digest_size=16)
    for part in parts:
        h.update(part.encode("utf-8", "surrogateescape"))
        h.update(b"\x1f")
    return h.hexdigest()


# The digest bits an id is cut from, taken from the most significant end.
# Cutting from the top is what makes a longer id extend the shorter one rather than replace it,
# so a lengthened id still reads as the same change.
_BITS = 64


def _encode(digest: str, length: int) -> str:
    """The first `length` Crockford base32 characters of a hex digest."""
    value = int(digest[:16], 16)
    return "".join(_ALPHABET[(value >> (_BITS - 5 * (i + 1))) & 31] for i in range(length))


def allocate(digest: str, taken: set[str]) -> str:
    """The id for a digest not yet in the ledger, lengthened only as far as a collision forces.

    Five characters is twenty-five bits.
    Four would have been twenty, which is about a one-in-five chance of a collision over a six-hundred-change review —
    common enough that ids of mixed width would have been the normal case rather than the rare one.
    The extension path still exists and is exercised by the self-test, since twenty-five bits is not a guarantee either.
    """
    for length in range(_MIN_LEN, _MAX_LEN + 1):
        candidate = PREFIX + _encode(digest, length)
        if candidate not in taken:
            return candidate
    raise RuntimeError(f"could not allocate a change id for digest {digest[:16]} after {_MAX_LEN} characters")


def allocate_many(digests: list[str], taken: set[str]) -> dict[str, str]:
    """Ids for several new digests at once, resolved deterministically.

    Two new digests colliding in the same run must not depend on the order the diff happened to emit them,
    so the lexicographically-first digest keeps the short id and the rest lengthen.
    """
    assigned: dict[str, str] = {}
    pool = set(taken)
    for digest in sorted(set(digests)):
        change_id = allocate(digest, pool)
        pool.add(change_id)
        assigned[digest] = change_id
    return assigned
