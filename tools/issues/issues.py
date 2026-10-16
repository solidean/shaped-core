#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6"]
# ///

"""In-repo issue tracking: one markdown file per issue, in any `docs/issues/` folder of a git repository.

The files are the tracker and this script is comfort on top: everything it does can be done by hand.
docs/guides/issue-tracking.md is the format and the workflow, and `--help` is the CLI.

It runs standalone, `uv run tools/issues/issues.py <command>` from anywhere inside a repository, or as `dev.py issues`.
It is one self-contained file on purpose, so a repository without shaped-core can run it from a URL.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote

TYPES = ("bug", "feat", "perf", "debt", "chore", "question", "epic")
PRIORITIES = ("next", "normal", "someday")
RESOLUTIONS = ("fixed", "wontfix", "duplicate", "obsolete")
REF_KINDS = ("file", "test", "symbol", "issue")
CONFIG_FILE = ".issues.toml"
CONFIG_KEYS = {"name", "branch", "remote", "exclude", "submodules"}

_SLUG = r"[a-z0-9]+(?:-[a-z0-9]+)*"
_ID = rf"(?:{'|'.join(TYPES)})-{_SLUG}"
ID_RE = re.compile(rf"^{_ID}$")
# An id, optionally qualified by the repository it lives in: `shaped-core:<id>`.
QID_RE = re.compile(rf"^(?:([A-Za-z0-9_.-]+):)?({_ID})$")
# How code points at an issue: an `issue: <id>` comment, or `TEMPORARY(<id>)` on a workaround.
MARKER_RE = re.compile(rf"(?:\bissue:[ \t]*|\bTEMPORARY\()(?:([A-Za-z0-9_.-]+):)?({_ID})\b")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_STAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(?::\d{2})?Z$")
_LINES_RE = re.compile(r"^(.*?):(\d+)(?:-(\d+))?$")
_LINK_RE = re.compile(r"(\]\()([^)\s#]+)(#[^)\s]*)?(\))")

# Files larger than this are not scanned for markers.
_MAX_SCAN_BYTES = 1 << 20
# A claim retries this often when someone else pushes in between.
_PUSH_ATTEMPTS = 5


class IssueError(Exception):
    pass


# ---------------------------------------------------------------------------
# git
# ---------------------------------------------------------------------------

def _git(root: Path, *argv: str, check: bool = True, env: dict | None = None,
         stdin: bytes | None = None) -> subprocess.CompletedProcess:
    # Bytes in, so a blob is hashed exactly as given; text out, decoded here rather than by a platform-dependent pipe.
    proc = subprocess.run(["git", "-C", str(root), *argv], capture_output=True, env=env, input=stdin)
    proc.stdout = proc.stdout.decode("utf-8", errors="replace")
    proc.stderr = proc.stderr.decode("utf-8", errors="replace")
    if check and proc.returncode != 0:
        raise IssueError(f"git {' '.join(argv)} failed in {root}:\n{proc.stderr.strip()}")
    return proc


def _git_out(root: Path, *argv: str, **kw) -> str:
    return _git(root, *argv, **kw).stdout.strip()


def _toplevel(path: Path) -> Path:
    proc = _git(path, "rev-parse", "--show-toplevel", check=False)
    if proc.returncode != 0:
        raise IssueError(f"{path} is not inside a git repository")
    return Path(proc.stdout.strip()).resolve()


def _ls_files(root: Path, *pathspec: str) -> list[str]:
    """Tracked and untracked-but-not-ignored files, repo-relative; a submodule's content is not listed."""
    out = _git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *pathspec).stdout
    return sorted({p for p in out.split("\0") if p and (root / p).is_file()})


def _is_tracked(root: Path, rel: str) -> bool:
    return _git(root, "ls-files", "--error-unmatch", "--", rel, check=False).returncode == 0


# ---------------------------------------------------------------------------
# Frontmatter: read with YAML, written by editing single lines so a hand-written file keeps its shape
# ---------------------------------------------------------------------------

def _frontmatter_end(lines: list[str]) -> int | None:
    """The index of the closing `---`, or None when the file has no frontmatter."""
    if not lines or lines[0].strip() != "---":
        return None
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            return i
    return None


def _scalar(value: str) -> str:
    """A YAML scalar for `value`: plain where YAML reads it back unchanged, else double-quoted."""
    plain = re.fullmatch(r"[\w.()/@+-][\w .()/@+:-]*", value)
    if plain and ": " not in value and not value.endswith((":", " ")):
        return value
    return json.dumps(value, ensure_ascii=False)


def set_fields(text: str, updates: dict[str, str | None]) -> str:
    """Set or (with None) remove top-level frontmatter keys, touching no other line."""
    lines = text.split("\n")
    end = _frontmatter_end(lines)
    fm, rest = (lines[1:end], lines[end + 1:]) if end is not None else ([], lines)
    for key, value in updates.items():
        new = [] if value is None else [f"{key}: {_scalar(value)}"]
        start = next((i for i, line in enumerate(fm) if re.match(rf"{re.escape(key)}\s*:", line)), None)
        if start is None:
            fm += new
            continue
        stop = start + 1
        # A block value continues on indented lines and on `- ` items, which YAML allows at column 0.
        while stop < len(fm) and fm[stop].startswith((" ", "\t", "-")):
            stop += 1
        fm[start:stop] = new
    if not any(line.strip() for line in fm):
        while rest and not rest[0].strip():
            rest = rest[1:]
        return "\n".join(rest)
    return "\n".join(["---", *fm, "---", *rest])


