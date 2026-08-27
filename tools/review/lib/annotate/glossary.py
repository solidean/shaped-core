"""Glossary terms, and the provider that underlines them wherever they are used.

A review coins vocabulary and one entry defines it, which helps only a reader who remembers to go there.
Entries are read in whatever order the navigation offers, so a term is usually met before its definition and the
reader either guesses or breaks off to look it up.

Best effort is the deal here.
A term the matcher misses costs nothing, because missing it is the situation without any of this.
What would cost something is a page where every third word is underlined, so a term is drawn once per block
rather than once per occurrence.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from ..entry.parse import Entry
from .providers import CODE, PROSE, Token

# `**term** — definition`, or with aliases: `**term** (plural, other) — definition`.
# An em dash, an en dash or a double hyphen, because all three get typed.
_TERM_RE = re.compile(r"^\*\*(?P<term>[^*]+)\*\*(?:\s*\((?P<aliases>[^)]*)\))?\s*(?:—|–|--)\s*(?P<body>.+)$")


@dataclass(frozen=True)
class Term:
    """One glossary entry: what it is called, what else it is called, and what it means."""

    term: str
    body: str
    entry: str = ""
    aliases: tuple[str, ...] = ()

    def spellings(self) -> list[str]:
        """Every form to look for, longest first so `line space` wins over `line`."""
        out = {self.term, *self.aliases}
        for word in list(out):
            # Naive plurals both ways.
            # Regular enough to be worth having, and an alias list is there for the rest.
            if word.endswith("s"):
                out.add(word[:-1])
            else:
                out.update((word + "s", word + "es"))
        return sorted(out, key=len, reverse=True)


def terms_in(entry: Entry) -> list[Term]:
    """Every term a glossary block in this entry declares."""
    out: list[Term] = []
    for block in entry.blocks:
        if block.attrs.get("glossary", "").lower() not in ("true", "yes", "1"):
            continue
        for paragraph in _paragraphs(block.prose):
            match = _TERM_RE.match(paragraph.replace("\n", " ").strip())
            if match is None:
                continue
            aliases = tuple(a.strip() for a in (match.group("aliases") or "").split(",") if a.strip())
            out.append(Term(term=match.group("term").strip(), body=match.group("body").strip(),
                            entry=entry.slug, aliases=aliases))
    return out


def malformed_in(entry: Entry) -> list[str]:
    """Paragraphs in a glossary block that do not parse as a term, which `validate` reports.

    This is the whole reason the block is marked rather than scraped: a paragraph that silently is not a term
    is a term nobody finds out is missing.
    """
    out: list[str] = []
    for block in entry.blocks:
        if block.attrs.get("glossary", "").lower() not in ("true", "yes", "1"):
            continue
        for paragraph in _paragraphs(block.prose):
            flat = paragraph.replace("\n", " ").strip()
            if flat and _TERM_RE.match(flat) is None:
                out.append(f"{entry.slug}: {flat[:60]}… is in a glossary block but is not `**term** — definition`")
    return out


def _paragraphs(text: str) -> list[str]:
    return [p for p in re.split(r"\n\s*\n", text) if p.strip()]


@dataclass
class GlossaryProvider:
    """Terms, matched whole-word and case-insensitively, in whatever spelling the text actually used.

    Regions are prose and code *comments* rather than everywhere: a term like `space` or `claim` matched inside an
    identifier would light up every line, which is how a reader learns the underlines are noise.

    Once-per-block is the page's rule rather than this one's.
    A token names a literal to look for, and the same word appears in several blocks of one entry — so the limit
    belongs where the blocks are, not where the terms are.
    """

    terms: list[Term]
    kind: str = "glossary"
    regions: tuple[str, ...] = (PROSE, CODE)
    seen: set[str] = field(default_factory=set)

    def tokens(self, text: str, *, skip_entry: str = "") -> list[Token]:
        out: list[Token] = []
        for term in self.terms:
            if skip_entry and term.entry == skip_entry:
                continue
            for spelling in term.spellings():
                for found in re.finditer(r"\b" + re.escape(spelling) + r"\b", text, re.IGNORECASE):
                    literal = text[found.start():found.end()]
                    if literal in self.seen:
                        continue
                    self.seen.add(literal)
                    out.append(Token(
                        text=literal, kind=self.kind, css="ref-term", regions=self.regions,
                        label=literal, note=f"**{term.term}** — {term.body}", target=term.entry,
                    ))
        return out
