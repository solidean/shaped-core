"""`restart` — stop whatever is serving this review, then serve it again.

Entry edits need no restart: the watcher notices them and the open page reloads itself.
What needs one is a change to the *tool* — the page assets, the renderer, the server — which a running process is holding an old copy of.
That happens once per iteration while the tool itself is being worked on, and it was being done by hand
with a shutdown request, a sleep, and a fresh `serve` that silently landed on the next port when the old one had not let go yet.

Blocks exactly like `serve`, because it becomes `serve`.
"""

from __future__ import annotations

import argparse
import time

import tools.review as review

from . import args as a
from . import serve as serve_cmd
from . import stop as stop_cmd
from .context import Context

NAME = "restart"

# How long to wait for the old server to release its port before serving again.
# Without this the new one binds the next port up and the maintainer's open tab keeps talking to a corpse.
_RELEASE_TIMEOUT = 5.0
_POLL = 0.2


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Stop this review's server and serve it again on the same port")
    a.review_name(p)
    p.add_argument("--port", type=int, default=0, help="port to bind (default: whatever the old server had, else serve's)")
    p.add_argument("--host", default="127.0.0.1", help="address to bind (default 127.0.0.1, local only)")
    p.add_argument("--no-open", action="store_true", help="do not open a browser")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, _ = ctx.open(args.name)
    served = review.read_json(paths.served_marker)
    previous = str(served.get("url", ""))

    port = args.port
    if not port and previous:
        try:
            port = int(previous.rstrip("/").rsplit(":", 1)[1])
        except (IndexError, ValueError):
            port = 0

    if previous:
        if stop_cmd.stop_at(previous):
            print(review.console.dim(f"stopped {previous}"))
        paths.served_marker.unlink(missing_ok=True)

        # Serving before the socket is released takes the next port, which is how an open tab ends up on a dead one.
        deadline = time.monotonic() + _RELEASE_TIMEOUT
        while serve_cmd.is_up(previous) and time.monotonic() < deadline:
            time.sleep(_POLL)
        if serve_cmd.is_up(previous):
            print(review.console.yellow(f"{previous} is still answering; serving on another port"))

    forwarded = argparse.Namespace(
        name=args.name,
        port=port or serve_cmd._DEFAULT_PORT,
        host=args.host,
        no_open=args.no_open,
    )
    serve_cmd.run(forwarded, ctx)
