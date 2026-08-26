"""What an entry file may contain, and the rules that make hand-editing it safe.

An entry is front matter plus a sequence of `## <type>` blocks.
Blocks do not nest and are not fenced, so an agent writing one cannot leave the file unbalanced —
the worst it can do is name a type that does not exist, and that is an error with a line number rather than silent corruption.

A block's body opens with an **attribute prelude**: the run of `key: value` lines starting at the body's first line.
Three conditions must hold together for a line to be an attribute — prelude position, a lowercase-kebab key, and membership in that block type's whitelist —
which is what keeps prose containing a colon from being eaten.
A body whose first line is blank has no prelude at all, and that is the escape hatch for prose that must start with `something:`.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

# Every block type, mapped to the attributes it accepts.
# A key that looks like an attribute but is not on its type's list is an error, never silently prose:
# a typo'd `discharge:` degrading into a sentence is exactly the failure this grammar exists to prevent.
#
# `name` is optional on every type but `ask`: a block already has a derived identity, and this is for one the agent
# expects to point at later.
# An ask is the exception because its heading already names it, and a second spelling of the same thing is one to get wrong.
#
# `addresses` is how the agent answers a comment: any appended block names the comment ids it responds to.
# Outstanding is then computed from those references the way an undischarged change is, rather than tracked —
# the tool has exactly one notion of something owed back, and a second one would be a second thing to get wrong.
#
# `supersedes` retires an earlier block in the same entry without editing it.
# A partial round leaves earlier entries out of date, and the only two moves were appending a correction that buries
# itself at the bottom, or editing a block the maintainer has already read.
_ANY = frozenset({"round", "name", "addresses", "supersedes"})

BLOCK_TYPES: dict[str, set[str]] = {
    "context/cold": set(_ANY),
    "context/repo": set(_ANY),
    "context/delta": set(_ANY),
    # `glossary: true` says the bold leads in this block are terms.
    # An attribute rather than a scrape, because the tool would otherwise drop a paragraph that does not parse
    # as one and say nothing — the same silence the attribute whitelist exists to prevent.
    "prose": _ANY | {"generated", "glossary"},
    "code": _ANY | {"lang", "file"},
    "changes": _ANY | {"generated", "show"},
    "recommendation": set(_ANY),
    "ask": {"round", "discharges", "follows", "addresses", "supersedes"},
    "auto-acknowledge": set(_ANY),
    "artifact": set(_ANY),
}

# A block's identity is `<entry>/r<round>/<name>`, derived so that no entry ever has to be retrofitted with one.
# The name is the block's type, indexed only when that type repeats within the same entry and round — and then all of
# them are indexed, never a bare `prose` beside a `prose#2`.
#
# `prose` and `prose#1` resolve to the same block.
# That alias is what keeps an anchor taken mid-round valid after a later append turns the round's only prose block
# into the first of two.


def derived_name(block_type: str, ordinal: int, *, indexed: bool) -> str:
    """The name a block carries when it declares none."""
    base = block_type.replace("/", "-")
    return f"{base}#{ordinal}" if indexed else base


def canonical_block_name(name: str) -> str:
    """A block name with a `#1` folded away, since the unindexed spelling means the same block."""
    return name[:-2] if name.endswith("#1") else name

# The context tiers, in the order a reader meets them, with the word budget each is only useful under.
CONTEXT_TIERS = ("context/cold", "context/repo", "context/delta")
WORD_LIMITS = {"context/cold": 150, "context/repo": 120}

# The answer key an acknowledgement is filed under, one per round.
#
# Per round rather than per entry, because an entry can gain material in a later round without gaining a question:
# a redrafted artifact, a correction, a note.
# One acknowledgement for the entry would already be answered from an earlier round,
# so the new material would arrive silently under a green tick.
#
# `acknowledged` is not a name an entry may use for an ask of its own, since the two would share an answer.
ACK_PREFIX = "acknowledged"

ACK_PROMPT = "Nothing here needs a decision. Acknowledge that you have read it."

ACK_PROMPT_LATER = "This entry gained material in this round and asks nothing. Acknowledge that you have read it."


def ack_name(round_number: int) -> str:
    return f"{ACK_PREFIX}-r{max(round_number, 1)}"


def is_ack_name(name: str) -> bool:
    """Whether `name` is a synthetic acknowledgement rather than an ask someone wrote."""
    return name == ACK_PREFIX or name.startswith(ACK_PREFIX + "-r")

# A block carrying `generated: <key>` is the tool's to rewrite, and nothing else in an entry ever is.
# That marker is what lets `review generate` refresh the overview without touching a sentence anyone wrote.
GENERATED_ATTR = "generated"

# How a `changes` block opens.
# Required rather than defaulted, because the choice is about the reader's attention rather than about formatting:
# a diff shown by default costs scrolling on every visit, and most entries are decidable from their prose and their evidence.
SHOW_KINDS = ("visible", "collapsed")

STATES = ("open", "obsolete", "superseded")
SEVERITIES = ("bug", "design", "api", "docs", "nit", "question", "lgtm")
OPTION_KINDS = ("radio", "check", "rank")

FRONT_REQUIRED = ("id", "title")
FRONT_KNOWN = {"id", "title", "group", "state", "severity", "round", "resolved-by"}

HEADING_RE = re.compile(r"^##[ \t]+(\S+)(?:[ \t]+(.*?))?[ \t]*$")
# A fenced code block, opened by three or more backticks or tildes and closed by at least as many of the same character.
# `## ` starts a block wherever it lands, so without this an entry cannot quote the block grammar it is written about —
# which is what every entry in a review of this tool wants to do.
FENCE_RE = re.compile(r"^(`{3,}|~{3,})")
ATTR_RE = re.compile(r"^([a-z][a-z0-9-]*):[ \t](.*)$")
OPTION_RE = re.compile(r"^-[ \t]+(" + "|".join(OPTION_KINDS) + r"):[ \t]*(.+?)[ \t]*$")
ASK_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
RECOMMENDED_RE = re.compile(r"\s*\((recommended|recommend)\)\s*$", re.IGNORECASE)


class ReviewParseError(Exception):
    """A malformed entry, carrying where it is and what to do about it."""

    def __init__(self, path, line: int, message: str, remedy: str = "") -> None:
        self.path = path
        self.line = line
        self.message = message
        self.remedy = remedy
        super().__init__(str(self))

    def __str__(self) -> str:
        where = f"{self.path}:{self.line}" if self.path else f"line {self.line}"
        tail = f"\n  {self.remedy}" if self.remedy else ""
        return f"{where}: {self.message}{tail}"


@dataclass(frozen=True)
class Option:
    """One answerable option under an `ask`."""

    kind: str
    label: str
    recommended: bool = False

    def canonical(self) -> str:
        """The option as its identity is taken over, the recommendation included since the maintainer saw it."""
        return f"{self.kind}\t{self.label}\t{'rec' if self.recommended else ''}"


def parse_option(line: str) -> Option | None:
    """Parse an option line, or return None when the line is ordinary prose."""
    m = OPTION_RE.match(line)
    if not m:
        return None
    kind, label = m.group(1), m.group(2)
    recommended = bool(RECOMMENDED_RE.search(label))
    return Option(kind=kind, label=RECOMMENDED_RE.sub("", label).strip(), recommended=recommended)


def did_you_mean(key: str, allowed: set[str]) -> str:
    """The closest whitelisted attribute name, for the remedy on an unknown one."""
    best, best_score = "", 0.0
    for candidate in allowed:
        shared = len(set(key) & set(candidate))
        score = shared / max(len(set(key) | set(candidate)), 1)
        if score > best_score:
            best, best_score = candidate, score
    return best if best_score > 0.5 else ""
