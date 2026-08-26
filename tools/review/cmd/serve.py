"""`serve` — the local page the review is answered in.

Non-blocking by design: the maintainer browses whenever, and answers save themselves as they are typed.
Handing the round back is a separate, deliberate act — the `Send to Claude` button, which `review round --wait` is watching for.
"""

from __future__ import annotations

import argparse
import os
import socket
import sys
import threading
import urllib.parse
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


def is_up(url: str) -> bool:
    """Whether anything still answers at `url`.

    A `serve` that is killed rather than asked to stop leaves its `.served` marker behind, and nothing revisits it.
    Quoting a dead url as though it were live is a whole round spent answering in a tab that saves nowhere,
    so every reader of the marker checks before believing it.
    """
    if not url:
        return False
    parsed = urllib.parse.urlparse(url)
    try:
        with socket.create_connection((parsed.hostname or "127.0.0.1", parsed.port or 80), timeout=0.4):
            return True
    except OSError:
        return False


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


def _warn_about_other_servers(ctx: Context, name: str) -> None:
    """Name any other review in this repo that already has a server up.

    Two servers on one machine is normal; not knowing which one a browser tab is showing is not,
    and that is a whole round of confusion when the answer is "a different review".
    """
    others = [p.name for p in _served_elsewhere(ctx, name)]
    if others:
        print(review.console.yellow(
            f"  note: {', '.join(others)} {'is' if len(others) == 1 else 'are'} also being served from this repo"
        ))


def _served_elsewhere(ctx: Context, name: str) -> list:
    root = review.reviews_root(ctx.home)
    if not root.is_dir():
        return []
    out = []
    for folder in sorted(p for p in root.iterdir() if p.is_dir() and p.name != name):
        marker = review.ReviewPaths(folder).served_marker
        if marker.is_file() and is_up(str(review.read_json(marker).get("url", ""))):
            out.append(folder)
    return out


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    server, port = start(ctx, args.name, host=args.host, port=args.port)
    url = f"http://{args.host}:{port}/"

    review.write_json(paths.served_marker, {"url": url, "pid": os.getpid(), "at": review.now()})
    review.record(paths.log, "serve", url=url, host=args.host)
    print(f"review {cfg.name} at {url}")
    _warn_about_other_servers(ctx, args.name)
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        # Reachable from the network is the point of --host, and there is no authentication of any kind.
        print(review.console.yellow(
            f"  warning: bound to {args.host}, so anyone who can reach this machine can read and answer this review"
        ))
    print(f"  round {cfg.next_round}, {len(paths.entry_files())} entries")
    print("  answers save as you type; `Send to Claude` finalizes the round")

    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    # An agent runs this in the background, where Python block-buffers stdout to a pipe.
    # Without the flush the lines above arrive only when the server exits, so a wrong port or a wrong review is invisible
    # to whoever is driving — which is a whole round spent answering another review's entries.
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()
        paths.served_marker.unlink(missing_ok=True)
