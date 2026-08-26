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
    # `--wait` used to be a flag that could not turn anything off, which read as though a non-blocking form existed.
    # This keeps `--wait` valid and gives it the `--no-wait` it implied.
    p.add_argument("--wait", action=argparse.BooleanOptionalAction, default=True,
                   help="block until the page signals (default: block)")
    p.add_argument("--timeout", type=int, default=_DEFAULT_TIMEOUT,
                   help=f"seconds to wait before giving up (default {_DEFAULT_TIMEOUT})")
    p.add_argument("--no-finalize", action="store_true",
                   help="print the round without freezing it, for a look mid-review")
    return p


def _pending(paths: review.ReviewPaths, round_number: int) -> dict | None:
    """A signal already waiting when `round` starts, if it belongs to the round about to be collected.

    Discarding one unread is how an hour of answering evaporates: the button was pressed with no waiter up,
    which is the normal order when the maintainer answers before the agent comes back to listen.
    A signal from an earlier round is stale and must not auto-fire, and that is what the round number separates.
    """
    if not paths.signal.is_file():
        return None
    payload = review.read_json(paths.signal)
    if int(payload.get("round", 0)) != round_number:
        paths.signal.unlink(missing_ok=True)
        return None
    return payload


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    waiting = _pending(paths, cfg.next_round)

    # Naming the review and the page it is waiting on, because an agent that has the wrong one otherwise finds out
    # only after the maintainer has spent a round answering someone else's entries.
    served = review.read_json(paths.served_marker)
    where = f" at {served['url']}" if served.get("url") else " (no server running — `review serve` first)"
    if waiting is None:
        print(review.console.dim(f"waiting for round {cfg.next_round} of {cfg.name}{where} ..."), flush=True)
    else:
        print(review.console.dim(f"round {cfg.next_round} of {cfg.name} was already handed back; collecting it"), flush=True)

    if waiting is not None:
        paths.signal.unlink(missing_ok=True)
        raise SystemExit(_collect(args, ctx, paths, cfg, waiting))

    if not args.wait:
        print("not waiting; run without --no-wait to block")
        raise SystemExit(EXIT_TIMEOUT)

    deadline = time.monotonic() + args.timeout

    while time.monotonic() < deadline:
        if paths.signal.is_file():
            payload = review.read_json(paths.signal)
            paths.signal.unlink(missing_ok=True)
            raise SystemExit(_collect(args, ctx, paths, cfg, payload))

        time.sleep(_POLL_SECONDS)

    print(review.console.yellow(f"timed out after {args.timeout}s; the review is untouched and still open"))
    raise SystemExit(EXIT_TIMEOUT)


def _collect(args: argparse.Namespace, ctx: Context, paths, cfg, payload: dict) -> int:
    """Act on one signal: a pause keeps everything and freezes nothing, a send finalizes the round."""
    if str(payload.get("action", "send")) == "pause":
        review.record(paths.log, "pause", round=cfg.next_round)
        print(review.console.yellow("paused: answers are kept, nothing was finalized"))
        return EXIT_PAUSED

    forwarded = argparse.Namespace(name=args.name, finalize=not args.no_finalize, round=0)
    delta_cmd.run(forwarded, ctx)
    return EXIT_SENT
