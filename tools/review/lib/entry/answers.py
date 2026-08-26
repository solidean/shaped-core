"""`answers/<entry>.json` — what the maintainer chose and typed.

The server is the only writer here, and the agent is the only writer under `entries/`.
That split, not locking, is what lets an entry gain a paragraph while an answer to it is being typed.

An answer is never destroyed.
A question that changes keeps the old answer under its old hash; a question that disappears keeps it as an orphan.
Losing typed text to a tool's bookkeeping would teach the maintainer not to type.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path

from ..core.atomic import read_json, write_json
from .askhash import hash_ask
from .grammar import is_ack_name
from .parse import Block, Entry


@dataclass
class Answer:
    """One ask's answer, as the maintainer left it."""

    name: str
    prompt_hash: str = ""
    selected: list[str] = field(default_factory=list)
    text: str = ""
    round: int = 0
    tentative: bool = True
    at: str = ""

    @property
    def is_empty(self) -> bool:
        return not self.selected and not self.text.strip()

    def to_record(self) -> dict:
        return {
            "prompt_hash": self.prompt_hash,
            "selected": list(self.selected),
            "text": self.text,
            "round": self.round,
            "tentative": self.tentative,
            "at": self.at,
        }

    @staticmethod
    def from_record(name: str, raw: dict) -> Answer:
        return Answer(
            name=name,
            prompt_hash=str(raw.get("prompt_hash", "")),
            selected=[str(s) for s in raw.get("selected", [])],
            text=str(raw.get("text", "")),
            round=int(raw.get("round", 0)),
            tentative=bool(raw.get("tentative", True)),
            at=str(raw.get("at", "")),
        )


@dataclass
class Comment:
    """A remark the maintainer left somewhere the agent did not leave a question.

    Anchored on a block — `r2/prose#1`, the identity every block carries — or on one line of a change's diff,
    keyed by the change id and the offset into that change's `.diff` body.
    That key is stable for exactly as long as the change id is, so a `sync` that supersedes the change correctly
    leaves the comment hanging on a hunk that no longer exists.

    A comment is never a tracked question.
    The agent answers one by appending a block that names it in `addresses:`, and what is outstanding is computed
    from that rather than stored here — the same way an undischarged change is computed.
    """

    id: str
    text: str = ""
    block: str = ""
    change: str = ""
    offset: int = -1
    round: int = 0
    tentative: bool = True
    at: str = ""

    @property
    def is_line(self) -> bool:
        return bool(self.change) and self.offset >= 0

    def where(self) -> str:
        """Where this comment sits, as one line an agent can act on without opening the page."""
        if self.is_line:
            return f"{self.change} line {self.offset + 1} of its diff"
        return self.block or "the entry"

    def to_record(self) -> dict:
        return {
            "text": self.text, "block": self.block, "change": self.change, "offset": self.offset,
            "round": self.round, "tentative": self.tentative, "at": self.at,
        }

    @staticmethod
    def from_record(comment_id: str, raw: dict) -> Comment:
        return Comment(
            id=comment_id,
            text=str(raw.get("text", "")),
            block=str(raw.get("block", "")),
            change=str(raw.get("change", "")),
            offset=int(raw.get("offset", -1)),
            round=int(raw.get("round", 0)),
            tentative=bool(raw.get("tentative", True)),
            at=str(raw.get("at", "")),
        )


def _comment_order(comment: Comment) -> tuple:
    """Comments in the order they were written, which is the order they are read in."""
    return (comment.round, int(comment.id[1:]) if comment.id[1:].isdigit() else 0, comment.id)


