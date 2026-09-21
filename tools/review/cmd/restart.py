"""`restart` — stop whatever is serving this review, then serve it again.

Entry edits need no restart: the watcher notices them and the open page reloads itself.
What needs one is a change to the *tool* — the page assets, the renderer, the server — which a running process is holding an old copy of.
That happens once per iteration while the tool itself is being worked on, and it was being done by hand
with a shutdown request, a sleep, and a fresh `serve` that silently landed on the next port when the old one had not let go yet.

Returns once the new server answers, unlike `serve`.
The new server is a detached `serve` process of its own, so an agent's shell gets its prompt back instead of blocking until its timeout.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import tools.review as review

from . import args as a
from . import serve as serve_cmd
from . import stop as stop_cmd
from .context import Context

NAME = "restart"

# How long to wait for the old server to release its port before serving again.
# Without this the new one binds the next port up and the maintainer's open tab keeps talking to a corpse.
_RELEASE_TIMEOUT = 5.0
# How long the new server gets to write its marker and answer on the port it names.
_START_TIMEOUT = 10.0
_POLL = 0.2

# review.py, which is what the detached server is started through.
_CLI = Path(__file__).resolve().parents[3] / "review.py"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Stop this review's server, serve it again on the same port, and return")
    a.review_name(p)
    p.add_argument("--port", type=int, default=0, help="port to bind (default: whatever the old server had, else serve's)")
    p.add_argument("--host", default="127.0.0.1", help="address to bind (default 127.0.0.1, local only)")
    p.add_argument("--no-open", action="store_true", help="do not open a browser")
    return p


def spawn_detached(argv: list[str], *, cwd: Path, log: Path) -> subprocess.Popen:
    """Start `argv` so that it outlives this process and holds none of its handles.

    Output goes to `log`, since a detached process has no terminal and a server that died at startup has to say why somewhere.
    stdin is closed for the same reason: inheriting the caller's pipe is what keeps an agent's shell waiting on a process it never sees.
    """
    if os.name == "nt":
        # A new process group keeps the caller's Ctrl+C away, and no window keeps a console from flashing up.
        # Breaking away from the caller's job matters under a runner that kills its job on exit;
        # a job that forbids breakaway refuses the spawn outright, so that flag is tried and then dropped.
        base = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        attempts = [{"creationflags": base | subprocess.CREATE_BREAKAWAY_FROM_JOB}, {"creationflags": base}]
    else:
        attempts = [{"start_new_session": True}]

    with open(log, "wb") as out:
        for index, how in enumerate(attempts):
            try:
                return subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                        close_fds=True, **how)
            except OSError:
                if index == len(attempts) - 1:
                    raise
    raise AssertionError("unreachable: the last attempt either returns or raises")


def wait_until_served(marker: Path, child: subprocess.Popen, *, timeout: float) -> str:
    """The url the new server answers on, or "" when it exited or never came up.

    The marker is what names the url, because `serve` takes the next port up when the one asked for is taken.
    `marker` must not exist when the child is started, so that whatever appears there is the child's.
    The pid inside it cannot settle that: a venv's `python.exe` on Windows is a launcher, and the server is its child.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        served = review.read_json(marker) if marker.is_file() else {}
        url = str(served.get("url", ""))
        if url and serve_cmd.is_up(url):
            return url
        if child.poll() is not None:
            return ""
        time.sleep(_POLL)
    return ""


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

        # Serving before the socket is released takes the next port, which is how an open tab ends up on a dead one.
        deadline = time.monotonic() + _RELEASE_TIMEOUT
        while serve_cmd.is_up(previous) and time.monotonic() < deadline:
            time.sleep(_POLL)
        if serve_cmd.is_up(previous):
            print(review.console.yellow(f"{previous} is still answering; serving on another port"))

    paths.served_marker.unlink(missing_ok=True)
    argv = [sys.executable, str(_CLI), "--dir", str(paths.root), "serve", args.name,
            "--port", str(port or serve_cmd._DEFAULT_PORT), "--host", args.host]
    if args.no_open:
        argv.append("--no-open")

    child = spawn_detached(argv, cwd=ctx.home, log=paths.serve_log)
    url = wait_until_served(paths.served_marker, child, timeout=_START_TIMEOUT)
    if not url:
        if child.poll() is None:
            # Left running it would come up later on a port nobody was told about.
            child.kill()
            child.wait(timeout=5)
            state = f"did not answer within {_START_TIMEOUT:.0f}s and was stopped"
        else:
            state = f"exited with {child.returncode}"
        said = paths.serve_log.read_text(encoding="utf-8", errors="replace").strip()
        ctx.die(f"the new server {state}" + (f":\n{said}" if said else ""))

    print(f"review {args.name} at {url}")
    sys.stdout.flush()
