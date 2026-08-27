"""`edit` — the path to an entry file, so writing one does not mean retyping the folder layout.

An entry is a file and editing it directly is the interface, which left every write starting with the review
folder spelled out from `.tmp/` down.
That is the kind of friction that reads as deliberate and is not.

It prints a path rather than launching anything.
An agent pipes it into whatever it writes with, and a person pastes it — a command that opened an editor would
have to guess which one, and would be useless in the case that actually motivated this.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "edit"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Print the path to an entry file, or list every entry and its path")
    a.review_name(p)
    p.add_argument("entry", nargs="?", default="", help="an entry id, slug or substring; omit to list them all")
    return p


def _matches(entries, needle: str) -> list:
    """Entries this selector names, narrowest reading first.

    An exact id or slug wins outright, because `010` must not be ambiguous with `0100` or with an entry whose
    title happens to contain it.
    """
    exact = [e for e in entries if needle in (e.id, e.slug)]
    return exact or [e for e in entries if needle.lower() in e.slug.lower()]


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, _ = ctx.open(args.name)
    entries = ctx.entries(paths)

    if not args.entry:
        width = max((len(e.id) for e in entries), default=3)
        for entry in entries:
            print(f"{entry.id:<{width}}  {ctx.rel(entry.path)}  {review.console.dim(entry.title)}")
        return

    found = _matches(entries, args.entry)
    if not found:
        ctx.die(f"no entry matches {args.entry!r}")
    if len(found) > 1:
        # Refused rather than resolved to the first: the caller is about to write to whatever comes back.
        listed = "\n".join(f"  {e.id}  {ctx.rel(e.path)}  {e.title}" for e in found)
        ctx.die(f"{args.entry!r} matches {len(found)} entries:\n{listed}")

    print(ctx.rel(found[0].path))