def _parse_meta(text: str) -> tuple[dict, dict[str, int], list[str], int, str | None]:
    """The frontmatter as plain strings, lists and maps, each key's line, the body's lines, its first line number, and a parse error."""
    import yaml  # Only reading an issue needs it, so dev.py's other commands never import it.

    lines = text.split("\n")
    end = _frontmatter_end(lines)
    if end is None:
        if lines and lines[0].strip() == "---":
            return {}, {}, lines, 1, "the frontmatter's closing `---` is missing"
        return {}, {}, lines, 1, None
    key_lines = {m.group(1): i + 2 for i, line in enumerate(lines[1:end]) if (m := re.match(r"([\w-]+)\s*:", line))}
    try:
        # BaseLoader keeps every scalar a string: a date stays `2026-10-16`, and `no` stays `no`.
        meta = yaml.load("\n".join(lines[1:end]), Loader=yaml.BaseLoader) or {}
    except yaml.YAMLError as e:
        return {}, key_lines, lines[end + 1:], end + 2, f"the frontmatter is not valid YAML: {e}"
    if not isinstance(meta, dict):
        return {}, key_lines, lines[end + 1:], end + 2, "the frontmatter must be a mapping of keys"
    return meta, key_lines, lines[end + 1:], end + 2, None


# ---------------------------------------------------------------------------
# Repositories and their issues
# ---------------------------------------------------------------------------

@dataclass
class Config:
    name: str | None = None
    branch: str | None = None
    remote: str = "origin"
    exclude: list[str] = field(default_factory=list)
    submodules: list[str] = field(default_factory=list)


def _load_config(root: Path) -> Config:
    path = root / CONFIG_FILE
    if not path.is_file():
        return Config()
    import tomllib  # Here rather than at the top, so dev.py can import this module on the Python 3.10 it still allows.

    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise IssueError(f"{path}: {e}")
    unknown = set(data) - CONFIG_KEYS
    if unknown:
        raise IssueError(f"{path}: unknown key(s) {', '.join(sorted(unknown))}; known are {', '.join(sorted(CONFIG_KEYS))}")
    return Config(**data)


@dataclass
class Repo:
    root: Path
    name: str
    # The prefix an id from this repository carries when shown from the top one; empty for the top one.
    qualifier: str
    config: Config
    gitmodules_branch: str | None = None
    _issues: list[Issue] | None = None

    @property
    def branch(self) -> str:
        """The integration branch a claim pushes to: the config's, the superproject's .gitmodules', origin's HEAD, or main."""
        if self.config.branch:
            return self.config.branch
        if self.gitmodules_branch:
            return self.gitmodules_branch
        head = _git(self.root, "symbolic-ref", "--short", f"refs/remotes/{self.config.remote}/HEAD", check=False)
        if head.returncode == 0 and "/" in head.stdout:
            return head.stdout.strip().split("/", 1)[1]
        return "main"

    def excluded(self, rel: str) -> bool:
        return any(rel == e.rstrip("/") or rel.startswith(e.rstrip("/") + "/") for e in self.config.exclude)

    def issues(self) -> list[Issue]:
        if self._issues is None:
            files = _ls_files(self.root, ":(glob)**/docs/issues/**/*.md")
            self._issues = [_load_issue(self, self.root / rel) for rel in files if not self.excluded(rel)]
        return self._issues


@dataclass
class Issue:
    repo: Repo
    path: Path
    id: str
    scope: str
    closed: bool
    text: str
    meta: dict
    key_lines: dict[str, int]
    title: str | None
    title_line: int
    errors: list[tuple[int, str]]

    @property
    def type(self) -> str:
        return self.id.split("-", 1)[0]

    @property
    def qid(self) -> str:
        return f"{self.repo.qualifier}:{self.id}" if self.repo.qualifier else self.id

    @property
    def rel(self) -> str:
        return self.path.relative_to(self.repo.root).as_posix()

    def get(self, key: str) -> str | None:
        value = self.meta.get(key)
        return value if isinstance(value, str) and value else None

    def get_list(self, key: str) -> list[str]:
        value = self.meta.get(key)
        if isinstance(value, str):
            return [value] if value else []
        return [v for v in value if isinstance(v, str)] if isinstance(value, list) else []

    def refs(self) -> list[tuple[str, str]]:
        value = self.meta.get("refs")
        if not isinstance(value, list):
            return []
        return [next(iter(r.items())) for r in value
                if isinstance(r, dict) and len(r) == 1 and isinstance(next(iter(r.values())), str)]


def _load_issue(repo: Repo, path: Path) -> Issue:
    rel = path.relative_to(repo.root).as_posix()
    parts = rel.split("/")
    at = max(i for i in range(len(parts) - 1) if parts[i:i + 2] == ["docs", "issues"])
    scope, rest = "/".join(parts[:at]), parts[at + 2:]
    errors: list[tuple[int, str]] = []
    closed = False
    if len(rest) == 4 and rest[0] == "closed":
        closed = True
        if not (re.fullmatch(r"\d{4}", rest[1]) and re.fullmatch(r"0[1-9]|1[0-2]", rest[2])):
            errors.append((1, f"a closed issue lives in docs/issues/closed/<yyyy>/<mm>/, mm two digits; not {'/'.join(rest[:3])}/"))
    elif len(rest) != 1:
        errors.append((1, "an issue lives in docs/issues/ or docs/issues/closed/<yyyy>/<mm>/"))
    stem = path.stem
    if not ID_RE.match(stem):
        errors.append((1, f"the file name must be <type>-<slug>.md, type one of {', '.join(TYPES)}, slug lowercase kebab-case"))

    text = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    meta, key_lines, body, body_line, error = _parse_meta(text)
    if error:
        errors.append((1, error))
    title, title_line = None, body_line
    for i, line in enumerate(body):
        if line.strip():
            title_line = body_line + i
            if line.startswith("# "):
                title = line[2:].strip()
            break
    if title is None:
        errors.append((title_line, "the first line after the frontmatter must be the title, `# <title>`"))
    return Issue(repo, path, stem, scope, closed, text, meta, key_lines, title, title_line, errors)


