"""`stop` — shut a review's server down.

`serve` is normally started by an agent in the background, so there is no terminal to interrupt and no pid to hand out.
The `.served` marker records where the server is, and asking it to stop over its own HTTP surface works everywhere
a signal would need platform-specific handling.
"""

from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request

import tools.review as review

from . import args as a
from .context import Context

NAME = "stop"

_TIMEOUT = 5


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Stop the server for a review")
    a.review_name(p)
    return p


def stop_at(url: str) -> bool:
    """Ask the server at `url` to stop; True when it acknowledged."""
    request = urllib.request.Request(
        url.rstrip("/") + "/api/shutdown",
        data=json.dumps({}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=_TIMEOUT) as response:
            return response.status == 200
    except (urllib.error.URLError, OSError):
        return False


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    served = review.read_json(paths.served_marker)
    url = str(served.get("url", ""))

    if not url:
        print(f"no server recorded for {cfg.name}")
        return

    if stop_at(url):
        print(f"stopped {cfg.name} at {url}")
    else:
        # The marker outlives a server that was killed rather than asked to stop, so a stale one is not an error.
        print(review.console.yellow(f"nothing answering at {url}; clearing the stale marker"))
    paths.served_marker.unlink(missing_ok=True)
