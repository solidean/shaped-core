"""`submodules` — bring the submodules up to their recorded commits, or to their tracked branches, without risking local work.

The rules are tools/dev/lib/project/submodules.py's docstring; docs/guides/building-and-testing.md has the workflow.
"""

from __future__ import annotations

import argparse
import sys

from tools.dev import console
from tools.dev.lib.project import submodules as lib

from .context import Context

NAME = "submodules"

_COLORS = {"initialized": console.green, "would init": console.green, "updated": console.green, "would update": console.green,
           "up-to-date": console.dim, "ahead": console.yellow, "skipped": console.yellow, "refused": console.red}


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Fast-forward the submodules to their recorded commits, or to their tracked branches, never touching local work")
    s = p.add_subparsers(dest="submodules_cmd", required=True, metavar="<command>")
    q = s.add_parser("sync", help="init the missing submodules, fast-forward every clean one to the commit the superproject records (--tip: to its tracked branch), then init their missing submodules")
    q.add_argument("paths", nargs="*", help="only these submodules (default: all)")
    q.add_argument("--dry-run", action="store_true", help="fetch and report, change nothing")
    q.add_argument("--no-fetch", action="store_true", help="use the remote-tracking branches as they are")
    q.add_argument("--tip", action="store_true",
                   help="go to the head of the tracked branch instead, ahead of the recorded pointer: to work in the submodule, or before a bump")
    q.add_argument("--bump", action="store_true",
                   help="implies --tip, then commits each moved pointer in this repository, one commit per submodule")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    missing = lib.list_submodules(ctx.root, missing=True)
    subs = lib.list_submodules(ctx.root)
    if args.paths:
        wanted = {p.replace("\\", "/").rstrip("/") for p in args.paths}
        unknown = wanted - {s.path for s in subs + missing}
        if unknown:
            ctx.die(f"not a submodule: {', '.join(sorted(unknown))}")
        missing = [s for s in missing if s.path in wanted]
        subs = [s for s in subs if s.path in wanted]
    if not subs and not missing:
        print(console.dim("no submodules"), file=sys.stderr)
        sys.exit(0)

    # A submodule just initialized is synced like the others, so it ends on its branch too.
    outcomes = [lib.init(ctx.root, s, dry_run=args.dry_run) for s in missing]
    fresh = [s for s, o in zip(missing, outcomes) if o.status == "initialized"]
    tip = args.tip or args.bump
    outcomes += [lib.sync(ctx.root, s, dry_run=args.dry_run, fetch=not args.no_fetch, tip=tip) for s in subs + fresh]
    # After the sync, so a nested submodule a fast-forward just declared is initialized too.
    for s in subs + fresh:
        outcomes += lib.init_nested(ctx.root, s, dry_run=args.dry_run)
    width = max(len(o.path) for o in outcomes)
    for o in outcomes:
        print(f"  {o.path:<{width}}  {_COLORS[o.status](f"{o.status:<12}")}  {o.detail}")

    if args.bump and not args.dry_run:
        for s in subs + fresh:
            subject = lib.bump(ctx.root, s)
            if subject:
                print(f"  committed: {subject}")
    elif tip and any(o.old != o.new and o.status == "updated" for o in outcomes):
        print(console.dim("these submodules now run ahead of their recorded pointers; commit a bump only for submodule work you pushed, or one you were asked for (--bump)"),
              file=sys.stderr)
    sys.exit(0 if all(o.ok for o in outcomes) else 1)
