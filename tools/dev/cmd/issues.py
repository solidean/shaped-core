"""`issues` — the in-repo issue tracker, tools/issues/issues.py, wired into dev.py.

The tracker is a standalone script so a repository without dev.py can run it too; this module only hands it a subparser.
`gate` is its `check` gate, shared by every dev.py that registers one.
docs/guides/issue-tracking.md is the format and the workflow.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools import dev
from tools.issues import issues as tracker

from .context import Context

NAME = "issues"

_SCRIPT = Path(__file__).resolve().parents[2] / "issues" / "issues.py"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="In-repo issue tracking: new, list, show, known, claim, release, close, check, serve")
    tracker.add_arguments(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    sys.exit(tracker.run(args, cwd=Path.cwd()))


def gate(ctx: Context, *, mirror: bool, verbose: bool) -> bool:
    """`issues check` over the repository and the submodules its .issues.toml lists; always repo-wide, since a deleted file breaks a ref elsewhere."""
    result = dev.run_step(
        ["uv", "run", str(_SCRIPT), "check"],
        step_type="check", name="issues",
        build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
    )
    return result.ok
