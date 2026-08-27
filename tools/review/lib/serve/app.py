"""The routes the review page is made of.

Nothing here is authoritative in memory.
Every request re-reads the review folder, which is what lets the agent rewrite an entry, or the tool be restarted,
while a tab stays open — and what makes the browser's view of the review always the folder's view.

No request may fail on a malformed entry.
A parse error renders as a panel with the line number and the remedy, and the navigation keeps working,
because a broken entry is the moment the maintainer most needs to see the rest of the review.
"""

from __future__ import annotations

import hashlib
import json
import re
import threading
import time
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

from ..annotate.index import RepoIndex
from ..annotate.table import build as build_tokens, glossary_terms, to_json as tokens_to_json
from ..changeset.ledger import Ledger
from ..core import config as config_module
from ..core.atomic import stat_key, write_json
from ..core.log import record
from ..core.paths import ReviewPaths
from ..entry.answers import AnswerFile
from ..entry.askhash import hash_ask
from ..entry.generate import POPOVER_ROWS, _tree_html as tree_html
from ..entry.grammar import ReviewParseError
from ..entry.parse import Entry, parse_file
from ..git.run import Git
from ..goals.skeleton import describe, groups_for
from ..render.entryview import render_entry
from ..render.highlight import css as highlight_css, highlight_code
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
        self._index: RepoIndex | None = None
        self._index_key: tuple | None = None
        self._terms: list | None = None
        self._terms_digest: str = ""
        self._head_sha: str = ""

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

    def index(self) -> RepoIndex:
        """The paths an entry can refer to, rebuilt when the tracked set actually changes.

        Keyed on git's own index file rather than on the watcher's digest: the watcher sees the review folder,
        and the set of tracked files changes when someone stages something in the repository under review —
        two different events, and using the wrong one leaves a newly tracked file unresolvable until a restart.
        """
        key = (stat_key(self.repo / ".git" / "index"), stat_key(self.paths.root))
        if self._index is None or self._index_key != key:
            self._index, self._index_key = RepoIndex.build(self.repo, self.paths.root), key
        return self._index

    def terms(self) -> list:
        """Every glossary term in the review, which is what makes one entry's vocabulary reach the others.

        Keyed on the watcher's digest, unlike `index()` above: the terms come from the entry files, which is
        exactly what the watcher is watching, while the tracked file set moves independently of this folder.
        Without a key, fetching one entry re-parses every entry — quadratic in a review's size.
        """
        digest = self.watcher.digest
        if self._terms is None or self._terms_digest != digest:
            self._terms = glossary_terms([e for _, e, err in self.entries() if err is None])
            self._terms_digest = digest
        return self._terms

    def head_sha(self) -> str:
        """HEAD, resolved once for the life of the server rather than once per entry fetch.

        A review is read against one tree, and `restart` is already the answer for anything that moves
        under a running server — which is what the staleness marker on an `example` block is measured against.
        """
        if not self._head_sha:
            self._head_sha = Git(self.repo).rev_parse("HEAD") or ""
        return self._head_sha

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
                head=self.head_sha(),
            )
            tokens = build_tokens(entry, self.index(), answers=answers,
                                  confirm_shas=Git(self.repo).which_are_commits,
                                  terms=self.terms())
            return 200, {"slug": slug, "html": html, "broken": False, "tokens": tokens_to_json(tokens)}
        return 404, {"error": f"no entry {slug!r}"}

    def file_view(self, path: str) -> tuple[int, dict]:
        """One whole file, highlighted — for the peek popover and for the page a click opens alike.

        Both want the same thing, and a computed window only ever guessed at how much of it.
        The rule that walked up through the comment block above a line was right often enough to keep and wrong
        often enough to notice.
        The reader scrolls instead, which never hides the line above the one they asked about.

        Only paths the index resolves are served, so a path outside the tree is a lookup miss rather than
        a case to defend against.
        """
        resolution = self.index().resolve(path)
        if not resolution.ok:
            return 404, {"error": f"{path!r} is not a tracked file in the repository under review"}
        target = self.index().absolute(resolution.path)
        if target is None:
            return 404, {"error": f"{resolution.path} left the index"}
        try:
            body = target.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            return 404, {"error": f"{resolution.path} could not be read: {e}"}

        count = len(body.splitlines())
        return 200, {
            "path": resolution.path, "absolute": target.resolve().as_posix(),
            "start": 1, "end": count, "lines": count,
            "html": highlight_code(body, path=resolution.path),
        }

    def tree_view(self, path: str) -> tuple[int, dict]:
        """What is under one folder, as the same tree the overview draws.

        The leaf label is size rather than diff delta: a folder reference asks what is in here, not what moved.
        """
        resolution = self.index().resolve_dir(path)
        if not resolution.ok:
            return 404, {"error": f"{path!r} is not a folder in the repository under review"}
        files = self.index().under(resolution.path)
        return 200, {
            "path": resolution.path, "files": len(files),
            "html": tree_html(files, _size_label(self.index()), max_rows=POPOVER_ROWS),
        }

    def commit_view(self, sha: str) -> tuple[int, dict]:
        """One commit's message and diffstat, for the popover its sha carries."""
        if not sha or not all(c in "0123456789abcdef" for c in sha.lower()):
            return 404, {"error": "not a commit id"}
        details = Git(self.repo).commit_details(sha)
        if not details:
            return 404, {"error": f"{sha} does not name a commit here"}
        cfg = self.config()
        details["forge"] = _forge_commit_url(cfg.upstream, details["sha"])
        return 200, details

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


