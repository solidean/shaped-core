"""`round` — wait for the maintainer to hand the round back.

The agent's shell caps a foreground command well below how long a review takes, so the server runs in the background
and this is the blocking half: a poll on a one-shot mailbox the `Send to Claude` button writes.

The exit code is the answer.
A round that was sent and a review that was paused are different outcomes, and an agent that cannot tell them apart
would either stop early or keep waiting on someone who has walked away.
"""

from __future__ import annotations

import argparse
import time

import tools.review as review

from . import args as a
from .context import Context
from . import delta as delta_cmd

NAME = "round"

EXIT_SENT = 0
EXIT_ERROR = 1
EXIT_PAUSED = 2
EXIT_TIMEOUT = 3

_POLL_SECONDS = 0.25
_DEFAULT_TIMEOUT = 5400


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Block until the round is handed back, then print it")
    a.review_name(p)
    p.add_argument("--wait", action="store_true", default=True, help="block until the page signals (the default)")
    p.add_argument("--timeout", type=int, default=_DEFAULT_TIMEOUT,
                   help=f"seconds to wait before giving up (default {_DEFAULT_TIMEOUT})")
    p.add_argument("--no-finalize", action="store_true",
                   help="print the round without freezing it, for a look mid-review")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    paths.signal.unlink(missing_ok=True)

    print(review.console.dim(f"waiting for round {cfg.next_round} of {cfg.name} ..."), flush=True)
    deadline = time.monotonic() + args.timeout

    while time.monotonic() < deadline:
        if paths.signal.is_file():
            payload = review.read_json(paths.signal)
            paths.signal.unlink(missing_ok=True)
            action = str(payload.get("action", "send"))

            if action == "pause":
                review.record(paths.log, "pause", round=cfg.next_round)
                print(review.console.yellow("paused: answers are kept, nothing was finalized"))
                raise SystemExit(EXIT_PAUSED)

            forwarded = argparse.Namespace(
                name=args.name, finalize=not args.no_finalize, round=0,
            )
            delta_cmd.run(forwarded, ctx)
            raise SystemExit(EXIT_SENT)

        time.sleep(_POLL_SECONDS)

    print(review.console.yellow(f"timed out after {args.timeout}s; the review is untouched and still open"))
    raise SystemExit(EXIT_TIMEOUT)
