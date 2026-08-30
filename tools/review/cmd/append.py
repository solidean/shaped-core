"""`append` — add blocks to an existing entry, which is how a round answers the round before it.

A follow-up belongs under the ask it follows, so the answer stays on screen above it and the thread reads in one file.
That makes appending the most common write in a review, and it was the one thing with no command:
it was done by hand, on a file the server is reading, with no check that the result still parsed.

Nothing is ever rewritten here.
The text is appended, the new blocks are stamped with the round about to be served, and the result is parsed before it is written —
so a malformed addition fails with a line number instead of leaving an entry the page cannot render.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import tools.review as review

from . import args as a
from .context import Context

NAME = "append"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Append blocks to an entry, stamped with the current round")
    a.review_name(p)
    p.add_argument("entry", help="the entry to append to: its slug, or just its number (`045`)")
    p.add_argument("--file", default="", metavar="PATH", help="read the blocks from a file, or `-` for stdin (default: stdin)")
    p.add_argument("--dry-run", action="store_true", help="parse and report, write nothing")
    return p


def resolve_entry(ctx: Context, paths: review.ReviewPaths, wanted: str) -> Path:
    """The entry file `wanted` names, by full slug or by its numeric prefix."""
    files = paths.entry_files()
    exact = [f for f in files if f.stem == wanted]
    if exact:
        return exact[0]

    prefixed = [f for f in files if f.stem.startswith(wanted + "-") or f.stem.split("-")[0] == wanted]
    if len(prefixed) == 1:
        return prefixed[0]
    if not prefixed:
        ctx.die(f"no entry {wanted!r} in {ctx.rel(paths.entries_dir)}")
    ctx.die(f"{wanted!r} matches several entries: {', '.join(f.stem for f in prefixed)}")
    raise SystemExit(1)


def read_addition(file: str) -> str:
    """The blocks to append, from a file or from stdin.

    Stdin is decoded as UTF-8 explicitly rather than through the locale.
    Python decodes stdin with the process's code page, which is cp1252 on a default Windows install, so an em dash
    arrives as three characters and is then written back out as three real ones — the entry ends up holding `â€”` and
    nothing reports it.

    `-` means stdin, because every other tool spells it that way and reading it as a path fails with a traceback.
    """
    if not file or file == "-":
        return sys.stdin.buffer.read().decode("utf-8")
    return Path(file).read_text(encoding="utf-8")


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    target = resolve_entry(ctx, paths, args.entry)

    addition = read_addition(args.file)
    if not addition.strip():
        ctx.die("nothing to append (empty input)")

    try:
        entry = review.parse_entry_file(target)
    except review.ReviewParseError as e:
        ctx.die(f"{target.name} does not parse as it stands: {e}")

    before = {b.name for b in entry.asks}
    text = review.append_text(entry, addition)

    # Parsed before it is written, so a bad block is an error with a line number rather than a broken page.
    try:
        merged = review.parse_entry_text(text, target)
    except review.ReviewParseError as e:
        ctx.die(f"the result would not parse: {e}")

    # Counted rather than diffed: appending moves the previous last block's end offset, so identity comparison overcounts by one.
    added = len(merged.blocks) - len(entry.blocks)
    new_asks = [b.name for b in merged.asks if b.name not in before]

    if args.dry_run:
        print(f"{target.stem}: would add {added} block(s)" + (f", asks: {', '.join(new_asks)}" if new_asks else ""))
        return

    review.write_atomic(target, text)

    # Stamp after writing, so the blocks carry the round they were written in rather than the one they are read in.
    stamped = review.parse_entry_file(target)
    updated = review.stamp_rounds(stamped, cfg.next_round)
    if updated is not None:
        review.write_atomic(target, updated)

    review.record(paths.log, "append", entry=target.stem, round=cfg.next_round, asks=new_asks)
    print(f"{target.stem}: appended {added} block(s) as round {cfg.next_round}")
    for name in new_asks:
        print(f"  ask {name}")

    for warning in review.word_warnings(review.parse_entry_file(target)):
        print(review.console.yellow(f"warning: {warning}"), file=sys.stderr)
