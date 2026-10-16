#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for tools/dev/lib/project/submodules.py — `dev.py submodules sync`.

The point of the command is what it leaves alone, so most cases here are refusals.
Each builds a throwaway superproject whose submodule's remote is a local bare repository.

    uv run tools/dev/submodules-self-test.py [-v]
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.project import submodules as lib  # noqa: E402

VERBOSE = "-v" in sys.argv


def git(cwd: Path, *argv: str) -> str:
    proc = subprocess.run(["git", "-c", "protocol.file.allow=always", "-C", str(cwd), *argv],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, f"git {' '.join(argv)}: {proc.stderr}"
    return proc.stdout.strip()


def identity(repo: Path) -> None:
    git(repo, "config", "user.name", "T")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "core.autocrlf", "false")


def commit(repo: Path, name: str) -> str:
    (repo / name).write_text(name + "\n", encoding="utf-8")
    git(repo, "add", name)
    git(repo, "commit", "-q", "-m", name)
    return git(repo, "rev-parse", "HEAD")


class World:
    """`upstream`, a working clone of the submodule's remote, and `parent`, the superproject with it at `extern/lib`."""

    def __init__(self, base: Path, branch: str | None = "main"):
        git(base, "init", "-q", "--bare", "-b", "main", "lib.git")
        git(base, "clone", "-q", str(base / "lib.git"), "upstream")
        self.upstream = base / "upstream"
        identity(self.upstream)
        git(self.upstream, "checkout", "-q", "-b", "main")
        self.c1 = commit(self.upstream, "c1")
        git(self.upstream, "push", "-q", "origin", "main")

        git(base, "init", "-q", "-b", "main", "parent")
        self.parent = base / "parent"
        identity(self.parent)
        git(self.parent, "submodule", "add", "-q", *(["-b", branch] if branch else []), str(base / "lib.git"), "extern/lib")
        git(self.parent, "commit", "-q", "-m", "add lib")
        self.lib = self.parent / "extern/lib"
        identity(self.lib)
        self.sub = lib.list_submodules(self.parent)[0]

        # The remote moves on by two commits.
        commit(self.upstream, "c2")
        self.c3 = commit(self.upstream, "c3")
        git(self.upstream, "push", "-q", "origin", "main")

    def sync(self, **kw) -> lib.Outcome:
        outcome = lib.sync(self.parent, self.sub, **kw)
        if VERBOSE:
            print(f"    {outcome}")
        return outcome

    def head(self) -> str:
        return git(self.lib, "rev-parse", "HEAD")


# ---------------------------------------------------------------------------

def test_clean_branch_fast_forwards(base: Path):
    w = World(base)
    assert w.sub.branch == "main"
    o = w.sync(dry_run=True)
    assert o.status == "would update" and w.head() == w.c1, o
    o = w.sync()
    assert o.status == "updated" and "2 commit(s)" in o.detail and w.head() == w.c3, o
    assert w.sync().status == "up-to-date"


def test_uncommitted_changes_are_refused(base: Path):
    w = World(base)
    (w.lib / "c1").write_text("edited\n", encoding="utf-8")
    o = w.sync()
    assert o.status == "refused" and "uncommitted" in o.detail and w.head() == w.c1, o
    git(w.lib, "checkout", "--", "c1")
    (w.lib / "scratch.txt").write_text("untracked\n", encoding="utf-8")
    assert w.sync().status == "updated", "an untracked file is no reason to refuse"


def test_unpushed_commits_are_refused(base: Path):
    w = World(base)
    mine = commit(w.lib, "mine")
    o = w.sync()
    assert o.status == "refused" and "1 unpushed" in o.detail and w.head() == mine, o


def test_another_branch_is_skipped(base: Path):
    w = World(base)
    git(w.lib, "checkout", "-q", "-b", "topic")
    o = w.sync()
    assert o.status == "skipped" and "topic" in o.detail and w.head() == w.c1, o


def test_detached_within_the_branch_moves_onto_it(base: Path):
    w = World(base)
    git(w.lib, "checkout", "-q", "--detach", w.c1)
    o = w.sync()
    assert o.status == "updated" and w.head() == w.c3 and "then 2 commit(s)" in o.detail, o
    assert git(w.lib, "symbolic-ref", "--short", "HEAD") == "main"
    git(w.lib, "checkout", "-q", "--detach")
    o = w.sync()
    assert o.status == "updated" and "then" not in o.detail, "detached at the tip: only the checkout"
    assert git(w.lib, "symbolic-ref", "--short", "HEAD") == "main"


def test_detached_without_a_local_branch_creates_it(base: Path):
    w = World(base)
    git(w.lib, "checkout", "-q", "--detach", w.c1)
    git(w.lib, "branch", "-q", "-D", "main")
    assert w.sync().status == "updated" and w.head() == w.c3
    assert git(w.lib, "rev-parse", "--abbrev-ref", "main@{upstream}") == "origin/main"


def test_detached_elsewhere_is_refused(base: Path):
    w = World(base)
    git(w.lib, "checkout", "-q", "--detach", w.c1)
    stray = commit(w.lib, "stray")
    o = w.sync()
    assert o.status == "refused" and "does not contain" in o.detail and w.head() == stray, o


def test_detached_with_unpushed_local_branch_is_refused(base: Path):
    w = World(base)
    mine = commit(w.lib, "mine")
    git(w.lib, "checkout", "-q", "--detach", w.c1)
    o = w.sync()
    assert o.status == "refused" and "local main has 1 unpushed" in o.detail, o
    assert git(w.lib, "rev-parse", "main") == mine and w.head() == w.c1


def test_no_branch_is_skipped(base: Path):
    w = World(base, branch=None)
    o = w.sync()
    assert o.status == "skipped" and "branch =" in o.detail and w.head() == w.c1, o


def test_bump_commits_only_the_pointer(base: Path):
    w = World(base)
    (w.parent / "staged.txt").write_text("not part of the bump\n", encoding="utf-8")
    git(w.parent, "add", "staged.txt")
    assert lib.bump(w.parent, w.sub) is None, "nothing moved yet"
    w.sync()
    subject = lib.bump(w.parent, w.sub)
    assert subject == "Bump lib: 2 commit(s) from main", subject
    assert git(w.parent, "show", "--name-only", "--format=", "HEAD") == "extern/lib"
    assert lib.recorded(w.parent, w.sub) == w.c3
    assert "A  staged.txt" in git(w.parent, "status", "--porcelain")


# ---------------------------------------------------------------------------

def main() -> int:
    tests = [(name, fn) for name, fn in globals().items() if name.startswith("test_")]
    failed = 0
    for name, fn in tests:
        with tempfile.TemporaryDirectory(prefix="submodules-test-") as tmp:
            try:
                fn(Path(tmp))
                print(f"ok    {name}")
            except Exception:
                failed += 1
                print(f"FAIL  {name}")
                traceback.print_exc()
    print(f"{len(tests) - failed} of {len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
