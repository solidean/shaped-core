#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["pygments>=2.17", "markdown-it-py>=3"]
# ///

"""A folder-based review of a commit range, answered in a local web UI.

A review is a directory of plain files: a ledger of every change in the range, entries written as typed blocks,
and the maintainer's answers alongside them. Nothing here is a database, and every file is readable and editable by hand.

`uv run review.py --help` is the CLI reference.
tools/review/readme.md is the tool, and tools/review/docs/_index.md the design behind it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

import tools.review as review  # noqa: E402
from tools.review import cmd  # noqa: E402
from tools.review.cmd import (  # noqa: E402
    coverage,
    delta,
    finalize,
    generate,
    ingest,
    init,
    list_reviews,
    round as round_cmd,
    selftest,
    serve,
    show,
    sync,
    validate,
)

# The map of the CLI: one module per command, in the order `review.py --help` lists them.
COMMANDS = [
    init,
    list_reviews,
    ingest,
    coverage,
    generate,
    show,
    validate,
    serve,
    delta,
    round_cmd,
    sync,
    finalize,
    selftest,
]


def _force_utf8_streams() -> None:
    """Windows consoles default to a code page that cannot render the report; ask for UTF-8 and carry on if refused."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def _resolve_repo(spec: str | None) -> Path:
    start = Path(spec).resolve() if spec else Path.cwd()
    if not start.is_dir():
        print(review.console.red(f"ERROR: {start} is not a directory"), file=sys.stderr)
        sys.exit(2)
    try:
        return review.Git(start).toplevel()
    except review.GitError as e:
        print(review.console.red(f"ERROR: {start} is not inside a git repository ({e})"), file=sys.stderr)
        sys.exit(2)


def main() -> None:
    _force_utf8_streams()

    parser = argparse.ArgumentParser(
        prog="review.py",
        description="Review a commit range as a folder of files, answered in a local web UI.",
    )
    parser.add_argument("--repo", default=None, help="the repository to review (default: the current directory's)")
    parser.add_argument("--dir", default=None, help="the review folder (default: <repo>/.tmp/reviews/<name>)")
    parser.add_argument("--colored", action="store_true", help="force ANSI colour")
    parser.add_argument("--plain", action="store_true", help="disable ANSI colour")

    sub = parser.add_subparsers(dest="command", required=True)
    commands = {m.NAME: m for m in COMMANDS}
    for module in COMMANDS:
        module.add_parser(sub)

    args = parser.parse_args()
    review.console.configure("colored" if args.colored else "plain" if args.plain else "auto")

    repo = _resolve_repo(args.repo)
    ctx = cmd.Context(
        repo=repo,
        git=review.Git(repo),
        dir_override=Path(args.dir).resolve() if args.dir else None,
    )
    commands[args.command].run(args, ctx)


if __name__ == "__main__":
    main()