class Workspace:
    """The repository the command runs in, plus its checked-out submodules, each a repository of issues of its own."""

    def __init__(self, cwd: Path, *, with_submodules: bool = False, top_only: bool = False):
        self.cwd = cwd.resolve()
        root = _toplevel(self.cwd)
        config = _load_config(root)
        self.top = Repo(root, config.name or root.name, "", config)
        self.subs: list[Repo] = []
        listed = _git(root, "config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$", check=False)
        for line in listed.stdout.splitlines() if (root / ".gitmodules").is_file() else []:
            key, rel = line.split(" ", 1)
            sub = root / rel
            if not (sub / ".git").exists():
                continue
            name = key[len("submodule."):-len(".path")]
            branch = _git(root, "config", "-f", ".gitmodules", f"submodule.{name}.branch", check=False).stdout.strip()
            sub_config = _load_config(sub)
            sub_name = sub_config.name or Path(rel).name
            self.subs.append(Repo(sub.resolve(), sub_name, sub_name, sub_config, branch or None))
        configured = {Path(p).as_posix().rstrip("/") for p in config.submodules}
        if top_only:
            self.default = [self.top]
        else:
            self.default = [self.top] + [r for r in self.subs
                                         if with_submodules or r.root.relative_to(root).as_posix() in configured]

    @property
    def all(self) -> list[Repo]:
        return [self.top, *self.subs]

    def repo_named(self, name: str) -> Repo | None:
        return next((r for r in self.all if r.name == name), None)

    def repo_containing(self, path: Path) -> Repo:
        path = path.resolve()
        owners = [r for r in self.all if path == r.root or r.root in path.parents]
        if not owners:
            raise IssueError(f"{path} is outside {self.top.root}")
        return max(owners, key=lambda r: len(r.root.parts))

    def lookup(self, qid: str, origin: Repo) -> Issue | None:
        """The issue `qid` names, read from `origin` when unqualified."""
        m = QID_RE.match(qid)
        if not m:
            return None
        repo = self.repo_named(m.group(1)) if m.group(1) else origin
        if repo is None:
            return None
        return next((i for i in repo.issues() if i.id == m.group(2)), None)

    def resolve(self, query: str) -> Issue:
        """What a user typed: a path, a (qualified) id, or a fragment unique among the ids."""
        path = (self.cwd / query)
        if query.endswith(".md") and path.is_file():
            repo = self.repo_containing(path)
            found = [i for i in repo.issues() if i.path == path.resolve()]
            if found:
                return found[0]
            raise IssueError(f"{query} is not an issue file")
        m = QID_RE.match(query)
        if m and m.group(1):
            repo = self.repo_named(m.group(1))
            if repo is None:
                raise IssueError(f"no checked-out repository named {m.group(1)}")
            hits = [i for i in repo.issues() if i.id == m.group(2)]
        else:
            hits = [i for r in self.default for i in r.issues() if i.id == query]
            hits = hits or [i for r in self.all for i in r.issues() if i.id == query]
            hits = hits or [i for r in self.default for i in r.issues() if query in i.id]
        if len(hits) == 1:
            return hits[0]
        if not hits:
            raise IssueError(f"no issue matches {query!r}")
        raise IssueError(f"{query!r} is ambiguous: {', '.join(i.qid for i in hits)}")

    def show_path(self, path: Path) -> str:
        """`path` relative to where the command runs, so it is clickable from there."""
        return Path(os.path.relpath(path, self.cwd)).as_posix()


# ---------------------------------------------------------------------------
# new / list / show
# ---------------------------------------------------------------------------

def _today() -> str:
    return dt.date.today().isoformat()


def _now_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def cmd_new(ws: Workspace, args: argparse.Namespace) -> int:
    issue_id = args.name if args.slug is None else f"{args.name}-{args.slug}"
    if not ID_RE.match(issue_id):
        raise IssueError(f"{issue_id!r} is no issue id: <type>-<slug>, type one of {', '.join(TYPES)}, slug lowercase kebab-case")
    owner = (ws.cwd / (args.in_ or ".")).resolve()
    if owner.name == "issues" and owner.parent.name == "docs":
        owner = owner.parent.parent
    repo = ws.repo_containing(owner)
    taken = [i for i in repo.issues() if i.id == issue_id]
    if taken:
        raise IssueError(f"{issue_id} exists already: {ws.show_path(taken[0].path)}")
    if args.priority and args.priority not in PRIORITIES:
        raise IssueError(f"priority is one of {', '.join(PRIORITIES)}")

    fm = [f"created: {_today()}"]
    if args.priority:
        fm.append(f"priority: {args.priority}")
    if args.tag:
        fm.append(f"tags: [{', '.join(_scalar(t) for t in args.tag)}]")
    if args.parent:
        fm.append(f"parent: {_scalar(args.parent)}")
    if args.ref:
        fm.append("refs:")
        for ref in args.ref:
            kind, sep, value = ref.partition(":")
            if not sep or kind not in REF_KINDS:
                raise IssueError(f"--ref is <kind>:<value>, kind one of {', '.join(REF_KINDS)}; not {ref!r}")
            fm.append(f"  - {kind}: {_scalar(value)}")
    title = args.title or issue_id.split("-", 1)[1].replace("-", " ").capitalize()
    body = args.body or "What is wrong or wanted, and what is already known."
    path = owner / "docs" / "issues" / f"{issue_id}.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(["---", *fm, "---", "", f"# {title}", "", body, ""]), encoding="utf-8", newline="\n")
    print(ws.show_path(path))
    return 0


