"""Telling the browser that the review folder moved under it.

One poll thread computes a digest over the files a page depends on, and pushes it to every open tab.
Entries and answers are hashed by content rather than by mtime and size, because an agent rewriting an entry
within the same clock tick, to the same length, is an ordinary thing for an agent to do.

A tab holds exactly one connection, and each one pins a thread, so the count is capped rather than trusted.
"""

from __future__ import annotations

import queue
import threading
import time
from pathlib import Path

from ..core.atomic import stat_key
from ..core.paths import ReviewPaths
from ..render.highlight import digest as hash_text

POLL_SECONDS = 0.5
HEARTBEAT_SECONDS = 15.0
MAX_CLIENTS = 8


def compute_digest(paths: ReviewPaths) -> str:
    """A short digest of everything a rendered page depends on."""
    parts: list[str] = []
    for file in paths.entry_files():
        try:
            parts.append(file.name + hash_text(file.read_text(encoding="utf-8", errors="replace")))
        except OSError:
            parts.append(file.name + "?")
    if paths.answers_dir.is_dir():
        for file in sorted(paths.answers_dir.glob("*.json")):
            try:
                parts.append(file.name + hash_text(file.read_text(encoding="utf-8", errors="replace")))
            except OSError:
                parts.append(file.name + "?")
    for cheap in (paths.ledger, paths.config):
        parts.append(f"{cheap.name}{stat_key(cheap)}")
    return hash_text("\x1f".join(parts))


class Watcher:
    """Polls the review folder and fans changes out to the open tabs."""

    def __init__(self, paths: ReviewPaths) -> None:
        self.paths = paths
        self.digest = compute_digest(paths)
        self._clients: list[queue.Queue] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, name="review-watch", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def subscribe(self) -> queue.Queue | None:
        """A queue of events for one tab, or None when the connection cap is reached."""
        with self._lock:
            if len(self._clients) >= MAX_CLIENTS:
                return None
            client: queue.Queue = queue.Queue(maxsize=16)
            self._clients.append(client)
            return client

    def unsubscribe(self, client: queue.Queue) -> None:
        with self._lock:
            if client in self._clients:
                self._clients.remove(client)

    def _publish(self, payload: str) -> None:
        with self._lock:
            clients = list(self._clients)
        for client in clients:
            try:
                client.put_nowait(payload)
            except queue.Full:
                # A tab that has stopped reading gets dropped rather than blocking the poll thread.
                self.unsubscribe(client)

    def _loop(self) -> None:
        last_beat = time.monotonic()
        while not self._stop.wait(POLL_SECONDS):
            try:
                current = compute_digest(self.paths)
            except Exception:
                # The poll thread outliving a transient read failure is the whole point of catching here.
                continue
            if current != self.digest:
                self.digest = current
                self._publish(f"event: reload\ndata: {current}\n\n")
                last_beat = time.monotonic()
            elif time.monotonic() - last_beat > HEARTBEAT_SECONDS:
                self._publish(": ping\n\n")
                last_beat = time.monotonic()
