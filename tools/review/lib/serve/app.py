"""The routes the review page is made of.

Nothing here is authoritative in memory.
Every request re-reads the review folder, which is what lets the agent rewrite an entry, or the tool be restarted,
while a tab stays open — and what makes the browser's view of the review always the folder's view.

No request may fail on a malformed entry.
A parse error renders as a panel with the line number and the remedy, and the navigation keeps working,
because a broken entry is the moment the maintainer most needs to see the rest of the review.
"""

from __future__ import annotations

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

from ..changeset.ledger import Ledger
from ..core import config as config_module
from ..core.atomic import write_json
from ..core.log import record
from ..core.paths import ReviewPaths
from ..entry.answers import AnswerFile
from ..entry.askhash import hash_ask
from ..entry.grammar import ReviewParseError
from ..entry.parse import Entry, parse_file
from ..goals.skeleton import describe, groups_for
from ..render.entryview import render_entry
from ..render.highlight import css as highlight_css
from .watch import Watcher, compute_digest

ASSETS = Path(__file__).resolve().parents[2] / "assets"

# One lock per entry, so two autosaves against the same answers file cannot interleave.
_answer_locks: dict[str, threading.Lock] = {}
_locks_guard = threading.Lock()


def _lock_for(slug: str) -> threading.Lock:
    with _locks_guard:
        return _answer_locks.setdefault(slug, threading.Lock())