def _claim_label(issue: Issue) -> str:
    by = issue.get("claimed-by")
    return f"@{by} {(issue.get('claimed-at') or '')[:10]}".rstrip() if by else ""


def _is_blocked(ws: Workspace, issue: Issue) -> bool:
    return any((b := ws.lookup(q, issue.repo)) is not None and not b.closed for q in issue.get_list("blocked-by"))


def _matches_list_filters(ws: Workspace, issue: Issue, args: argparse.Namespace, me: str | None) -> bool:
    if args.closed and not issue.closed or not (args.closed or args.all) and issue.closed:
        return False
    if args.type and issue.type not in args.type:
        return False
    if args.tag and not set(args.tag) <= set(issue.get_list("tags")):
        return False
    if args.priority and (issue.get("priority") or "normal") != args.priority:
        return False
    by = issue.get("claimed-by")
    if args.claimed and not by or args.unclaimed and by:
        return False
    if me is not None and not (by and me.lower() in by.lower()):
        return False
    if args.stale is not None:
        at = issue.get("claimed-at")
        if not (by and at and _STAMP_RE.match(at)):
            return False
        claimed = dt.datetime.strptime(at[:16], "%Y-%m-%dT%H:%M").replace(tzinfo=dt.timezone.utc)
        if dt.datetime.now(dt.timezone.utc) - claimed < dt.timedelta(days=args.stale):
            return False
    if args.in_:
        where = (ws.cwd / args.in_).resolve()
        owner = (issue.repo.root / issue.scope).resolve()
        if owner != where and where not in owner.parents:
            return False
    if args.grep and not re.search(args.grep, f"{issue.id}\n{issue.text}", re.I):
        return False
    return True


def cmd_list(ws: Workspace, args: argparse.Namespace) -> int:
    me = _git_out(ws.top.root, "config", "user.name") if args.mine else None
    repos = [ws.repo_named(args.repo)] if args.repo else ws.default
    if repos == [None]:
        raise IssueError(f"no checked-out repository named {args.repo}")
    rank = {"next": 0, "someday": 2}
    found = [i for r in repos for i in r.issues() if _matches_list_filters(ws, i, args, me)]
    found.sort(key=lambda i: (ws.all.index(i.repo), i.scope, rank.get(i.get("priority") or "", 1), TYPES.index(i.type)
                              if i.type in TYPES else 99, i.id))

    # An epic's children follow it, indented, when both are listed.
    listed = {id(i) for i in found}
    children: dict[int, list[Issue]] = {}
    roots = []
    for issue in found:
        parent = ws.lookup(issue.get("parent") or "", issue.repo)
        if parent is not None and id(parent) in listed and parent is not issue:
            children.setdefault(id(parent), []).append(issue)
        else:
            roots.append(issue)
    rows: list[tuple[str, Issue]] = []

    def visit(issue: Issue, depth: int) -> None:
        rows.append(("  " * depth + issue.qid, issue))
        for child in children.get(id(issue), []):
            visit(child, depth + 1)

    for issue in roots:
        visit(issue, 0)
    width_id = max((len(r[0]) for r in rows), default=0)
    width_scope = max((len(i.scope or "/") for _, i in rows), default=0)
    for label, issue in rows:
        flags = [f for f in (
            _claim_label(issue),
            "next" if issue.get("priority") == "next" else "",
            "blocked" if not issue.closed and _is_blocked(ws, issue) else "",
            f"closed {issue.get('closed') or ''} {issue.get('resolution') or ''}".strip() if issue.closed else "",
        ) if f]
        suffix = f"  [{'] ['.join(flags)}]" if flags else ""
        print(f"{label:<{width_id}}  {issue.scope or '/':<{width_scope}}  {issue.title or '(no title)'}{suffix}")
    print(f"{len(rows)} issue(s)", file=sys.stderr)
    return 0


def cmd_show(ws: Workspace, args: argparse.Namespace) -> int:
    issue = ws.resolve(args.id)
    print(f"{issue.qid}  {ws.show_path(issue.path)}")
    print()
    print(issue.text.rstrip("\n"))
    kids = [i for r in ws.all for i in r.issues() if ws.lookup(i.get("parent") or "", i.repo) is issue]
    if kids:
        done = sum(1 for k in kids if k.closed)
        print(f"\nchildren: {done} of {len(kids)} closed")
        for k in sorted(kids, key=lambda k: (k.closed, k.id)):
            print(f"  {'x' if k.closed else ' '} {k.qid}  {k.title or ''}")
    return 0


# ---------------------------------------------------------------------------
# known — which issues a file, a line, a test, a symbol or some text belongs to
# ---------------------------------------------------------------------------

def _markers(repo: Repo) -> list[tuple[str, int, str | None, str]]:
    """Every marker in the repository's files: (path, line, qualifier, id); issue files themselves are skipped."""
    found = []
    for rel in _ls_files(repo.root):
        if repo.excluded(rel) or "docs/issues/" in f"/{rel}" or rel == CONFIG_FILE:
            continue
        path = repo.root / rel
        try:
            if path.stat().st_size > _MAX_SCAN_BYTES:
                continue
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192] or (b"issue:" not in data and b"TEMPORARY(" not in data):
            continue
        for n, line in enumerate(data.decode("utf-8", errors="replace").splitlines(), 1):
            for m in MARKER_RE.finditer(line):
                found.append((rel, n, m.group(1), m.group(2)))
    return found


