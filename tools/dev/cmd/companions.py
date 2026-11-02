"""`companions` — clone, fast-forward and list the opt-in repositories dev.py's COMPANIONS table declares.

The rules are tools/dev/lib/project/companions.py's docstring; docs/guides/building-and-testing.md has the workflow.
"""

from __future__ import annotations

import argparse
import sys

from tools.dev import console
from tools.dev.lib.project import companions as lib

from .context import Context

NAME = "companions"

_COLORS = {"initialized": console.green, "would init": console.green, "updated": console.green, "would update": console.green,
           "up-to-date": console.dim, "skipped": console.yellow, "refused": console.red}


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Clone and fast-forward the opt-in companion repositories, kept at gitignored paths in this tree")
    s = p.add_subparsers(dest="companions_cmd", required=True, metavar="<command>")
    s.add_parser("list", help="every companion, its path, and where its clone stands (no network)")
    q = s.add_parser("init", help="clone a companion on its branch; one already cloned is left alone")
    q.add_argument("names", nargs="+", help="the companions to clone")
    q.add_argument("--url", help="clone from here instead, say over https where no SSH key is set up; only with one name")
    q.add_argument("--dry-run", action="store_true", help="report, clone nothing")
    q = s.add_parser("sync", help="fast-forward each cloned companion to its branch head, never touching local work")
    q.add_argument("names", nargs="*", help="only these companions (default: every cloned one)")
    q.add_argument("--dry-run", action="store_true", help="fetch and report, change nothing")
    q.add_argument("--no-fetch", action="store_true", help="use the remote-tracking branches as they are")
    return p


def _select(ctx: Context, names: list[str]) -> list[lib.Companion]:
    known = {c.name: c for c in ctx.policy.companions}
    unknown = [n for n in names if n not in known]
    if unknown:
        ctx.die(f"not a companion: {', '.join(unknown)}; known: {', '.join(known) or 'none'}")
    return [known[n] for n in names] if names else list(ctx.policy.companions)


def _print(outcomes: list[lib.Outcome]) -> None:
    width = max(len(o.path) for o in outcomes)
    for o in outcomes:
        print(f"  {o.path:<{width}}  {_COLORS[o.status](f"{o.status:<12}")}  {o.detail}")


def run(args: argparse.Namespace, ctx: Context) -> None:
    if not ctx.policy.companions:
        print(console.dim("no companions declared"), file=sys.stderr)
        sys.exit(0)

    match args.companions_cmd:
        case "list":
            width = max(len(c.name) for c in ctx.policy.companions)
            for c in ctx.policy.companions:
                print(f"  {c.name:<{width}}  {c.path}  {lib.describe(ctx.root, c)}")
                if c.purpose:
                    print(f"  {'':<{width}}  {console.dim(c.purpose)}")
            sys.exit(0)
        case "init":
            if args.url and len(args.names) != 1:
                ctx.die("--url names one clone source, so it takes exactly one companion")
            outcomes = [lib.init(ctx.root, c, url=args.url, dry_run=args.dry_run) for c in _select(ctx, args.names)]
        case "sync":
            selected = _select(ctx, args.names)
            # Asking for a companion by name and finding it absent is worth a line; the default sweep just passes it by.
            if not args.names:
                selected = [c for c in selected if lib.is_cloned(ctx.root, c)]
                if not selected:
                    print(console.dim("no companion is cloned; `companions list` shows them"), file=sys.stderr)
                    sys.exit(0)
            outcomes = [lib.sync(ctx.root, c, dry_run=args.dry_run, fetch=not args.no_fetch) for c in selected]
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown companions subcommand {args.companions_cmd!r}")
    _print(outcomes)
    sys.exit(0 if all(o.ok for o in outcomes) else 1)