def favicon_svg(name: str) -> str:
    """A tab icon derived from the review's name.

    Several reviews open in several tabs is the normal case, and a checked-in icon would make every one of them
    look the same — which is the complaint rather than the fix.
    Initials on a colour hashed from the whole name: distinguishable, stable per review, and nothing to ship.
    """
    parts = [p for p in re.split(r"[-_. ]+", name) if p]
    # A number in the name is the discriminator, so it wins: `pr-146` and `pr-147` both initial to `P1`.
    numbers = [p for p in parts if p.isdigit()]
    mark = numbers[-1][-3:] if numbers else "".join(p[0] for p in parts)[:2].upper() or "R"
    hue = int(hashlib.blake2b(name.encode("utf-8"), digest_size=2).hexdigest(), 16) % 360
    size = 64
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}">'
        f'<rect width="{size}" height="{size}" rx="12" fill="hsl({hue} 55% 42%)"/>'
        f'<text x="50%" y="54%" dominant-baseline="central" text-anchor="middle" '
        f'font-family="system-ui, sans-serif" font-size="{34 if len(mark) < 2 else 28 if len(mark) < 3 else 22}" '
        f'font-weight="700" fill="#fff">{escape(mark)}</text></svg>'
    )


# Read far enough to be sure, and no further: a NUL in the first block is what every tool uses to call a file
# binary, and a file with none in 8 kB does not hide one in the middle often enough to pay for reading it all.
_SNIFF_BYTES = 8192


def _size_label(index: RepoIndex):
    """A leaf's label in a folder tree: lines where the file reads as text, bytes where it does not."""
    def label(path: str) -> str:
        target = index.absolute(path)
        if target is None:
            return ""
        try:
            size = target.stat().st_size
            head = target.read_bytes()[:_SNIFF_BYTES]
        except OSError:
            return ""
        if b"\0" in head:
            return f'<span class="tree-size">{_human_bytes(size)}</span>'
        try:
            lines = len(target.read_text(encoding="utf-8").splitlines())
        except (OSError, UnicodeDecodeError):
            return f'<span class="tree-size">{_human_bytes(size)}</span>'
        return f'<span class="tree-size">{lines} lines</span>'
    return label


def _human_bytes(size: int) -> str:
    for unit, cut in (("MB", 1024 * 1024), ("kB", 1024)):
        if size >= cut:
            return f"{size / cut:.1f} {unit}"
    return f"{size} B"


def _forge_commit_url(upstream: str, sha: str) -> str:
    """A web URL for a commit, or empty where the remote is not one this can be derived from.

    A repository with no forge is a valid thing to review, so this degrades to no link rather than to a broken one.
    """
    if not upstream or not sha:
        return ""
    url = upstream.strip()
    if url.startswith("git@") and ":" in url:
        host, _, path = url[len("git@"):].partition(":")
        url = f"https://{host}/{path}"
    if not url.startswith(("http://", "https://")):
        return ""
    return url.removesuffix(".git").rstrip("/") + f"/commit/{sha}"


def _error_panel(error: ReviewParseError) -> str:
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

    def _attachment(self, name: str) -> None:
        """Serve one file out of the review's own `attachments/`, and nothing outside it."""
        root = self.app.paths.attachments_dir.resolve()
        target = (root / name).resolve()
        if not target.is_file() or root not in target.parents:
            self._json(404, {"error": "no such attachment"})
            return
        types = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".gif": "image/gif",
                 ".webp": "image/webp", ".svg": "image/svg+xml", ".txt": "text/plain; charset=utf-8"}
        self._send(200, target.read_bytes(), types.get(target.suffix.lower(), "application/octet-stream"))

    def do_GET(self) -> None:  # noqa: N802 — the base class names it.
        route = urlparse(self.path).path
        try:
            if route in ("/", "/index.html"):
                self._asset("page.html")
            elif route == "/favicon.svg":
                self._send(200, favicon_svg(self.app.config().name).encode("utf-8"),
                           "image/svg+xml; charset=utf-8", cache=True)
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
            elif route == "/api/file":
                query = parse_qs(urlparse(self.path).query)
                code, payload = self.app.file_view(query.get("path", [""])[0])
                self._json(code, payload)
            elif route == "/api/tree":
                query = parse_qs(urlparse(self.path).query)
                code, payload = self.app.tree_view(query.get("path", [""])[0])
                self._json(code, payload)
            elif route == "/api/commit":
                query = parse_qs(urlparse(self.path).query)
                code, payload = self.app.commit_view(query.get("sha", [""])[0])
                self._json(code, payload)
            elif route.startswith("/attachments/"):
                self._attachment(unquote(route[len("/attachments/"):]))
            elif route.startswith("/file/"):
                self._asset("file.html")
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
