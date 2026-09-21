"""`changes` — the ledger, as something you can read.

Writing an entry means writing `discharges:`, so the change ids are the one thing an agent needs constantly and could not get.
`coverage` reports only what is *un*discharged and truncates; `show` is about entries.
Without this the answer came from parsing `changes/ledger.jsonl` by hand, and "what does nothing account for yet"
came from re-deriving the tool's own gate 2 out of the entry files.
"""

from __future__ import annotations

import argparse
import fnmatch
import json

import tools.review as review

from . import args as a
from .context import Context

NAME = "changes"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="List the ledger: every change, and what accounts for it")
    a.review_name(p)
    a.as_json(p)
    p.add_argument("--undischarged", action="store_true", help="only changes no entry accounts for")
    p.add_argument("--path", default="",
                   help="only changes under these paths: comma-OR'd prefixes or globs, a `!` term subtracts")
    p.add_argument("--ids", action="store_true", help="bare ids, one per line, for pasting into `discharges:`")
    return p


def path_filter(selector: str):
    """What `--path` keeps: comma-OR'd terms, each a path prefix or a glob, and a `!`-prefixed term subtracts.

    The selector grammar `repo_search` takes, so an entry that spans several folders is one query.
    An exclude wins over an include, and a selector of excludes alone keeps everything else.
    """
    terms = [t.strip() for t in selector.split(",") if t.strip()]
    includes = [t for t in terms if not t.startswith("!")]
    excludes = [t[1:] for t in terms if t.startswith("!")]

    def hit(path: str, term: str) -> bool:
        return fnmatch.fnmatch(path, term) if any(c in term for c in "*?[") else path.startswith(term)

    def keeps(path: str) -> bool:
        if any(hit(path, t) for t in excludes):
            return False
        return not includes or any(hit(path, t) for t in includes)

    return keeps


def _rows(ctx: Context, paths: review.ReviewPaths, args) -> list[tuple[review.Change, str]]:
    """(change, what discharges it) for every live change, in ledger order."""
    ledger = ctx.ledger(paths)
    entries = ctx.entries(paths)

    by_change: dict[str, list[str]] = {}
    for entry in entries:
        if entry.state != "open":
            continue
        for block in entry.asks:
            for change_id in block.discharges:
                resolved = ledger.resolve(change_id)
                if resolved is not None:
                    by_change.setdefault(resolved.id, []).append(f"{entry.id}/{block.name}")

    keeps = path_filter(args.path)
    out = []
    for change in ledger.live():
        if not keeps(change.path):
            continue
        holders = by_change.get(change.id, [])
        if change.discharged_by_reason and not holders:
            holders = ["bulk reason"]
        if args.undischarged and holders:
            continue
        out.append((change, ", ".join(holders)))
    return out


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open_changeset(args.name)
    rows = _rows(ctx, paths, args)

    if args.ids:
        print(" ".join(change.id for change, _ in rows))
        return

    if args.json:
        print(json.dumps([{
            "id": change.id, "kind": change.kind, "path": change.path,
            "summary": change.summary, "reason": change.reason,
            "atoms": len(change.claim), "discharged_by": holders,
        } for change, holders in rows], indent=2))
        return

    if not rows:
        print("no changes match" if args.path or args.undischarged else f"{cfg.name} has no changes yet; `review ingest {args.name}`")
        return

    for change, holders in rows:
        mark = review.console.green("+") if holders else review.console.yellow("-")
        print(f"{mark} {change.id}  {change.kind:<11} {change.summary}")
        if holders:
            print(review.console.dim(f"    discharged by {holders}"))

    accounted = sum(1 for _, holders in rows if holders)
    print(f"\n{accounted}/{len(rows)} discharged")