class ReviewApp:
    """Everything a request needs, rebuilt from disk on each one."""

    def __init__(self, repo: Path, paths: ReviewPaths, watcher: Watcher) -> None:
        self.repo = repo
        self.paths = paths
        self.watcher = watcher

    def config(self):
        return config_module.load(self.paths.config)

    def ledger(self) -> Ledger:
        return Ledger.load(self.paths.ledger)

    def entries(self) -> list[tuple[Path, Entry | None, ReviewParseError | None]]:
        out = []
        for file in self.paths.entry_files():
            try:
                out.append((file, parse_file(file), None))
            except ReviewParseError as e:
                out.append((file, None, e))
        return out

    def answers_for(self, entry: Entry) -> AnswerFile:
        return AnswerFile.load(self.paths.answers_for(entry.path), entry.slug)

    def state(self) -> dict:
        cfg = self.config()
        ledger = self.ledger()
        groups = list(groups_for(cfg.goals))

        rows = []
        discharged: set[str] = set()
        total_asks = answered_asks = 0
        for file, entry, error in self.entries():
            if error is not None:
                rows.append({
                    "slug": file.stem, "id": file.stem.split("-")[0], "title": file.stem,
                    "group": "broken", "state": "broken", "severity": "",
                    "asks": 0, "answered": 0, "error": str(error),
                })
                continue
            answers = self.answers_for(entry)
            asks = entry.asks
            answered = sum(1 for b in asks if (a := answers.get(b.name)) and not a.is_empty)
            if entry.state == "open":
                discharged.update(entry.discharged_changes())
                total_asks += len(asks)
                answered_asks += answered
            rows.append({
                "slug": entry.slug, "id": entry.id, "title": entry.title,
                "group": entry.group, "state": entry.state, "severity": entry.severity,
                "asks": len(asks), "answered": answered, "error": "",
            })

        live = ledger.live()
        covered = {c.id for c in live if c.id in discharged or c.discharged_by_reason}
        return {
            "name": cfg.name,
            "title": cfg.title,
            "goals": cfg.goals,
            "round": cfg.next_round,
            "watermark": cfg.watermark,
            "range": f"{cfg.base_spec}..{cfg.head_spec}" if cfg.base else "",
            "groups": [{"name": g, "description": describe(g)} for g in groups],
            "entries": rows,
            "progress": {
                "asks": total_asks, "answered": answered_asks,
                "changes": len(live), "discharged": len(covered),
            },
            "digest": self.watcher.digest,
        }

    def entry_html(self, slug: str) -> tuple[int, dict]:
        for file, entry, error in self.entries():
            if file.stem != slug:
                continue
            if error is not None:
                return 200, {"slug": slug, "html": _error_panel(error), "broken": True}
            answers = self.answers_for(entry)
            html = render_entry(
                entry, answers,
                repo=self.repo, paths=self.paths, ledger=self.ledger(), hash_of=hash_ask,
            )
            return 200, {"slug": slug, "html": html, "broken": False}
        return 404, {"error": f"no entry {slug!r}"}

    def save_answer(self, payload: dict) -> tuple[int, dict]:
        slug = str(payload.get("entry", ""))
        ask_name = str(payload.get("ask", ""))
        client_hash = str(payload.get("hash", ""))

        target = next((f for f in self.paths.entry_files() if f.stem == slug), None)
        if target is None:
            return 410, {"error": "that entry no longer exists"}

        with _lock_for(slug):
            try:
                entry = parse_file(target)
            except ReviewParseError as e:
                return 409, {"error": str(e)}

            answers = AnswerFile.load(self.paths.answers_for(target), slug)
            block = entry.ask(ask_name)
            if block is None:
                orphan = answers.orphan(ask_name, client_hash)
                answers.save()
                return 410, {
                    "error": "that question was removed from the entry; your answer is kept as an orphan",
                    "orphaned": bool(orphan),
                }

            current_hash = hash_ask(block)
            cfg = self.config()

            # A tab left open across a `Send to Claude` still shows the old round's forms, and they still look answerable.
            # Writing them would file the answer under the *next* round, silently, against a question already handed back.
            client_round = payload.get("round")
            if client_round is not None and int(client_round) != cfg.next_round:
                return 409, {
                    "stale": True,
                    "error": f"this window is showing round {int(client_round)}, which has been handed back",
                    "round": cfg.next_round,
                }

            answer = answers.upsert(
                block,
                selected=[str(s) for s in payload.get("selected", [])],
                text=str(payload.get("text", "")),
                round_number=cfg.next_round,
            )
            answers.save()

            # The digest this write produced, so the tab can recognise the reload it is about to cause as its own.
            # Without it the watcher tells every tab that answers/ moved, and the one that moved it re-renders itself.
            written_digest = compute_digest(self.paths)

            if client_hash and client_hash != current_hash:
                return 409, {
                    "error": "the question changed while you were answering it",
                    "hash": current_hash,
                    "answer": answer.to_record(),
                    "digest": written_digest,
                }
            return 200, {"answer": answer.to_record(), "hash": current_hash, "digest": written_digest}

    def save_comment(self, payload: dict) -> tuple[int, dict]:
        """Add, edit or delete one comment.

        A comment is maintainer-authored, so it is server-owned and never spliced into an entry file —
        it lives in the answers file, which is the half of the split the server already writes.
        """
        slug = str(payload.get("entry", ""))
        target = next((f for f in self.paths.entry_files() if f.stem == slug), None)
        if target is None:
            return 410, {"error": "that entry no longer exists"}

        with _lock_for(slug):
            answers = AnswerFile.load(self.paths.answers_for(target), slug)
            cfg = self.config()
            comment = answers.comment(
                comment_id=str(payload.get("id", "")),
                text=str(payload.get("text", "")),
                block=str(payload.get("block", "")),
                change=str(payload.get("change", "")),
                offset=int(payload.get("offset", -1)),
                round_number=cfg.next_round,
            )
            answers.save()
            return 200, {
                "comment": comment.to_record() | {"id": comment.id} if comment else None,
                "digest": compute_digest(self.paths),
            }

    def summary(self) -> dict:
        """Every ask and what it is about to hand back, for the confirmation the send button shows.

        The count that matters is `offered` against `taken` for the check options.
        An unticked checkbox is a valid answer and is indistinguishable from one nobody read,
        so this is the one place the difference can still be caught — by the person who would know.
        """
        rows = []
        for file in self.paths.entry_files():
            try:
                entry = parse_file(file)
            except ReviewParseError:
                continue
            answers = AnswerFile.load(self.paths.answers_for(file), file.stem)
            asks = []
            for block in entry.asks:
                answer = answers.get(block.name)
                selected = list(answer.selected) if answer else []
                checks = [o.label for o in block.options if o.kind == "check"]
                asks.append({
                    "name": block.name,
                    "answered": bool(answer) and not answer.is_empty,
                    "finalized": bool(answer) and not answer.tentative,
                    "selected": selected,
                    "text": (answer.text.strip() if answer else ""),
                    "checks_offered": len(checks),
                    "checks_taken": sum(1 for c in checks if c in selected),
                })
            if asks:
                rows.append({"entry": entry.slug, "title": entry.title, "asks": asks})
        return {"round": self.config().next_round, "entries": rows}

    def shutdown(self) -> tuple[int, dict]:
        """Stop the server, from the page or from `review stop`.

        The agent usually starts `serve` in the background, so the maintainer has no terminal to interrupt —
        without this the only way to get the port back is finding the process.
        `shutdown()` must not run on a handler thread, since it waits for the serve loop this request is inside.
        """
        record(self.paths.log, "shutdown")
        return 200, {"stopped": True}

    def signal(self, payload: dict) -> tuple[int, dict]:
        action = str(payload.get("action", "send"))
        if action not in ("send", "pause"):
            return 400, {"error": f"unknown action {action!r}"}
        cfg = self.config()
        write_json(self.paths.signal, {"action": action, "round": cfg.next_round, "at": time.time()})
        record(self.paths.log, "signal", signal=action, round=cfg.next_round)
        return 200, {"action": action, "round": cfg.next_round}