def _file_ref(value: str) -> tuple[str, int | None, int | None]:
    m = _LINES_RE.match(value)
    if m:
        first = int(m.group(2))
        return m.group(1), first, int(m.group(3)) if m.group(3) else first
    return value, None, None


def cmd_known(ws: Workspace, args: argparse.Namespace) -> int:
    query = args.query
    hits: list[tuple[Issue, str]] = []
    m = _LINES_RE.match(query)
    target = ws.cwd / (m.group(1) if m else query)
    repos = list(ws.default)

    if target.exists():
        repo = ws.repo_containing(target)
        repos += [] if repo in repos else [repo]
        rel = target.resolve().relative_to(repo.root).as_posix()
        first = int(m.group(2)) if m else None
        last = int(m.group(3)) if m and m.group(3) else first
        for issue in repo.issues():
            for kind, value in issue.refs():
                if kind != "file":
                    continue
                ref_path, ref_first, ref_last = _file_ref(value)
                ref_path = ref_path.rstrip("/")
                if rel != ref_path and not rel.startswith(ref_path + "/"):
                    continue
                if first is not None and ref_first is not None and (ref_last < first or last < ref_first):
                    continue
                hits.append((issue, f"refs file: {value}"))
        for marker_rel, line, qualifier, issue_id in _markers(repo):
            if marker_rel == rel and (first is None or first <= line <= last):
                issue = ws.lookup(f"{qualifier}:{issue_id}" if qualifier else issue_id, repo)
                if issue is not None:
                    hits.append((issue, f"marked at {marker_rel}:{line}"))
    else:
        lowered = query.lower()
        for issue in (i for r in repos for i in r.issues()):
            for kind, value in issue.refs():
                v = value.lower()
                if kind == "test" and (fnmatch.fnmatch(lowered, v) or lowered in v or v in lowered):
                    hits.append((issue, f"refs test: {value}"))
                elif kind == "symbol" and (v == lowered or v.endswith("::" + lowered) or lowered.endswith("::" + v)):
                    hits.append((issue, f"refs symbol: {value}"))
            if lowered in issue.id or lowered in issue.text.lower():
                hits.append((issue, "mentions it"))

    seen: dict[int, Issue] = {}
    reasons: dict[int, list[str]] = {}
    for issue, why in hits:
        if args.open and issue.closed:
            continue
        seen[id(issue)] = issue
        reasons.setdefault(id(issue), []).append(why)
    for key, issue in sorted(seen.items(), key=lambda kv: (kv[1].closed, kv[1].qid)):
        state = f"  [closed {issue.get('closed') or '?'} {issue.get('resolution') or '?'}]" if issue.closed else ""
        print(f"{issue.qid}  {issue.title or ''}{state}")
        print(f"    {ws.show_path(issue.path)}; {'; '.join(dict.fromkeys(reasons[key]))}")
    if not seen:
        print(f"no known issue matches {query!r}", file=sys.stderr)
    return 0 if seen else 1


# ---------------------------------------------------------------------------
# claim / release — one commit, built on the remote branch and pushed alone
# ---------------------------------------------------------------------------

def _remote_path(repo: Repo, base: str, issue_id: str) -> str | None:
    names = _git_out(repo.root, "ls-tree", "-r", "--name-only", base).splitlines()
    hits = [n for n in names if n.endswith(f"/{issue_id}.md") and "docs/issues/" in f"/{n}"]
    return hits[0] if hits else None


