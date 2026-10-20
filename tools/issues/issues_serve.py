"""`issues serve`: a local web UI over the issue files — browse, read, see what blocks what, and triage.

issues.py loads this on demand and passes itself in as `tracker`, so this file never imports it and runs the same standalone and under dev.py.
**An edit writes the issue file and nothing else.**
The page shows the outstanding edits, and its "Commit & push" is the only thing that commits: one commit of the chosen issue files, pushed alone.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import re
import secrets
import threading
import time
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

WEB = Path(__file__).with_name("web")
STATIC = {"/app.js": "text/javascript", "/app.css": "text/css"}
ISSUE_GLOB = ":(glob)**/docs/issues/**/*.md"
NOTES = "## Notes"
EDITABLE = ("priority", "tags", "parent", "blocked-by", "claimed-by")
_TAG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
# The markers scan reads every file of a repository, so it is not repeated on every panel that opens.
_MARKER_TTL = 30.0


class UserError(Exception):
    pass


def _notes_span(lines: list[str], start: int) -> tuple[int, int] | None:
    """The `## Notes` section's heading index and the index after it, from `start`; a heading in a code fence does not count."""
    fenced, head = False, None
    for i in range(start, len(lines)):
        line = lines[i]
        if line.lstrip().startswith("```"):
            fenced = not fenced
        elif not fenced and head is None and line.strip() == NOTES:
            head = i
        elif not fenced and head is not None and re.match(r"#{1,2} ", line):
            return head, i
    return (head, len(lines)) if head is not None else None


def split_notes(tracker, text: str) -> tuple[str, str]:
    """The body after the frontmatter and its title, without the notes section, and the notes."""
    lines = text.split("\n")
    end = tracker._frontmatter_end(lines)
    body = lines[end + 1:] if end is not None else lines
    while body and not body[0].strip():
        body = body[1:]
    if body and body[0].startswith("# "):
        body = body[1:]
    span = _notes_span(body, 0)
    if span is None:
        return "\n".join(body).strip("\n"), ""
    notes = "\n".join(body[span[0] + 1:span[1]]).strip("\n")
    return "\n".join(body[:span[0]] + body[span[1]:]).strip("\n"), notes


def set_notes(tracker, text: str, notes: str) -> str:
    """`text` with its `## Notes` section holding `notes`: replaced in place, appended when new, removed when empty."""
    notes = notes.replace("\r\n", "\n").strip("\n")
    lines = text.split("\n")
    end = tracker._frontmatter_end(lines)
    span = _notes_span(lines, end + 1 if end is not None else 0)
    section = [NOTES, "", *notes.split("\n")] if notes.strip() else []
    if span is None:
        if not section:
            return text
        return "\n".join([*text.rstrip("\n").split("\n"), "", *section, ""])
    before, after = lines[:span[0]], lines[span[1]:]
    while before and not before[-1].strip():
        before.pop()
    if after and any(line.strip() for line in after):
        return "\n".join([*before, "", *section, "", *after] if section else [*before, "", *after])
    return "\n".join([*before, "", *section, ""] if section else [*before, ""])