def _error_panel(error: ReviewParseError) -> str:
    from html import escape
    return (
        '<article class="entry"><header class="entry-head"><h1>This entry does not parse</h1></header>'
        f'<pre class="parse-error">{escape(str(error))}</pre>'
        "<p>The rest of the review still works; fix the file and this page refreshes itself.</p></article>"
    )


class Handler(BaseHTTPRequestHandler):
    """Routes.

    The app hangs off the server rather than the handler, since a handler is constructed per request.
    """

    server_version = "review"
    protocol_version = "HTTP/1.1"

    @property
    def app(self) -> ReviewApp:
        return self.server.app

    def log_message(self, fmt: str, *rest) -> None:
        """Quiet by default: the terminal running the server is also the one reading the round output."""

    def _send(self, code: int, body: bytes, content_type: str, *, cache: bool = False) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "public, max-age=3600" if cache else "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload: dict) -> None:
        self._send(code, json.dumps(payload).encode("utf-8"), "application/json; charset=utf-8")

    def _asset(self, name: str) -> None:
        target = (ASSETS / name).resolve()
        if not target.is_file() or ASSETS not in target.parents:
            self._json(404, {"error": "no such asset"})
            return
        types = {".html": "text/html", ".css": "text/css", ".js": "text/javascript"}
        content_type = types.get(target.suffix, "application/octet-stream") + "; charset=utf-8"
        self._send(200, target.read_bytes(), content_type)

    def do_GET(self) -> None:  # noqa: N802 — the base class names it.
        route = urlparse(self.path).path
        try:
            if route in ("/", "/index.html"):
                self._asset("page.html")
            elif route == "/assets/highlight.css":
                self._send(200, highlight_css().encode("utf-8"), "text/css; charset=utf-8")
            elif route.startswith("/assets/"):
                self._asset(unquote(route[len("/assets/"):]))
            elif route == "/api/state":
                self._json(200, self.app.state())
            elif route == "/api/summary":
                self._json(200, self.app.summary())
            elif route.startswith("/api/entry/"):
                code, payload = self.app.entry_html(unquote(route[len("/api/entry/"):]))
                self._json(code, payload)
            elif route == "/events":
                self._events()
            else:
                self._json(404, {"error": "no such route"})
        except BrokenPipeError:
            pass
        except Exception as e:  # noqa: BLE001 — a handler that raises would take the tab down with it.
            self._json(500, {"error": f"{type(e).__name__}: {e}"})

    def do_POST(self) -> None:  # noqa: N802 — the base class names it.
        route = urlparse(self.path).path
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length) or b"{}")
        except (ValueError, json.JSONDecodeError):
            self._json(400, {"error": "malformed request body"})
            return

        try:
            if route == "/api/answer":
                code, result = self.app.save_answer(payload)
            elif route == "/api/comment":
                code, result = self.app.save_comment(payload)
            elif route == "/api/signal":
                code, result = self.app.signal(payload)
            elif route == "/api/shutdown":
                code, result = self.app.shutdown()
                self._json(code, result)
                self.wfile.flush()
                # Off the handler thread on purpose: `shutdown` waits for the loop this request is running inside.
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                code, result = 404, {"error": "no such route"}
            self._json(code, result)
        except BrokenPipeError:
            pass
        except Exception as e:  # noqa: BLE001 — see do_GET.
            self._json(500, {"error": f"{type(e).__name__}: {e}"})

    def _events(self) -> None:
        client = self.app.watcher.subscribe()
        if client is None:
            self._json(503, {"error": "too many open tabs watching this review"})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            self.wfile.write(f"event: reload\ndata: {self.app.watcher.digest}\n\n".encode("utf-8"))
            self.wfile.flush()
            while True:
                payload = client.get()
                self.wfile.write(payload.encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.app.watcher.unsubscribe(client)


class Server(ThreadingHTTPServer):
    daemon_threads = True

    # `HTTPServer` turns this on, and on Windows SO_REUSEADDR lets a bind to a port another process is already
    # listening on *succeed* — so the port fallback never fires, two servers hold one port, and the OS keeps
    # routing the browser to whichever bound first.
    # A second review then silently serves the first one's entries.
    allow_reuse_address = False

    def __init__(self, address, app: ReviewApp) -> None:
        super().__init__(address, Handler)
        self.app = app
