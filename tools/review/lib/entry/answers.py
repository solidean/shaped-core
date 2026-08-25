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
class AnswerFile:
    """Every answer recorded against one entry, plus the orphans its questions left behind."""

    path: Path
    entry: str = ""
    answers: dict[str, Answer] = field(default_factory=dict)
    orphans: dict[str, Answer] = field(default_factory=dict)

    @staticmethod
    def load(path: Path, entry_slug: str = "") -> AnswerFile:
        raw = read_json(path)
        file = AnswerFile(path=path, entry=str(raw.get("entry", entry_slug)))
        for name, record in (raw.get("answers") or {}).items():
            file.answers[name] = Answer.from_record(name, record)
        for key, record in (raw.get("orphans") or {}).items():
            file.orphans[key] = Answer.from_record(key, record)
        return file

    def save(self) -> None:
        write_json(self.path, {
            "entry": self.entry,
            "answers": {name: a.to_record() for name, a in sorted(self.answers.items())},
            "orphans": {key: a.to_record() for key, a in sorted(self.orphans.items())},
        })

    def get(self, name: str) -> Answer | None:
        return self.answers.get(name)

    def upsert(self, block: Block, *, selected: list[str], text: str, round_number: int) -> Answer:
        """Record a tentative answer against an ask as it currently reads.

        An answer already finalized against a different wording is moved to the orphans rather than overwritten,
        so the round it belonged to still reads correctly afterwards.
        """
        current_hash = hash_ask(block)
        existing = self.answers.get(block.name)
        if existing is not None and not existing.tentative and existing.prompt_hash != current_hash:
            self.orphans[f"{block.name}@{existing.prompt_hash}"] = existing

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

    def finalize(self, round_number: int) -> list[Answer]:
        """Freeze every tentative, non-empty answer into `round_number`, and report what was frozen."""
        frozen: list[Answer] = []
        for answer in self.answers.values():
            if answer.tentative and not answer.is_empty:
                answer.tentative = False
                answer.round = round_number
                frozen.append(answer)
        return frozen

    def since(self, watermark: int) -> list[Answer]:
        """Finalized answers newer than the watermark, which is what a round hands back to the agent."""
        return [a for a in self.answers.values() if not a.tentative and a.round > watermark]

    def reconcile(self, entry: Entry) -> list[str]:
        """Move answers whose ask no longer exists into the orphans; report the names moved."""
        live = {block.name for block in entry.asks}
        moved = []
        for name in list(self.answers):
            if name not in live:
                self.orphan(name, "")
                moved.append(name)
        return moved