class State:
    """What the server holds across requests: none of the issues, which are read fresh from the files every time."""

    def __init__(self, tracker, cwd: Path, *, with_submodules: bool, top_only: bool):
        self.t = tracker
        self.cwd = cwd
        self.flags = {"with_submodules": with_submodules, "top_only": top_only}
        self.token = secrets.token_urlsafe(24)
        self.lock = threading.Lock()
        # Files this server wrote, so the commit dialog preselects them over edits made elsewhere.
        self.touched: set[str] = set()
        self._markers: tuple[float, dict] | None = None

    def workspace(self):
        return self.t.Workspace(self.cwd, **self.flags)

    def _find(self, ws, qid: str):
        issue = ws.lookup(qid, ws.top)
        if issue is None:
            raise UserError(f"no issue {qid}; it may have moved or been renamed since the page loaded")
        return issue

    # --- reading -----------------------------------------------------------------------------------------------------

    def version(self, ws) -> str:
        h = hashlib.sha1()
        for repo in ws.default:
            for rel in self.t._ls_files(repo.root, ISSUE_GLOB):
                st = (repo.root / rel).stat()
                h.update(f"{repo.name}:{rel}:{st.st_mtime_ns}:{st.st_size}\n".encode())
        return h.hexdigest()[:16]

    def _issue_json(self, ws, issue) -> dict:
        def resolved(qid: str) -> str | None:
            target = ws.lookup(qid, issue.repo)
            return target.qid if target is not None else None

        body, notes = split_notes(self.t, issue.text)
        parent = issue.get("parent")
        blocked_by = issue.get_list("blocked-by")
        return {
            "qid": issue.qid, "id": issue.id, "type": issue.type, "repo": issue.repo.name, "scope": issue.scope or "/",
            "path": issue.path.as_posix(), "rel": issue.rel, "closed": issue.closed, "title": issue.title or issue.id,
            "priority": issue.get("priority") or "normal", "tags": issue.get_list("tags"),
            "parent": parent, "parent_qid": resolved(parent) if parent else None,
            "blocked_by": blocked_by, "blocked_by_qids": [q for q in map(resolved, blocked_by) if q],
            "blocked": not issue.closed and self.t._is_blocked(ws, issue),
            "claimed_by": issue.get("claimed-by"), "claimed_at": issue.get("claimed-at"), "created": issue.get("created"),
            "closed_date": issue.get("closed"), "resolution": issue.get("resolution"),
            "refs": [[k, v] for k, v in issue.refs()],
            "problems": [m for _, m in self.t._check_issue(ws, issue)],
            "body": body, "notes": notes,
        }

    def snapshot(self) -> dict:
        ws = self.workspace()
        issues = [self._issue_json(ws, i) for r in ws.default for i in r.issues()]
        user = self.t._git(ws.top.root, "config", "user.name", check=False).stdout.strip()
        return {"repo": ws.top.name, "root": ws.top.root.as_posix(), "user": user, "issues": issues,
                "version": self.version(ws), "pending": len(self.pending(ws))}

    def status(self) -> dict:
        ws = self.workspace()
        return {"version": self.version(ws), "pending": len(self.pending(ws))}

    def pending(self, ws) -> list[dict]:
        """Every issue file that differs from HEAD, staged or not, new ones included; a rename is its two sides."""
        out = []
        for repo in ws.default:
            raw = self.t._git(repo.root, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", ISSUE_GLOB).stdout
            entries = raw.split("\0")
            i = 0
            while i < len(entries):
                entry = entries[i]
                i += 1
                if len(entry) < 4:
                    continue
                code, rel = entry[:2], entry[3:]
                rels = [rel]
                if "R" in code or "C" in code:
                    rels.append(entries[i])
                    i += 1
                for r in rels:
                    path = (repo.root / r).resolve().as_posix()
                    out.append({"repo": repo.name, "rel": r, "path": path, "status": code.strip() or "?",
                                "exists": (repo.root / r).is_file(), "touched": path in self.touched})
        return out

    def diff(self, path: str) -> str:
        ws = self.workspace()
        repo, rel = self._issue_path(ws, path)
        if self.t._in_head(repo, "HEAD", rel) is not None:
            return self.t._git(repo.root, "diff", "HEAD", "--", rel).stdout
        file = repo.root / rel
        text = file.read_text(encoding="utf-8", errors="replace") if file.is_file() else ""
        return f"new file {rel}\n" + "".join(f"+{line}\n" for line in text.split("\n"))

    def markers(self) -> dict:
        now = time.monotonic()
        if self._markers is not None and now - self._markers[0] < _MARKER_TTL:
            return self._markers[1]
        ws = self.workspace()
        found: dict[str, list[dict]] = {}
        for repo in ws.default:
            for rel, line, qualifier, issue_id in self.t._markers(repo):
                target = ws.lookup(f"{qualifier}:{issue_id}" if qualifier else issue_id, repo)
                if target is not None:
                    found.setdefault(target.qid, []).append(
                        {"rel": rel, "line": line, "path": (repo.root / rel).as_posix()})
        self._markers = (now, found)
        return found

    # --- writing -----------------------------------------------------------------------------------------------------

    def _issue_path(self, ws, path: str):
        """The repository and repo-relative path of an issue file; anything else is refused, since this server only writes issues."""
        p = Path(path).resolve()
        repo = ws.repo_containing(p)
        rel = p.relative_to(repo.root).as_posix()
        if "docs/issues/" not in f"/{rel}" or not rel.endswith(".md") or repo not in ws.default:
            raise UserError(f"{path} is not an issue file of this workspace")
        return repo, rel

    def _write(self, ws, issue, text: str) -> None:
        """Write `text` over the issue, and take it back when it breaks a rule `issues check` holds that was not broken before."""
        if text == issue.text:
            return
        before = {m for _, m in self.t._check_issue(ws, issue)}
        original = issue.path.read_bytes()
        issue.path.write_text(text, encoding="utf-8", newline="\n")
        fresh = self.workspace()
        after = self._find(fresh, issue.qid)
        broken = [m for _, m in self.t._check_issue(fresh, after) if m not in before]
        if broken:
            issue.path.write_bytes(original)
            raise UserError(f"{issue.qid}: {'; '.join(broken)}")
        self.touched.add(issue.path.resolve().as_posix())

    def _updates(self, ws, issue, fields: dict) -> dict:
        updates: dict = {}
        for key, value in fields.items():
            if key not in EDITABLE:
                raise UserError(f"{key} is not editable here; edit the file")
            if key == "priority":
                if value not in self.t.PRIORITIES:
                    raise UserError(f"priority is one of {', '.join(self.t.PRIORITIES)}")
                # `normal` is the default, so it is spelled by leaving the key out.
                updates[key] = None if value == "normal" else value
            elif key == "tags":
                tags = list(dict.fromkeys(str(v).strip() for v in value if str(v).strip()))
                bad = [t for t in tags if not _TAG_RE.match(t)]
                if bad:
                    raise UserError(f"not a tag: {', '.join(bad)}")
                updates[key] = tags
            elif key == "blocked-by":
                ids = list(dict.fromkeys(str(v).strip() for v in value if str(v).strip()))
                if issue.qid in ids or issue.id in ids:
                    raise UserError(f"{issue.qid} cannot block itself")
                updates[key] = ids
            elif key == "parent":
                updates[key] = value or None
            elif key == "claimed-by":
                updates[key] = value or None
                updates["claimed-at"] = self.t._now_stamp() if value else None
        return updates

    def edit(self, qid: str, fields: dict, notes: str | None) -> None:
        with self.lock:
            ws = self.workspace()
            issue = self._find(ws, qid)
            text = self.t.set_fields(issue.text, self._updates(ws, issue, fields)) if fields else issue.text
            if notes is not None:
                text = set_notes(self.t, text, notes)
            self._write(ws, issue, text)

    def bulk(self, qids: list[str], add_tags: list[str], remove_tags: list[str], priority: str | None) -> None:
        with self.lock:
            for qid in qids:
                ws = self.workspace()
                issue = self._find(ws, qid)
                tags = [t for t in issue.get_list("tags") if t not in remove_tags] + add_tags
                fields: dict = {"tags": tags}
                if priority:
                    fields["priority"] = priority
                self._write(ws, issue, self.t.set_fields(issue.text, self._updates(ws, issue, fields)))

    def rename_tag(self, old: str, new: str) -> int:
        """Rename a tag on every issue carrying it, closed ones too, so the vocabulary stays one; an empty `new` drops it."""
        if new and not _TAG_RE.match(new):
            raise UserError(f"not a tag: {new}")
        count = 0
        with self.lock:
            for qid in [i.qid for r in self.workspace().default for i in r.issues() if old in i.get_list("tags")]:
                ws = self.workspace()
                issue = self._find(ws, qid)
                tags = [new if t == old else t for t in issue.get_list("tags") if t != old or new]
                self._write(ws, issue, self.t.set_fields(issue.text, self._updates(ws, issue, {"tags": tags})))
                count += 1
        return count

    def publish(self, paths: list[str], message: str) -> str:
        """Commit and push the chosen issue files, one commit per repository, each pushed alone."""
        message = message.strip() or "issues: triage"
        log = io.StringIO()
        with self.lock, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            ws = self.workspace()
            by_repo: dict = {}
            for path in paths:
                repo, rel = self._issue_path(ws, path)
                by_repo.setdefault(repo.name, (repo, []))[1].append(rel)
            if not by_repo:
                raise UserError("no file chosen")
            for repo, rels in by_repo.values():
                chosen = [i for i in repo.issues() if i.rel in rels]
                problems = [f"{i.qid}: {m}" for i in chosen for _, m in self.t._check_issue(ws, i)]
                # An issue it names that HEAD does not have yet must go along, or the remote gets a dangling id.
                for issue in chosen:
                    for qid in [issue.get("parent") or "", *issue.get_list("blocked-by")]:
                        target = ws.lookup(qid, repo) if qid else None
                        if (target is not None and target.repo is repo and target.rel not in rels
                                and self.t._in_head(repo, "HEAD", target.rel) is None):
                            problems.append(f"{issue.qid} names {target.qid}, which is not committed: choose it too")
                if problems:
                    raise UserError("issues check fails, so nothing was pushed:\n" + "\n".join(problems))
            for repo, rels in by_repo.values():
                self.t.publish_files(repo, rels, message)
                self.touched -= {(repo.root / r).resolve().as_posix() for r in rels}
        return log.getvalue()


class Handler(BaseHTTPRequestHandler):
    state: State

    def log_message(self, *args) -> None:
        pass

    def _send(self, code: int, body: bytes, kind: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, value) -> None:
        self._send(code, json.dumps(value).encode("utf-8"), "application/json")

    def _host_ok(self) -> bool:
        # A page elsewhere could reach this port through a rebound DNS name; only the loopback names are answered.
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
        return host in ("127.0.0.1", "localhost")

    def _guarded(self, fn) -> None:
        try:
            fn()
        except (UserError, self.state.t.IssueError) as e:
            self._json(400, {"error": str(e)})
        except Exception:
            self._json(500, {"error": traceback.format_exc()})

    def do_GET(self) -> None:
        if not self._host_ok():
            return self._send(403, b"forbidden", "text/plain")
        url = urlparse(self.path)
        st = self.state
        if url.path in ("/", "/index.html"):
            html = (WEB / "index.html").read_text(encoding="utf-8").replace("{{TOKEN}}", st.token)
            return self._send(200, html.encode("utf-8"), "text/html; charset=utf-8")
        if url.path in STATIC:
            return self._send(200, (WEB / url.path[1:]).read_bytes(), f"{STATIC[url.path]}; charset=utf-8")
        query = parse_qs(url.query)
        routes = {
            "/api/issues": lambda: st.snapshot(),
            "/api/status": lambda: st.status(),
            "/api/pending": lambda: st.pending(st.workspace()),
            "/api/diff": lambda: {"diff": st.diff(query.get("path", [""])[0])},
            "/api/markers": lambda: st.markers(),
        }
        if url.path not in routes:
            return self._send(404, b"not found", "text/plain")
        self._guarded(lambda: self._json(200, routes[url.path]()))

    def do_POST(self) -> None:
        if not self._host_ok() or self.headers.get("X-Issues-Token") != self.state.token:
            return self._send(403, b"forbidden", "text/plain")
        st = self.state
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        except json.JSONDecodeError:
            return self._json(400, {"error": "the body is not JSON"})
        routes = {
            "/api/edit": lambda: st.edit(body["qid"], body.get("fields") or {}, body.get("notes")),
            "/api/bulk": lambda: st.bulk(body["qids"], body.get("add_tags") or [], body.get("remove_tags") or [],
                                         body.get("priority")),
            "/api/rename-tag": lambda: {"renamed": st.rename_tag(body["from"], body.get("to") or "")},
            "/api/publish": lambda: {"log": st.publish(body.get("paths") or [], body.get("message") or "")},
        }
        path = urlparse(self.path).path
        if path not in routes:
            return self._send(404, b"not found", "text/plain")
        self._guarded(lambda: self._json(200, routes[path]() or {}))


def make_server(tracker, cwd: Path, *, port: int = 0, with_submodules: bool = False, top_only: bool = False):
    """The server, bound to loopback but not yet serving, and its state."""
    state = State(tracker, cwd, with_submodules=with_submodules, top_only=top_only)
    state.workspace()
    handler = type("BoundHandler", (Handler,), {"state": state})
    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    except OSError:
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.daemon_threads = True
    return server, state


def serve(tracker, ws, args) -> int:
    server, _ = make_server(tracker, ws.cwd, port=args.port, with_submodules=args.with_submodules, top_only=args.top_only)
    url = f"http://127.0.0.1:{server.server_port}/"
    print(f"issues: serving {url}  (Ctrl+C stops)")
    if not args.no_open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
