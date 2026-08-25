"""`serve` — the local page the review is answered in.

Non-blocking by design: the maintainer browses whenever, and answers save themselves as they are typed.
Handing the round back is a separate, deliberate act — the `Send to Claude` button, which `review round --wait` is watching for.
"""

from __future__ import annotations

import argparse
import threading
import webbrowser

import tools.review as review

from . import args as a
from .context import Context

NAME = "serve"

_DEFAULT_PORT = 7801
_PORT_ATTEMPTS = 20


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Serve the review page (non-blocking)")
    a.review_name(p)
    p.add_argument("--port", type=int, default=_DEFAULT_PORT, help=f"port to bind (default {_DEFAULT_PORT})")
    p.add_argument("--host", default="127.0.0.1", help="address to bind (default 127.0.0.1, local only)")
    p.add_argument("--no-open", action="store_true", help="do not open a browser")
    return p


def start(ctx: Context, name: str, *, host: str, port: int) -> tuple[object, int]:
    """Bind and start a server for `name`, returning it and the port it actually took."""
    from tools.review.lib.serve.app import ReviewApp, Server
    from tools.review.lib.serve.watch import Watcher

    paths, _ = ctx.open(name)
    watcher = Watcher(paths)
    app = ReviewApp(ctx.repo, paths, watcher)

    last_error: OSError | None = None
    for offset in range(_PORT_ATTEMPTS):
        try:
            server = Server((host, port + offset), app)
        except OSError as e:
            last_error = e
            continue
        watcher.start()
        return server, port + offset

    ctx.die(f"no free port in {port}..{port + _PORT_ATTEMPTS - 1} ({last_error})")
    raise SystemExit(1)


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    server, port = start(ctx, args.name, host=args.host, port=args.port)
    url = f"http://{args.host}:{port}/"

    review.record(paths.log, "serve", url=url)
    print(f"review {cfg.name} at {url}")
    print(f"  round {cfg.next_round}, {len(paths.entry_files())} entries")
    print("  answers save as you type; `Send to Claude` finalizes the round")

    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()
