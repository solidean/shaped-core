"""`self-test` — run the tool's own tests.

The same suite `dev.py check` gates on, reachable from the tool itself so a downstream user has one entry point rather than two.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

from .context import Context

NAME = "self-test"

_SCRIPT = Path(__file__).resolve().parents[1] / "review-self-test.py"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run the review tool's own tests")
    p.add_argument("-v", "--verbose", action="store_true", help="name every test as it passes")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if not _SCRIPT.is_file():
        ctx.die(f"{_SCRIPT} is missing")

    spec = importlib.util.spec_from_file_location("review_self_test", _SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.VERBOSE = args.verbose
    sys.exit(module.main())
