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

from dataclasses import replace

import re
from collections.abc import Callable
from pathlib import Path

from ..entry.answers import AnswerFile
from ..entry.parse import Entry
from .glossary import GlossaryProvider, malformed_in, terms_in
from .index import RepoIndex
from .providers import CommitProvider, DirProvider, FileProvider, Token

# The kind a bad `context:` is reported under; it has no regions, so the page never draws one.
CONTEXT = "context"

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
          terms: list | None = None,
          history: Callable[[int], tuple[RepoIndex | None, str] | None] | None = None) -> list[Token]:
    """Every token this entry's text carries, deduplicated by literal.

    The maintainer's own answers are scanned too.
    They are prose about the change, they use the same vocabulary, and a path they name should reach the code
    exactly as one the agent named does.

    Each provider sees the regions its own kind belongs in.
    A path means the same thing in a code comment as in prose; a sha is safe everywhere; a term is not.

    `history(round)` is None for a round still open, which is judged strictly against the current tree.
    For a finalized round it is the tree that round was read at with its sha, or `(None, "")` where no head is recorded.
    Either way the round's references never raise a problem — see FileProvider.
    Text still open is scanned first, so where both name one literal the strict reading decides it.

    A superseded block is scanned last and never raises a problem.
    It still renders, struck, so its references still link where they can; but it is retired text that the block
    replacing it already corrected, and failing on it would leave no way to make `validate` pass.

    A block's `context:`, or else the entry's, is the folder its short paths are looked for under first.
    It is itself a folder reference, and one that resolves to no single folder is a problem, since every path it
    was meant to settle would otherwise fall back to the repository-wide lookup without a word.
    A literal is resolved once per entry, in the first text naming it, because the page matches literals entry-wide.
    """
    seen_files: set[str] = set()
    seen_dirs: set[str] = set()
    commits = CommitProvider(confirm=confirm_shas) if confirm_shas is not None else None
    glossary = GlossaryProvider(terms=terms) if terms else None
    tokens: list[Token] = []

    contexts: dict[str, str] = {}

    def context_of(raw: str, answered: bool) -> str:
        """The folder a `context:` names, recording a problem once when it names none."""
        if not raw:
            return ""
        if raw not in contexts:
            resolution = index.resolve_dir(raw)
            contexts[raw] = resolution.path if resolution.ok else ""
            if not resolution.ok and not answered:
                what = ("names several folders: " + ", ".join(f"`{c}`" for c in resolution.candidates)
                        if resolution.candidates else "is not a folder in this repository")
                tokens.append(Token(text=raw, kind=CONTEXT, regions=(), problem=f"context folder {raw} {what}"))
        return contexts[raw]

    def scan(text: str, then: tuple[RepoIndex | None, str] | None, context: str = "") -> None:
        past, rev = then if then is not None else (None, "")
        answered = then is not None
        folder = context_of(context, answered)
        files = FileProvider(index=index, seen=seen_files, answered=answered, history=past, history_rev=rev,
                             context=folder)
        dirs = DirProvider(index=index, seen=seen_dirs, answered=answered, history=past, history_rev=rev,
                           context=folder)
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

    entry_context = entry.front.get("context", "").strip()
    context_of(entry_context, False)
    texts: list[tuple[str, int, str]] = []
    retired: list[tuple[str, int, str]] = []
    for block in entry.blocks:
        into = retired if block.is_superseded else texts
        context = block.attrs.get("context", "").strip() or entry_context
        into.append((block.prose, block.round, context))
        into.append((block.head, block.round, context))
        into.extend((option.label, block.round, context) for option in block.options)
    if answers is not None:
        texts.extend((answer.text, 0 if answer.tentative else answer.round, entry_context)
                     for answer in answers.answers.values())
        texts.extend((comment.text, 0 if comment.tentative else comment.round, entry_context)
                     for comment in answers.comments.values())

    then_of = {r: (history(r) if history is not None and r else None) for _, r, _ in [*texts, *retired]}
    for text, r, context in texts:
        if then_of[r] is None:
            scan(text, None, context)
    for text, r, context in texts:
        if then_of[r] is not None:
            scan(text, then_of[r], context)

    live = len(tokens)
    for text, r, context in retired:
        scan(text, then_of[r], context)
    tokens[live:] = [replace(token, problem="") if token.problem else token for token in tokens[live:]]
    return tokens


# The remedy every reference failure carries.
# An author hitting one is looking for exactly this feature at exactly that moment, and has no reason to go and read
# the grammar to find out it exists — so the message says it rather than leaving it to be discovered.
_RAW_REMEDY = "write it as `raw:<path>`, or open the fence as ```raw, to say this is not a reference"


def problems(entry: Entry, tokens: list[Token]) -> list[str]:
    """What `validate` reports: every reference that does not hold, with the entry it is in.

    A context folder is not text in the entry, so `raw:` is no remedy for one and is not offered.
    """
    return [f"{entry.slug}: {token.problem}" if token.kind == CONTEXT
            else f"{entry.slug}: {token.text} — {token.problem}\n  {_RAW_REMEDY}"
            for token in tokens if token.problem]


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


def history_for(repo: Path, cfg,
                trees: dict[str, RepoIndex] | None = None) -> Callable[[int], tuple[RepoIndex | None, str] | None]:
    """The tree each finalized round was read at, for `build(history=...)`.

    None for a round still open, and only for that: the watermark is what makes a round's references lenient.
    A finalized round with no recorded head gets `(None, "")` — a design review has none, and neither has a round that predates the record.
    A round read at the current head still gets its tree, because the index also holds the working tree and that moves without a commit.
    Each commit is listed once however many entries ask about it; pass `trees` to keep that across calls, which is
    safe to keep forever since a commit's tree never changes.
    """
    cache: dict[str, RepoIndex] = trees if trees is not None else {}

    def at(round_number: int) -> tuple[RepoIndex, str] | None:
        if round_number > cfg.watermark:
            return None
        sha = cfg.head_of_round(round_number)
        if not sha:
            return None, ""
        if sha not in cache:
            cache[sha] = RepoIndex.build_at(repo, sha)
        return cache[sha], sha

    return at