def _publish(ws: Workspace, issue: Issue, verb: str, mutate) -> None:
    """Commit `mutate(text)` of the issue on top of the remote integration branch, push only that commit, then merge it locally.

    A rejected push means someone pushed first: it refetches, so `mutate` judges the claim against the state that won.
    """
    repo, rel = issue.repo, issue.rel
    untracked = not _is_tracked(repo.root, rel)
    if not untracked and _git(repo.root, "diff", "--quiet", "HEAD", "--", rel, check=False).returncode != 0:
        raise IssueError(f"{ws.show_path(issue.path)} has uncommitted changes; commit or discard them before you {verb}")
    remote, branch = repo.config.remote, repo.branch
    tracking = f"refs/remotes/{remote}/{branch}"
    message = f"{verb} {issue.id}"

    for _ in range(_PUSH_ATTEMPTS):
        _git(repo.root, "fetch", "-q", remote, f"+refs/heads/{branch}:{tracking}")
        base = _git_out(repo.root, "rev-parse", "--verify", f"{tracking}^{{commit}}")
        path = _remote_path(repo, base, issue.id)
        if path is None:
            if not untracked:
                raise IssueError(f"{issue.id} is committed here but not on {remote}/{branch}: push it first")
            path, text = rel, issue.text
        else:
            if untracked:
                raise IssueError(f"{remote}/{branch} already has an issue {issue.id}, at {path}")
            if "/closed/" in f"/{path}":
                raise IssueError(f"{issue.id} is closed on {remote}/{branch}")
            text = _git(repo.root, "show", f"{base}:{path}").stdout.replace("\r\n", "\n")
        new_text = mutate(text)
        blob = _git_out(repo.root, "hash-object", "-w", "--stdin", stdin=new_text.encode("utf-8"))
        mode = (_git_out(repo.root, "ls-tree", base, "--", path).split() or ["100644"])[0]
        with tempfile.TemporaryDirectory() as tmp:
            env = {**os.environ, "GIT_INDEX_FILE": str(Path(tmp) / "index")}
            _git(repo.root, "read-tree", base, env=env)
            _git(repo.root, "update-index", "--add", "--cacheinfo", f"{mode},{blob},{path}", env=env)
            tree = _git(repo.root, "write-tree", env=env).stdout.strip()
        commit = _git_out(repo.root, "commit-tree", tree, "-p", base, "-m", message)
        push = _git(repo.root, "push", "-q", remote, f"{commit}:refs/heads/{branch}", check=False)
        if push.returncode == 0:
            break
        if not re.search(r"rejected|fetch first|non-fast-forward|stale info", push.stderr):
            raise IssueError(f"pushing the {verb} failed:\n{push.stderr.strip()}")
    else:
        raise IssueError(f"gave up after {_PUSH_ATTEMPTS} pushes were rejected; {remote}/{branch} is moving fast")
    _git(repo.root, "update-ref", tracking, commit)
    print(f"{message}: pushed {commit[:10]} to {remote}/{branch}")

    # Bring the checkout up to date with the one commit; nothing else of the local state goes anywhere.
    if untracked:
        issue.path.unlink()
    if _git(repo.root, "merge-base", "--is-ancestor", "HEAD", commit, check=False).returncode == 0:
        _git(repo.root, "merge", "--ff-only", "-q", commit)
        return
    if _git(repo.root, "symbolic-ref", "-q", "HEAD", check=False).returncode == 0:
        merged = _git(repo.root, "merge", "--no-edit", "-q", "-m", f"Merge {message}", commit, check=False)
        if merged.returncode == 0:
            print(f"merged it into the local branch, which has commits {remote}/{branch} does not")
            return
        _git(repo.root, "merge", "--abort", check=False)
    if untracked or not _is_tracked(repo.root, rel):
        issue.path.write_text(new_text, encoding="utf-8", newline="\n")
    print(f"warning: the checkout was not updated; `git pull` brings the {verb} in", file=sys.stderr)


def _who(ws: Workspace, repo: Repo, explicit: str | None) -> str:
    name = explicit or _git(repo.root, "config", "user.name", check=False).stdout.strip()
    if not name:
        raise IssueError("no git user.name here; pass --as <name>")
    return name


def cmd_claim(ws: Workspace, args: argparse.Namespace) -> int:
    issue = ws.resolve(args.id)
    if issue.closed:
        raise IssueError(f"{issue.qid} is closed")
    who = _who(ws, issue.repo, args.as_name)

    def mutate(text: str) -> str:
        meta = _parse_meta(text)[0]
        by = meta.get("claimed-by")
        if by and not args.force:
            raise IssueError(f"{issue.qid} is claimed by {by} since {meta.get('claimed-at') or '?'}; --force takes it over")
        return set_fields(text, {"claimed-by": who, "claimed-at": _now_stamp()})

    _publish(ws, issue, "claim", mutate)
    return 0


def cmd_release(ws: Workspace, args: argparse.Namespace) -> int:
    issue = ws.resolve(args.id)
    who = _who(ws, issue.repo, args.as_name)

    def mutate(text: str) -> str:
        by = _parse_meta(text)[0].get("claimed-by")
        if not by:
            raise IssueError(f"{issue.qid} is not claimed")
        if by != who and not args.force:
            raise IssueError(f"{issue.qid} is claimed by {by}, not {who}; --force releases it anyway")
        return set_fields(text, {"claimed-by": None, "claimed-at": None})

    _publish(ws, issue, "release", mutate)
    return 0


# ---------------------------------------------------------------------------
# close / reopen — a move, committed with the change that resolves it
# ---------------------------------------------------------------------------

def _rewrite_links(text: str, rewrite) -> str:
    """Apply `rewrite(target) -> new target | None` to every relative markdown link."""
    def sub(m: re.Match) -> str:
        target = m.group(2)
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("/"):
            return m.group(0)
        new = rewrite(unquote(target))
        if new is None:
            return m.group(0)
        return f"{m.group(1)}{new.replace(' ', '%20') if '%20' in target else new}{m.group(3) or ''}{m.group(4)}"
    return _LINK_RE.sub(sub, text)


def _move(ws: Workspace, issue: Issue, new_path: Path, updates: dict[str, str | None]) -> None:
    """Move an issue file, its own relative links rebased, and point every markdown link in its repository at the new place."""
    repo, old_path = issue.repo, issue.path
    old_dir, new_dir = old_path.parent, new_path.parent

    def rebase(target: str) -> str | None:
        return Path(os.path.relpath((old_dir / target).resolve(), new_dir)).as_posix()

    text = _rewrite_links(set_fields(issue.text, updates), rebase)
    new_dir.mkdir(parents=True, exist_ok=True)
    if _is_tracked(repo.root, issue.rel):
        _git(repo.root, "mv", issue.rel, new_path.relative_to(repo.root).as_posix())
    else:
        old_path.replace(new_path)
    new_path.write_text(text, encoding="utf-8", newline="\n")

    for rel in _ls_files(repo.root, "*.md"):
        path = repo.root / rel
        if path == new_path or repo.excluded(rel):
            continue
        source = path.read_text(encoding="utf-8", errors="replace")

        def retarget(target: str, here: Path = path.parent) -> str | None:
            if (here / target).resolve() != old_path:
                return None
            return Path(os.path.relpath(new_path, here)).as_posix()

        updated = _rewrite_links(source, retarget)
        if updated != source:
            path.write_bytes(updated.encode("utf-8"))
            print(f"  relinked {ws.show_path(path)}")


