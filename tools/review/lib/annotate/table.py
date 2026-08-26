"""The token table, which is the seam between Python and the page.

Python finds and resolves over the entry's *source* — fences included, since it has the raw markdown before any
highlighting — and emits this table.
The page walks text nodes and wraps the literal tokens the table names.

That split is what makes annotating inside highlighted code work at all.
Highlighted output is nested spans, a match must never cross an element boundary, and rewriting that with a regex
is the classic mistake; a text node is by construction inside one element, so the page never faces the problem.
It also keeps `validate` able to fail on an ambiguous path, which a browser cannot do.

The half that can still fail silently is the page not finding a string it was handed, which is a much smaller
surface than a second matcher — and the table is data, so the suite asserts it without a browser.
"""

from __future__ import annotations

import re
from pathlib import Path

from ..entry.answers import AnswerFile
from ..entry.parse import Entry
from .index import RepoIndex
from .providers import FileProvider, Token

# Where a file name is actually written: a code span, a markdown link's destination, or inside a fenced block.
# Bare prose is deliberately not scanned.
# The repo's own convention backticks a path, and scanning running text turns every sentence containing a dot
# into a candidate — which is a lot of noise for references nobody wrote.
# A fence is scanned whole, because a path in a code comment is exactly the case the round asked for.
_CODE_SPAN_RE = re.compile(r"`([^`\n]+)`")
_LINK_TARGET_RE = re.compile(r"\]\(([^)\s]+)")
_FENCE_BODY_RE = re.compile(r"^(```|~~~)[^\n]*\n(.*?)(?:^\1|\Z)", re.M | re.S)


def _referencing_text(text: str) -> list[str]:
    """The fragments of a block's source a file reference may be written in."""
    out = [body for _, body in _FENCE_BODY_RE.findall(text)]
    without_fences = _FENCE_BODY_RE.sub("", text)
    out.extend(_CODE_SPAN_RE.findall(without_fences))
    out.extend(_LINK_TARGET_RE.findall(without_fences))
    return out


def build(entry: Entry, index: RepoIndex, *, answers: AnswerFile | None = None) -> list[Token]:
    """Every token this entry's text carries, deduplicated by literal.

    The maintainer's own answers are scanned too.
    They are prose about the change, they use the same vocabulary, and a path they name should reach the code
    exactly as one the agent named does.
    """
    provider = FileProvider(index=index)
    tokens: list[Token] = []

    def scan(text: str) -> None:
        for fragment in _referencing_text(text):
            tokens.extend(provider.tokens(fragment))

    for block in entry.blocks:
        scan(block.prose)
        scan(block.head)
        for option in block.options:
            scan(option.label)
    if answers is not None:
        for answer in answers.answers.values():
            scan(answer.text)
        for comment in answers.comments.values():
            scan(comment.text)
    return tokens


def problems(entry: Entry, tokens: list[Token]) -> list[str]:
    """What `validate` reports: every reference that does not hold, with the entry it is in."""
    return [f"{entry.slug}: {token.text} — {token.problem}" for token in tokens if token.problem]


def to_json(tokens: list[Token]) -> list[dict]:
    return [token.to_json() for token in tokens]


def index_for(repo: Path, review_root: Path | None = None) -> RepoIndex:
    return RepoIndex.build(repo, review_root)
