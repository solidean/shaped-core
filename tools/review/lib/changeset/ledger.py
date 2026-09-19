"""`changes/ledger.jsonl` — every change the review has given an identity to.

Append-only, and folded last-wins per id when read.
That is what lets `sync` mark a change superseded, or a bulk claim grow, without ever rewriting a line someone may be reading.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from ..core.atomic import append_jsonl, read_jsonl
from ..space.netspace import LineSpace

# How a change was arrived at, which is what `--stats` and the coverage report group by.
KINDS = ("hunk", "commit-hunk", "file", "rest", "bulk")


@dataclass
class Change:
    """One identified change and the atoms it claims."""

    id: str
    digest: str
    kind: str
    path: str
    summary: str = ""
    provenance: str = "net"
    reason: str = ""
    round: int = 1
    superseded: bool = False
    # The bulk claim this change was folded into, once one covered it before any entry spoke for it.
    # Distinct from `superseded`, which says the content left the range — the fact a land-changes review reads as a fix.
    absorbed_by: str = ""
    has_body: bool = False
    claim: LineSpace = field(default_factory=LineSpace.empty)

    @property
    def is_bulk(self) -> bool:
        return self.kind == "bulk"

    @property
    def discharged_by_reason(self) -> bool:
        """A bulk claim carries its own justification, so it needs no entry to discharge it."""
        return self.is_bulk and bool(self.reason)

    def to_record(self) -> dict:
        return {
            "id": self.id,
            "digest": self.digest,
            "kind": self.kind,
            "path": self.path,
            "summary": self.summary,
            "provenance": self.provenance,
            "reason": self.reason,
            "round": self.round,
            "superseded": self.superseded,
            "absorbed_by": self.absorbed_by,
            "has_body": self.has_body,
            "claim": self.claim.to_records(),
        }

    @staticmethod
    def from_record(raw: dict) -> Change:
        return Change(
            id=str(raw.get("id", "")),
            digest=str(raw.get("digest", "")),
            kind=str(raw.get("kind", "hunk")),
            path=str(raw.get("path", "")),
            summary=str(raw.get("summary", "")),
            provenance=str(raw.get("provenance", "net")),
            reason=str(raw.get("reason", "")),
            round=int(raw.get("round", 1)),
            superseded=bool(raw.get("superseded", False)),
            absorbed_by=str(raw.get("absorbed_by", "")),
            has_body=bool(raw.get("has_body", False)),
            claim=LineSpace.from_records(raw.get("claim") or {}),
        )


@dataclass
class Ledger:
    """The folded view of the ledger file, in first-seen order."""

    path: Path
    changes: dict[str, Change] = field(default_factory=dict)

    @staticmethod
    def load(path: Path) -> Ledger:
        ledger = Ledger(path)
        for raw in read_jsonl(path):
            change = Change.from_record(raw)
            if not change.id:
                continue
            ledger.changes[change.id] = change
        return ledger

    def append(self, change: Change) -> None:
        self.changes[change.id] = change
        append_jsonl(self.path, change.to_record())

    def ids(self) -> set[str]:
        return set(self.changes)

    def by_digest(self) -> dict[str, str]:
        """digest -> id, which is what makes a re-ingest hand out the ids it handed out before."""
        return {c.digest: c.id for c in self.changes.values() if c.digest}

    def live(self) -> list[Change]:
        return [c for c in self.changes.values() if not c.superseded and not c.absorbed_by]

    def covered(self) -> LineSpace:
        """Every atom claimed by a change that has not been superseded."""
        space = LineSpace.empty()
        for change in self.live():
            space = space.union(change.claim)
        return space

    def absorb_into(self, bulk: Change, keep: set[str]) -> list[Change]:
        """Fold every live change lying wholly inside `bulk` into it, except those in `keep`.

        A bulk claim declared after the hunks under it were already ingested — the ordinary case after `sync` — would
        otherwise leave each of them live, undischarged, and asking to be read.
        `keep` is what an entry already discharges: someone decided about those, so they keep their ids.
        """
        absorbed: list[Change] = []
        for change in self.live():
            if change.id == bulk.id or change.is_bulk or change.id in keep:
                continue
            if change.claim.subtract(bulk.claim).is_empty:
                change.absorbed_by = bulk.id
                self.append(change)
                absorbed.append(change)
        return absorbed

    def get(self, change_id: str) -> Change | None:
        return self.changes.get(change_id)

    def resolve(self, token: str) -> Change | None:
        """Look a change up by full id, or by the bare code without the `CHANGE-` prefix."""
        if token in self.changes:
            return self.changes[token]
        prefixed = f"CHANGE-{token.upper()}"
        return self.changes.get(prefixed)
