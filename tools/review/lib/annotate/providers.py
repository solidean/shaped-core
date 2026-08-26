"""What an annotation provider is, and the first one.

Four things want the same treatment — a file path, a glossary term, a commit sha, and one day a symbol.
Each is: find a pattern in the text, decide what it means, and attach something the reader can hover.
Built one at a time they would become four matchers with four ideas about what a code block is, four popovers,
and four sets of rules about what must never be touched.

So a provider offers a matcher over plain text and a decoration, and the pass that walks the content is shared.
The value of naming the seam while only three of the four exist is that the fourth is deferred — which is what
proves the seam is real rather than a description of three things that happened to look alike.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from .index import AMBIGUOUS, MISSING, RESOLVED, RepoIndex

# Where a provider's tokens may be decorated.
# Per provider rather than one global policy: a path means the same thing in prose and in a code comment,
# while a glossary term matched inside an identifier would light up every line.
PROSE = "prose"
CODE = "code"
DIFF = "diff"

# A path: at least one dot-suffixed segment, optionally `:line` or `:line-line`.
# The `new:` and `old:` prefixes are how an author says a path is not supposed to be there yet, or not any more.
_PREFIXES = ("new:", "old:")
_PATH_RE = re.compile(r"(?:\b|^|(?<=/))((?:new:|old:)?/?[\w./+-]*[\w+-]\.[A-Za-z0-9]{1,6})(?::(\d+)(?:-(\d+))?)?")

# What a reference asserts about the repository, which is what `validate` checks it against.
PLAIN, NEW, OLD = "plain", "new", "old"


@dataclass
class Token:
    """One decorated span: the literal text to find, and everything the page needs to draw it.

    The page never decides what a token *is*.
    It walks text nodes, looks for these literals, and wraps them — a string search against a known list rather
    than a second matcher that could disagree with this one.
    """

    text: str
    kind: str
    label: str = ""
    href: str = ""
    path: str = ""
    line: int = 0
    end_line: int = 0
    css: str = ""
    regions: tuple[str, ...] = (PROSE, CODE, DIFF)
    problem: str = ""

    def to_json(self) -> dict:
        out = {"text": self.text, "kind": self.kind, "regions": list(self.regions)}
        for key in ("label", "href", "path", "css"):
            if getattr(self, key):
                out[key] = getattr(self, key)
        if self.line:
            out["line"] = self.line
        if self.end_line:
            out["end"] = self.end_line
        return out


@dataclass
class FileProvider:
    """Paths, resolved against the repository under review.

    Ambiguous is an error the author can always fix by writing a longer path, so failing on it costs nothing
    and buys a guarantee.
    Unresolved is an error too, because the exceptional cases are marked rather than guessed: `new:` for a file
    this change intends to create, which must *not* resolve, and `old:` for one it removes, which may go either way.
    A single "might not exist" marker would let a stale `new:` sit forever, and stale is what the strictness is for.
    """

    index: RepoIndex
    regions: tuple[str, ...] = (PROSE, CODE, DIFF)
    kind: str = "file"
    seen: set[str] = field(default_factory=set)

    def tokens(self, text: str) -> list[Token]:
        out: list[Token] = []
        for match in _PATH_RE.finditer(text):
            literal = match.group(0)
            if literal in self.seen:
                continue
            ref = match.group(1)
            for prefix in _PREFIXES:
                ref = ref[len(prefix):] if ref.startswith(prefix) else ref
            # Prose that happens to hold a dot is not a reference, and treating one as a broken reference would
            # make the strictness unusable.
            # The repository decides, by what its own files are actually called.
            if not self.index.looks_like_a_path(ref):
                continue
            self.seen.add(literal)
            token = self._token(literal, match.group(1), match.group(2), match.group(3))
            if token is not None:
                out.append(token)
        return out

    def _token(self, literal: str, ref: str, start: str | None, end: str | None) -> Token | None:
        intent = PLAIN
        for prefix in _PREFIXES:
            if ref.startswith(prefix):
                intent, ref = (NEW if prefix == "new:" else OLD), ref[len(prefix):]
                break

        resolution = self.index.resolve(ref)
        line = int(start) if start else 0
        end_line = int(end) if end else 0
        shown = literal[len(intent) + 1:] if intent != PLAIN else literal

        if resolution.state == AMBIGUOUS:
            listed = ", ".join(resolution.candidates[:4]) + ("…" if len(resolution.candidates) > 4 else "")
            return Token(text=literal, kind=self.kind, css="ref-bad", regions=self.regions,
                         label=shown, problem=f"{ref} names {len(resolution.candidates)} files: {listed}")

        if intent == NEW:
            problem = "" if resolution.state == MISSING else f"{ref} already exists, so it is not new any more"
            return Token(text=literal, kind=self.kind, css="ref-new", regions=self.regions,
                         label=shown, problem=problem)

        if resolution.state == MISSING:
            if intent == OLD:
                return Token(text=literal, kind=self.kind, css="ref-old", regions=self.regions, label=shown)
            return Token(text=literal, kind=self.kind, css="ref-bad", regions=self.regions,
                         label=shown, problem=f"{ref} is not a file in this repository")

        return Token(
            text=literal, kind=self.kind, label=shown, path=resolution.path, line=line, end_line=end_line,
            css="ref-old" if intent == OLD else "ref", regions=self.regions,
            href=f"/file/{resolution.path}" + (f"#L{line}" if line else ""),
        )


# Seven or more hex characters at a word boundary.
# Everything narrower is a word, and everything wider is still checked against git before it is decorated.
_SHA_RE = re.compile(r"\b([0-9a-f]{7,40})\b")


@dataclass
class CommitProvider:
    """Commit shas, confirmed against the repository rather than against a better regex.

    The safest of the providers, and the reason is that there is no ambiguity case: a hash either names a commit
    in this repository or it does not.
    So an unresolved candidate stays plain text with no diagnostic at all — a sha quoted from somewhere else is
    a normal thing for an entry to carry, not a mistake.
    """

    confirm: object
    regions: tuple[str, ...] = (PROSE, CODE, DIFF)
    kind: str = "commit"
    seen: set[str] = field(default_factory=set)

    def tokens(self, text: str) -> list[Token]:
        candidates = [m.group(1) for m in _SHA_RE.finditer(text) if m.group(1) not in self.seen]
        if not candidates:
            return []
        # Deduplicated before asking, since one sha usually appears several times in a paragraph about it.
        unique = list(dict.fromkeys(candidates))
        real = self.confirm(unique)
        out = []
        for sha in unique:
            self.seen.add(sha)
            if sha in real:
                out.append(Token(text=sha, kind=self.kind, label=sha, css="ref-commit", regions=self.regions))
        return out


__all__ = ["CODE", "DIFF", "PROSE", "CommitProvider", "FileProvider", "Token",
           "AMBIGUOUS", "MISSING", "RESOLVED"]
