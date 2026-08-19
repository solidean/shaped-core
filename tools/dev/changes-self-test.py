#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for tools/dev/lib/quality/changes.py — the change-set discovery behind `--dirty-only` and `--commit`.

These are the parts that fail quietly.
A revision that resolves to nothing scopes every gate to zero files and reports green without having looked at a single line, which is the failure mode a scoped re-check exists to prevent.
A merge commit diffed against the wrong parent yields only the conflict resolutions, which looks like a successful run over a mostly-empty change set.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/changes-self-test.py [-v]
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.quality.changes import (  # noqa: E402
    ChangeScope,
    ChangeScopeError,
    changed_files,
    changed_line_ranges,
    format_changed_line_spec,
    resolve_range,
)

VERBOSE = "-v" in sys.argv


def git(root: Path, *args: str) -> str:
    out = subprocess.run(["git", *args], cwd=str(root), capture_output=True, text=True, encoding="utf-8")
    if out.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout.strip()


def write(root: Path, name: str, text: str) -> None:
    p = root / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


def names(root: Path, paths: list[Path]) -> set[str]:
    """Selected files as repo-relative POSIX names, which is what the assertions read as."""
    return {Path(os.path.relpath(p, root)).as_posix() for p in paths}


def make_repo(root: Path) -> None:
    """A repo with a root commit, a two-commit feature branch, and a merge of it back into main.

    The merge is the shape the `--commit` flag exists for, so every fixture below is built on this one history.
    """
    git(root, "init", "--initial-branch=main")
    git(root, "config", "user.email", "self-test@example.com")
    git(root, "config", "user.name", "self test")
    git(root, "config", "commit.gpgsign", "false")

    write(root, "root.md", "one\ntwo\nthree\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "root")

    git(root, "checkout", "-b", "feature")
    write(root, "a.md", "alpha\nbeta\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "add a")

    write(root, "b.md", "gamma\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "add b")

    git(root, "checkout", "main")
    write(root, "on-main.md", "main-side\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "diverge on main")

    git(root, "merge", "--no-ff", "feature", "-m", "merge feature")


# ---------------------------------------------------------------------------

def test_single_commit_is_its_first_parent_diff(root: Path) -> None:
    head = git(root, "rev-parse", "HEAD~1")  # "diverge on main"
    assert names(root, changed_files(root, ChangeScope(head))) == {"on-main.md"}


def test_merge_commit_yields_everything_it_brought_in(root: Path) -> None:
    """The motivating case: a merge's first-parent diff is the whole merged-in change set, not just the resolutions."""
    merge = git(root, "rev-parse", "HEAD")
    assert len(git(root, "rev-parse", f"{merge}^@").split()) == 2, "fixture must be a real merge"
    assert names(root, changed_files(root, ChangeScope(merge))) == {"a.md", "b.md"}


def test_range(root: Path) -> None:
    base = git(root, "rev-parse", "main~1")
    assert names(root, changed_files(root, ChangeScope(f"{base}..HEAD"))) == {"a.md", "b.md"}


def test_three_dot_range_is_accepted(root: Path) -> None:
    assert names(root, changed_files(root, ChangeScope("HEAD~1...HEAD"))) == {"a.md", "b.md"}


def test_root_commit_diffs_against_the_empty_tree(root: Path) -> None:
    first = git(root, "rev-list", "--max-parents=0", "HEAD")
    assert names(root, changed_files(root, ChangeScope(first))) == {"root.md"}


def test_bad_revision_raises_rather_than_selecting_nothing(root: Path) -> None:
    """The silent-green regression this module exists to prevent: a typo must not scope a gate to zero files."""
    for bad in ("nosuchrev", "nosuchrev..HEAD", "HEAD..nosuchrev"):
        try:
            changed_files(root, ChangeScope(bad))
        except ChangeScopeError:
            continue
        raise AssertionError(f"{bad!r} should have raised ChangeScopeError")


def test_deletions_are_dropped_and_renames_yield_the_new_path(root: Path) -> None:
    git(root, "mv", "a.md", "renamed.md")
    git(root, "rm", "-q", "b.md")
    git(root, "commit", "-m", "rename and delete")
    selected = names(root, changed_files(root, ChangeScope("HEAD")))
    assert "renamed.md" in selected, selected
    assert "b.md" not in selected, selected
    assert "a.md" not in selected, selected


def test_line_ranges_are_numbered_against_the_head_commit(root: Path) -> None:
    write(root, "lines.md", "\n".join(f"line {i}" for i in range(1, 11)) + "\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "ten lines")

    body = [f"line {i}" for i in range(1, 11)]
    body[4] = "line five, edited"
    write(root, "lines.md", "\n".join(body) + "\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "edit line five")

    ranges = changed_line_ranges(root, ChangeScope("HEAD"))
    assert list(ranges.values()) == [[(5, 5)]], ranges


def test_pure_deletion_marks_the_surviving_line_above(root: Path) -> None:
    write(root, "del.md", "keep 1\ndrop 2\nkeep 3\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "three lines")

    write(root, "del.md", "keep 1\nkeep 3\n")
    git(root, "add", "-A")
    git(root, "commit", "-m", "drop the middle")

    ranges = changed_line_ranges(root, ChangeScope("HEAD"))
    assert list(ranges.values()) == [[(1, 1)]], ranges


def test_a_commit_has_no_untracked_files(root: Path) -> None:
    """An untracked file is a working-tree notion, so it must not leak into a revision's change set."""
    write(root, "stray.md", "not committed\n")
    try:
        assert "stray.md" not in names(root, changed_files(root, ChangeScope("HEAD")))
        assert "stray.md" not in {p.name for p in changed_line_ranges(root, ChangeScope("HEAD"))}
    finally:
        (root / "stray.md").unlink()


def test_working_tree_scope_still_covers_dirty_and_untracked(root: Path) -> None:
    write(root, "root.md", "one\ntwo\nthree\nfour\n")
    write(root, "fresh.md", "brand new\n")
    try:
        selected = names(root, changed_files(root, ChangeScope()))
        assert {"root.md", "fresh.md"} <= selected, selected

        ranges = changed_line_ranges(root, ChangeScope())
        by_name = {p.name: v for p, v in ranges.items()}
        assert by_name["root.md"] == [(4, 4)], by_name
        assert by_name["fresh.md"] == [(1, 0xFFFFFFFF)], by_name
    finally:
        git(root, "checkout", "--", "root.md")
        (root / "fresh.md").unlink()


def test_resolve_range_expands_a_single_commit_to_its_first_parent(root: Path) -> None:
    head = git(root, "rev-parse", "HEAD")
    parent = git(root, "rev-parse", "HEAD^")
    assert resolve_range(root, head) == (parent, head)
    assert resolve_range(root, "HEAD~1..HEAD") == ("HEAD~1", "HEAD")


def test_spec_rendering(root: Path) -> None:
    spec = format_changed_line_spec({Path("/x/a.md"): [(1, 2), (7, 7)]})
    assert spec.strip().endswith("a.md:1-2,7-7"), spec
    assert format_changed_line_spec({}) == ""


# ---------------------------------------------------------------------------

def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    # The history-mutating tests run last, so the shared fixture stays the documented shape for the rest.
    order = ["test_deletions_are_dropped_and_renames_yield_the_new_path",
             "test_line_ranges_are_numbered_against_the_head_commit",
             "test_pure_deletion_marks_the_surviving_line_above"]
    tests.sort(key=lambda t: order.index(t.__name__) + 1 if t.__name__ in order else 0)

    failed = 0
    with tempfile.TemporaryDirectory(prefix="changes-self-test-") as tmp:
        root = Path(tmp).resolve()
        make_repo(root)
        for t in tests:
            try:
                t(root)
            except Exception as e:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first
                failed += 1
                print(f"FAIL {t.__name__}: {type(e).__name__}: {e}")
            else:
                if VERBOSE:
                    print(f"ok   {t.__name__}")

    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