def cmd_close(ws: Workspace, args: argparse.Namespace) -> int:
    issue = ws.resolve(args.id)
    if issue.closed:
        raise IssueError(f"{issue.qid} is closed already")
    date = args.date or _today()
    if not _DATE_RE.match(date):
        raise IssueError("--date is yyyy-mm-dd")
    year, month = date[:4], date[5:7]
    new_path = issue.repo.root / issue.scope / "docs" / "issues" / "closed" / year / month / f"{issue.id}.md"
    _move(ws, issue, new_path, {"closed": date, "resolution": args.resolution})
    print(f"closed {issue.qid} as {args.resolution}: {ws.show_path(new_path)}")
    print("commit it together with the change that resolves it", file=sys.stderr)
    return 0


def cmd_reopen(ws: Workspace, args: argparse.Namespace) -> int:
    issue = ws.resolve(args.id)
    if not issue.closed:
        raise IssueError(f"{issue.qid} is open")
    new_path = issue.repo.root / issue.scope / "docs" / "issues" / f"{issue.id}.md"
    _move(ws, issue, new_path, {"closed": None, "resolution": None})
    print(f"reopened {issue.qid}: {ws.show_path(new_path)}")
    return 0


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def _check_issue(ws: Workspace, issue: Issue) -> list[tuple[int, str]]:
    problems = list(issue.errors)
    meta, line = issue.meta, lambda key: issue.key_lines.get(key, 1)

    for key, pattern, shape in (("created", _DATE_RE, "yyyy-mm-dd"), ("closed", _DATE_RE, "yyyy-mm-dd"),
                                ("claimed-at", _STAMP_RE, "yyyy-mm-ddThh:mmZ, in UTC")):
        if key in meta and not (isinstance(meta[key], str) and pattern.match(meta[key])):
            problems.append((line(key), f"{key} is {shape}"))
    for key, allowed in (("priority", PRIORITIES), ("resolution", RESOLUTIONS)):
        if key in meta and meta[key] not in allowed:
            problems.append((line(key), f"{key} is one of {', '.join(allowed)}"))
    for key in ("claimed-by", "parent"):
        if key in meta and not isinstance(meta[key], str):
            problems.append((line(key), f"{key} is a single value"))
    for key in ("tags", "blocked-by"):
        value = meta.get(key)
        if value is not None and not (isinstance(value, str) or isinstance(value, list)
                                      and all(isinstance(v, str) for v in value)):
            problems.append((line(key), f"{key} is a list of values"))
    if ("claimed-by" in meta) != ("claimed-at" in meta):
        problems.append((line("claimed-by" if "claimed-by" in meta else "claimed-at"), "claimed-by and claimed-at go together"))

    if issue.closed:
        for key in ("closed", "resolution"):
            if key not in meta:
                problems.append((1, f"a closed issue records `{key}:`"))
        date = issue.get("closed") or ""
        folder = issue.path.parent.parent.name + "-" + issue.path.parent.name
        if _DATE_RE.match(date) and date[:7] != folder:
            problems.append((line("closed"), f"closed {date} belongs in closed/{date[:4]}/{date[5:7]}/"))
    else:
        for key in ("closed", "resolution"):
            if key in meta:
                problems.append((line(key), f"an open issue has no `{key}:`; closing moves it to docs/issues/closed/<yyyy>/<mm>/"))

    parent_id = issue.get("parent")
    if parent_id:
        parent = ws.lookup(parent_id, issue.repo)
        if parent is None:
            problems.append((line("parent"), f"parent {parent_id} does not exist"))
        elif parent.type != "epic":
            problems.append((line("parent"), f"parent {parent_id} is not an epic"))
        if issue.type == "epic":
            problems.append((line("parent"), "an epic has no parent"))
    for qid in issue.get_list("blocked-by"):
        if ws.lookup(qid, issue.repo) is None:
            problems.append((line("blocked-by"), f"blocked-by {qid} does not exist"))

    refs = meta.get("refs")
    if refs is not None and not isinstance(refs, list):
        problems.append((line("refs"), "refs is a list of `- <kind>: <value>` items"))
    for ref in refs if isinstance(refs, list) else []:
        if not (isinstance(ref, dict) and len(ref) == 1 and next(iter(ref)) in REF_KINDS
                and isinstance(next(iter(ref.values())), str)):
            problems.append((line("refs"), f"a ref is `- <kind>: <value>`, kind one of {', '.join(REF_KINDS)}; not {ref!r}"))
            continue
        kind, value = next(iter(ref.items()))
        if kind == "issue" and ws.lookup(value, issue.repo) is None:
            problems.append((line("refs"), f"refs issue {value}, which does not exist"))
        # A closed issue's files may have moved on since, which is history rather than a problem.
        if kind == "file" and not issue.closed:
            rel, first, last = _file_ref(value)
            target = issue.repo.root / rel
            if not target.exists():
                problems.append((line("refs"), f"refs file {rel}, which does not exist"))
            elif first is not None:
                count = len(target.read_text(encoding="utf-8", errors="replace").splitlines()) if target.is_file() else 0
                if not 1 <= first <= last <= count:
                    problems.append((line("refs"), f"refs {value}, but {rel} has {count} lines"))
    return problems


