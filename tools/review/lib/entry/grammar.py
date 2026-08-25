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
BLOCK_TYPES: dict[str, set[str]] = {
    "context/cold": {"round"},
    "context/repo": {"round"},
    "context/delta": {"round"},
    "prose": {"round", "generated"},
    "code": {"round", "lang", "file"},
    "changes": {"round", "generated"},
    "recommendation": {"round"},
    "ask": {"round", "discharges", "follows", "multi"},
}

# The context tiers, in the order a reader meets them, with the word budget each is only useful under.
CONTEXT_TIERS = ("context/cold", "context/repo", "context/delta")
WORD_LIMITS = {"context/cold": 150, "context/repo": 120}

# A block carrying `generated: <key>` is the tool's to rewrite, and nothing else in an entry ever is.
# That marker is what lets `review generate` refresh the overview without touching a sentence anyone wrote.
GENERATED_ATTR = "generated"

STATES = ("open", "obsolete", "superseded")
SEVERITIES = ("bug", "design", "api", "docs", "nit", "question", "lgtm")
OPTION_KINDS = ("radio", "check", "rank")

FRONT_REQUIRED = ("id", "title")
FRONT_KNOWN = {"id", "title", "group", "state", "severity", "round", "resolved-by"}

HEADING_RE = re.compile(r"^##[ \t]+(\S+)(?:[ \t]+(.*?))?[ \t]*$")
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
