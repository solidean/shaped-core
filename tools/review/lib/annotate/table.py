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
from .glossary import GlossaryProvider, malformed_in, terms_in
from .index import RepoIndex
from .providers import CommitProvider, DirProvider, FileProvider, Token

# Where a file name is actually written: a code span, a markdown link's destination, or inside a fenced block.
# Bare prose is deliberately not scanned.
# The repo's own convention backticks a path, and scanning running text turns every sentence containing a dot
# into a candidate — which is a lot of noise for references nobody wrote.
# A fence is scanned whole, because a path in a code comment is exactly the case the round asked for.
_CODE_SPAN_RE = re.compile(r"`([^`\n]+)`")
_LINK_TARGET_RE = re.compile(r"\]\(([^)\s]+)")
_FENCE_BODY_RE = re.compile(r"^(```|~~~)([^\n]*)\n(.*?)(?:^\1|\Z)", re.M | re.S)


def _is_raw(info: str) -> bool:
    """Whether this fence's info string opts it out, matching what the renderer does with the same string."""
    info = info.strip()
    return info == "raw" or info.startswith("raw:")


def _referencing_text(text: str) -> list[str]:
    """The fragments of a block's source a file reference may be written in.

    A `raw:` span, a `raw:` link destination and a `raw` fence are left out: that is the author saying this looks
    like a reference and is not, which is the one thing the matcher cannot work out for itself.

    A link destination has to opt out the same way a code span does, and dropping the prefix is not enough on its
    own — the file matcher's lookbehind rejects a `:`, so a `raw:` left in front makes it start one segment late
    and report a path the author never wrote.
    """
    out = [body for _, info, body in _FENCE_BODY_RE.findall(text) if not _is_raw(info)]
    without_fences = _FENCE_BODY_RE.sub("", text)
    out.extend(span for span in _CODE_SPAN_RE.findall(without_fences) if not span.startswith("raw:"))
    out.extend(target for target in _LINK_TARGET_RE.findall(without_fences) if not target.startswith("raw:"))
    return out


def build(entry: Entry, index: RepoIndex, *, answers: AnswerFile | None = None, confirm_shas=None,
          terms: list | None = None) -> list[Token]:
    """Every token this entry's text carries, deduplicated by literal.

    The maintainer's own answers are scanned too.
    They are prose about the change, they use the same vocabulary, and a path they name should reach the code
    exactly as one the agent named does.

    Each provider sees the regions its own kind belongs in.
    A path means the same thing in a code comment as in prose; a sha is safe everywhere; a term is not.
    """
    files = FileProvider(index=index)
    dirs = DirProvider(index=index)
    commits = CommitProvider(confirm=confirm_shas) if confirm_shas is not None else None
    glossary = GlossaryProvider(terms=terms) if terms else None
    tokens: list[Token] = []

    def scan(text: str) -> None:
        for fragment in _referencing_text(text):
            # Files first: a folder token is only ever the trailing-slash form, so the two cannot claim the
            # same span, and ordering them keeps the page's longest-first sort from having to break a tie.
            tokens.extend(files.tokens(fragment))
            tokens.extend(dirs.tokens(fragment))
        if commits is not None:
            tokens.extend(commits.tokens(text))
        if glossary is not None:
            # The glossary entry is skipped: underlining a definition inside its own definition says nothing.
            tokens.extend(glossary.tokens(text, skip_entry=entry.slug))

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


def glossary_terms(entries) -> list:
    """Every term declared anywhere in the review, since one entry defines what all of them use."""
    out = []
    for entry in entries:
        out.extend(terms_in(entry))
    return out


def glossary_problems(entries) -> list[str]:
    """Paragraphs in a glossary block that do not parse as a term."""
    out = []
    for entry in entries:
        out.extend(malformed_in(entry))
    return out


def index_for(repo: Path, review_root: Path | None = None) -> RepoIndex:
    return RepoIndex.build(repo, review_root)