def cmd_check(ws: Workspace, args: argparse.Namespace) -> int:
    problems: list[tuple[str, int, str]] = []
    count = 0
    for repo in ws.default:
        issues = repo.issues()
        count += len(issues)
        by_id: dict[str, list[Issue]] = {}
        for issue in issues:
            by_id.setdefault(issue.id, []).append(issue)
            problems += [(ws.show_path(issue.path), n, msg) for n, msg in _check_issue(ws, issue)]
        for issue_id, same in by_id.items():
            if len(same) > 1:
                problems += [(ws.show_path(i.path), 1, f"id {issue_id} is used {len(same)} times in {repo.name}; ids are never reused")
                             for i in same]
        for rel, n, qualifier, issue_id in _markers(repo):
            qid = f"{qualifier}:{issue_id}" if qualifier else issue_id
            if qualifier and ws.repo_named(qualifier) is None:
                continue
            target = ws.lookup(qid, repo)
            if target is None:
                problems.append((ws.show_path(repo.root / rel), n, f"marker names {qid}, which does not exist"))
            elif target.closed:
                problems.append((ws.show_path(repo.root / rel), n,
                                 f"marker names {qid}, which is closed: remove the marker, or reopen the issue"))
    for path, n, msg in problems:
        print(f"{path}:{n}: {msg}")
    print(f"issues check: {count} issue(s) in {', '.join(r.name for r in ws.default)}, {len(problems)} problem(s)",
          file=sys.stderr)
    return 1 if problems else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def add_arguments(p: argparse.ArgumentParser) -> None:
    """The `issues` CLI on `p`; dev.py calls this on its own subparser, so no dest here may collide with dev.py's."""
    p.add_argument("--with-submodules", action="store_true",
                   help=f"also every checked-out submodule, not only those {CONFIG_FILE} lists")
    p.add_argument("--top-only", action="store_true", help="only the repository itself, none of its submodules")
    sub = p.add_subparsers(dest="issues_cmd", metavar="<command>")

    q = sub.add_parser("new", help="create an issue file (commits nothing)")
    q.add_argument("name", help=f"the type ({', '.join(TYPES)}), or the whole id <type>-<slug>")
    q.add_argument("slug", nargs="?", help="the slug, when the first argument is only the type")
    q.add_argument("--in", dest="in_", metavar="DIR", help="the directory owning the issue: DIR/docs/issues/ (default: here)")
    q.add_argument("-t", "--title")
    q.add_argument("--body", help="the description under the title")
    q.add_argument("--priority", choices=PRIORITIES)
    q.add_argument("--tag", action="append", help="repeatable")
    q.add_argument("--parent", help="the epic it belongs to")
    q.add_argument("--ref", action="append", metavar="KIND:VALUE",
                   help="repeatable: file:<path>[:<line>[-<line>]], test:<glob>, symbol:<name>, issue:<id>")

    q = sub.add_parser("list", help="list issues (default: open ones)")
    q.add_argument("--type", action="append", choices=TYPES, help="repeatable")
    q.add_argument("--tag", action="append", help="repeatable; all must be present")
    q.add_argument("--priority", choices=PRIORITIES)
    q.add_argument("--in", dest="in_", metavar="DIR", help="only issues owned by DIR or below it")
    q.add_argument("--repo", help="only this repository, by name")
    q.add_argument("--claimed", action="store_true")
    q.add_argument("--unclaimed", action="store_true")
    q.add_argument("--mine", action="store_true", help="claimed by a name containing your git user.name")
    q.add_argument("--stale", type=float, metavar="DAYS", help="claimed longer ago than DAYS")
    q.add_argument("--closed", action="store_true", help="only closed ones")
    q.add_argument("--all", action="store_true", help="open and closed")
    q.add_argument("-g", "--grep", metavar="REGEX", help="in the id, frontmatter and text")

    q = sub.add_parser("show", help="print an issue (an epic also lists its children)")
    q.add_argument("id", help="an id, repo:id, a unique part of an id, or the file")

    q = sub.add_parser("known", help="the issues a file, file:line, test, symbol or text belongs to (exit 1 if none)")
    q.add_argument("query")
    q.add_argument("--open", action="store_true", help="leave closed issues out")

    for verb, text in (("claim", "claim an issue: pushes one commit to the integration branch, nothing else"),
                       ("release", "release a claim, pushed the same way")):
        q = sub.add_parser(verb, help=text)
        q.add_argument("id")
        q.add_argument("--as", dest="as_name", metavar="NAME", help="who claims (default: git user.name)")
        q.add_argument("--force", action="store_true", help="take over or release someone else's claim")

    q = sub.add_parser("close", help="move an issue to docs/issues/closed/<yyyy>/<mm>/ (commits nothing)")
    q.add_argument("id")
    q.add_argument("--resolution", choices=RESOLUTIONS, default="fixed")
    q.add_argument("--date", help="yyyy-mm-dd (default: today)")

    q = sub.add_parser("reopen", help="move a closed issue back (commits nothing)")
    q.add_argument("id")

    sub.add_parser("check", help="validate every issue, and every marker in the code")


COMMANDS = {"new": cmd_new, "list": cmd_list, "show": cmd_show, "known": cmd_known, "claim": cmd_claim,
            "release": cmd_release, "close": cmd_close, "reopen": cmd_reopen, "check": cmd_check}


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="issues", description="In-repo issue tracking; docs/guides/issue-tracking.md is the format.")
    add_arguments(p)
    return p


def run(args: argparse.Namespace, cwd: Path | None = None) -> int:
    if args.issues_cmd is None:
        args = _parser().parse_args(["--with-submodules"] * args.with_submodules + ["--top-only"] * args.top_only + ["list"])
    try:
        ws = Workspace(cwd or Path.cwd(), with_submodules=args.with_submodules, top_only=args.top_only)
        return COMMANDS[args.issues_cmd](ws, args)
    except IssueError as e:
        print(f"issues: {e}", file=sys.stderr)
        return 2


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass
    return run(_parser().parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