@dataclass
class AnswerFile:
    """Every answer recorded against one entry, its comments, and the orphans its questions left behind."""

    path: Path
    entry: str = ""
    answers: dict[str, Answer] = field(default_factory=dict)
    orphans: dict[str, Answer] = field(default_factory=dict)
    comments: dict[str, Comment] = field(default_factory=dict)

    @staticmethod
    def load(path: Path, entry_slug: str = "") -> AnswerFile:
        raw = read_json(path)
        file = AnswerFile(path=path, entry=str(raw.get("entry", entry_slug)))
        for name, record in (raw.get("answers") or {}).items():
            file.answers[name] = Answer.from_record(name, record)
        for key, record in (raw.get("orphans") or {}).items():
            file.orphans[key] = Answer.from_record(key, record)
        for comment_id, record in (raw.get("comments") or {}).items():
            file.comments[comment_id] = Comment.from_record(comment_id, record)
        return file

    def save(self) -> None:
        write_json(self.path, {
            "entry": self.entry,
            "answers": {name: a.to_record() for name, a in sorted(self.answers.items())},
            "orphans": {key: a.to_record() for key, a in sorted(self.orphans.items())},
            "comments": {c.id: c.to_record() for c in sorted(self.comments.values(), key=_comment_order)},
        })

    def comment(self, *, comment_id: str, text: str, block: str, change: str, offset: int,
                round_number: int) -> Comment | None:
        """Add, edit or delete a comment; deleting is saving it empty, and returns None.

        A finalized comment is left alone — it has been handed to the agent, and the round that quoted it
        has to keep reading correctly.
        """
        existing = self.comments.get(comment_id)
        if existing is not None and not existing.tentative:
            return existing
        if not text.strip():
            self.comments.pop(comment_id, None)
            return None
        comment = Comment(
            id=comment_id or self.next_comment_id(), text=text, block=block, change=change, offset=offset,
            round=round_number, tentative=True, at=time.strftime("%Y-%m-%dT%H:%M:%S"),
        )
        self.comments[comment.id] = comment
        return comment

    def next_comment_id(self) -> str:
        """The next id for this entry, monotonic so an id is never reused by a later comment."""
        used = [int(c[1:]) for c in self.comments if c[1:].isdigit()]
        return f"c{max(used, default=0) + 1}"

    def comments_since(self, watermark: int) -> list[Comment]:
        """Finalized comments newer than the watermark, which is what a round hands over."""
        return sorted((c for c in self.comments.values() if not c.tentative and c.round > watermark),
                      key=_comment_order)

    def get(self, name: str) -> Answer | None:
        return self.answers.get(name)

    def upsert(self, block: Block, *, selected: list[str], text: str, round_number: int) -> Answer:
        """Record a tentative answer against an ask as it currently reads.

        A finalized answer is never overwritten, whatever the new one says.
        It is moved to the orphans first, keyed by the round it was frozen in, so re-answering an ask in a later round
        adds to the record instead of replacing it — and the round that already quoted it still reads correctly.
        The wording being unchanged is not a licence to overwrite: that is the common case, not the safe one.
        """
        current_hash = hash_ask(block)
        existing = self.answers.get(block.name)
        if existing is not None and not existing.tentative:
            key = f"{block.name}@{existing.prompt_hash}" if existing.prompt_hash != current_hash else f"{block.name}@r{existing.round}"
            self.orphans[key] = existing

        answer = Answer(
            name=block.name, prompt_hash=current_hash, selected=list(selected), text=text,
            round=round_number, tentative=True, at=time.strftime("%Y-%m-%dT%H:%M:%S"),
        )
        self.answers[block.name] = answer
        return answer

    def orphan(self, name: str, prompt_hash: str) -> Answer | None:
        """Move an answer whose ask has disappeared into the orphans, keeping every character of it."""
        answer = self.answers.pop(name, None)
        if answer is None:
            return None
        self.orphans[f"{name}@{prompt_hash or answer.prompt_hash}"] = answer
        return answer

    def finalize(self, round_number: int) -> tuple[list[Answer], list[Comment]]:
        """Freeze every tentative, non-empty answer and every tentative comment into `round_number`.

        Comments freeze with the answers: nothing reaches the agent until a round is sent, so a remark the maintainer
        thought better of can still be deleted right up to that point.
        """
        frozen: list[Answer] = []
        for answer in self.answers.values():
            if answer.tentative and not answer.is_empty:
                answer.tentative = False
                answer.round = round_number
                frozen.append(answer)
        sent: list[Comment] = []
        for comment in self.comments.values():
            if comment.tentative:
                comment.tentative = False
                comment.round = round_number
                sent.append(comment)
        return frozen, sent

    def since(self, watermark: int) -> list[Answer]:
        """Finalized answers newer than the watermark, which is what a round hands back to the agent."""
        return [a for a in self.answers.values() if not a.tentative and a.round > watermark]

    def reconcile(self, entry: Entry) -> list[str]:
        """Move answers whose ask no longer exists into the orphans; report the names moved.

        An acknowledgement from an earlier round is exempt: it is synthetic, only the newest one is ever offered,
        and the older ones going out of scope is the mechanism working rather than a question disappearing.
        """
        live = {block.name for block in entry.asks}
        moved = []
        for name in list(self.answers):
            if is_ack_name(name):
                continue
            if name not in live:
                self.orphan(name, "")
                moved.append(name)
        return moved
