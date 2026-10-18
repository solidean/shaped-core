"""`submodules` — keep the submodules on the branches `.gitmodules` names, without risking local work.

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
           "up-to-date": console.dim, "skipped": console.yellow, "refused": console.red}


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Fast-forward the submodules to the branches .gitmodules names, never touching local work")
    s = p.add_subparsers(dest="submodules_cmd", required=True, metavar="<command>")
    q = s.add_parser("sync", help="init the missing submodules, fetch, fast-forward every clean one on (or detached within) its tracked branch, then init their missing submodules")
    q.add_argument("paths", nargs="*", help="only these submodules (default: all)")
    q.add_argument("--dry-run", action="store_true", help="fetch and report, change nothing")
    q.add_argument("--no-fetch", action="store_true", help="use the remote-tracking branches as they are")
    q.add_argument("--bump", action="store_true",
                   help="commit each moved pointer in this repository, one commit per submodule")
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
    outcomes += [lib.sync(ctx.root, s, dry_run=args.dry_run, fetch=not args.no_fetch) for s in subs + fresh]
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
    elif any(o.old != o.new and o.status == "updated" for o in outcomes):
        print(console.dim("the superproject now records new submodule commits; bump each in a commit of its own, or rerun with --bump"),
              file=sys.stderr)
    sys.exit(0 if all(o.ok for o in outcomes) else 1)
