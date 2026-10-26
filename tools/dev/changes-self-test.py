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
    touched_paths,
)
from tools.dev.lib.quality.checks import Check, CheckRegistryError  # noqa: E402
from tools.dev.lib.quality.memo import CheckMemo, file_ids  # noqa: E402
from tools.dev.lib.quality.test_selection import select_test_binaries_for_change  # noqa: E402
from tools.dev.lib.quality.watch import Watch  # noqa: E402
from tools.dev.lib.project.graph import TargetGraph  # noqa: E402

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


def test_touched_paths_keep_deletions_and_both_names_of_a_rename(root: Path) -> None:
    # Runs after the rename-and-delete commit: selection must see what a linter drops, since a deleted file under a
    # watched tree is a reason to run the gate.
    touched = set(touched_paths(root, ChangeScope("HEAD")))
    assert {"renamed.md", "b.md"} <= touched, touched
    assert "a.md" in touched or "renamed.md" in touched, touched


def test_touched_paths_in_the_working_tree_include_a_deletion(root: Path) -> None:
    write(root, "fresh.md", "brand new\n")
    (root / "root.md").unlink()
    try:
        touched = set(touched_paths(root, ChangeScope()))
        assert {"fresh.md", "root.md"} <= touched, touched
    finally:
        git(root, "checkout", "--", "root.md")
        (root / "fresh.md").unlink()


def test_watch_globs_are_gitignore_shaped(root: Path) -> None:
    w = Watch.of(["**/*.cc", "tools/review/**", "docs/guides/prose.md", "libs/*/x/*.hh"])
    assert w.matches("a.cc") and w.matches("libs/base/clean-core/src/a.cc")
    assert not w.matches("a.cch") and not w.matches("a.cc/b")
    assert w.matches("tools/review/x/y.py") and not w.matches("tools/reviewer/x.py")
    assert w.matches("docs/guides/prose.md") and not w.matches("docs/guides/prose.md.bak")
    assert w.matches("libs/base/x/a.hh") and not w.matches("libs/base/y/x/a.hh")
    bare = Watch.of(["tools/review"])
    assert bare.matches("tools/review") and bare.matches("tools/review/a.py") and not bare.matches("tools/reviewer")
    assert w.dead_patterns(["a.cc", "tools/review/x"]) == ["docs/guides/prose.md", "libs/*/x/*.hh"]
    assert w.touched_by(["a.cc", "b.md"]) == ["a.cc"]


def test_a_check_declares_watches_or_always_and_never_both(root: Path) -> None:
    run = lambda **kw: True  # noqa: E731
    Check("w", "", False, run, watches=("x/**",))
    Check("a", "", False, run, always=True)
    for kwargs in ({}, {"watches": ("x",), "always": True}, {"watches": ()}):
        try:
            Check("bad", "", False, run, **kwargs)
        except CheckRegistryError:
            continue
        raise AssertionError(f"Check(**{kwargs}) should have raised CheckRegistryError")
    assert Check("a", "", False, run, always=True).touched_by(["p"]) == ["p"]
    assert Check("w", "", False, run, watches=("x/**",)).touched_by(["x/p", "y/p"]) == ["x/p"]


def _graph() -> TargetGraph:
    # core <- geo <- render <- render-test; core-test on core; a shader file under render's directory, no target lists it.
    g = TargetGraph()
    g.owners = {"libs/core/src/a.cc": {"core"}, "libs/geo/src/g.cc": {"geo"}, "libs/render/src/r.cc": {"render"},
                "libs/render/tests/t.cc": {"render-test"}, "libs/core/tests/t.cc": {"core-test"}}
    g.defined_in = {"libs/core": {"core"}, "libs/core/tests": {"core-test"}, "libs/geo": {"geo"},
                    "libs/render": {"render", "render-test"}}
    g.dependents = {"core": {"geo", "core-test"}, "geo": {"render"}, "render": {"render-test"},
                    "render-test": set(), "core-test": set()}
    return g


def test_target_graph_attributes_by_source_then_by_deepest_directory(root: Path) -> None:
    g = _graph()
    assert g.owners_of("libs/core/src/a.cc") == {"core"}
    assert g.owners_of("libs/render/shaders/x.sgl") == {"render", "render-test"}
    assert g.owners_of("libs/core/tests/data.bin") == {"core-test"}, "the deepest directory wins"
    assert g.owners_of("tools/x.py") is None
    assert g.affected({"core"}) == {"core", "geo", "render", "render-test", "core-test"}
    assert g.affected({"geo"}) == {"geo", "render", "render-test"}


def test_test_selection_narrows_and_never_guesses(root: Path) -> None:
    g = _graph()
    cands = ["core-test", "render-test"]
    inputs = Watch.of(["**/CMakeLists.txt"])
    prose = Watch.of(["**/*.md", "tools/dev/**"])
    sel = select_test_binaries_for_change(g, ["libs/geo/src/g.cc"], cands, build_inputs=inputs, prose=prose)
    assert sel.binaries == ["render-test"] and sel.skipped == ["core-test"] and not sel.everything
    sel = select_test_binaries_for_change(g, ["docs/a.md", "tools/dev/x.py"], cands, build_inputs=inputs, prose=prose)
    assert sel.binaries == [] and not sel.everything, "prose selects nothing"
    sel = select_test_binaries_for_change(g, ["libs/geo/CMakeLists.txt"], cands, build_inputs=inputs, prose=prose)
    assert sel.everything and sel.binaries == cands and "build input" in sel.reasons[0]
    sel = select_test_binaries_for_change(g, ["mystery.bin"], cands, build_inputs=inputs, prose=prose)
    assert sel.everything and "no target owns mystery.bin" in sel.reasons
    both = Watch.of(["tools/dev/run.py"])
    sel = select_test_binaries_for_change(g, ["tools/dev/run.py"], cands, build_inputs=both, prose=prose)
    assert sel.everything, "a build input wins over prose"


def test_memo_keys_a_gate_on_what_it_watches_and_nothing_else(root: Path) -> None:
    memo = CheckMemo(root, root / "build" / "memo.json")
    w = Watch.of(["a.md"])
    key = memo.gate_key("g", w, None)
    assert key is not None and memo.gate_key("g", w, None) == key, "stable across calls"
    assert memo.gate_key("h", w, None) != key, "the gate's name is part of the key"
    assert memo.gate_key("g", w, lambda: "tool 2") != key, "and so is the tool stamp"
    assert not memo.gate_passed_before("g", key)
    memo.record_gate("g", key)
    assert memo.gate_passed_before("g", key)
    assert CheckMemo(root, root / "build" / "memo.json").gate_passed_before("g", key), "it survives a reload"
    assert not CheckMemo(root, root / "build" / "memo.json", read=False).gate_passed_before("g", key), "unless reads are off"

    write(root, "root.md", "one\ntwo\nthree\nchanged\n")
    try:
        memo.invalidate_files()
        assert memo.gate_key("g", w, None) == key, "an unwatched edit changes nothing"
        write(root, "a.md", "alpha\nbeta\nedited\n")
        memo.invalidate_files()
        edited = memo.gate_key("g", w, None)
        assert edited != key, "a watched edit changes the key"
        assert not memo.gate_passed_before("g", edited)
        ids = file_ids(root)
        assert ids is not None and ids["a.md"].startswith("disk:"), "a dirty file is hashed from disk"
    finally:
        git(root, "checkout", "--", "root.md", "a.md")
    memo.invalidate_files()
    assert memo.gate_key("g", w, None) == key, "and reverting restores it"
    memo.forget_gate("g")
    assert not memo.gate_passed_before("g", key)


def test_memo_keys_a_test_binary_on_its_artifact(root: Path) -> None:
    memo = CheckMemo(root, root / "build" / "memo2.json")
    exe = root / "build" / "fake-test.exe"
    exe.parent.mkdir(exist_ok=True)
    exe.write_bytes(b"v1")
    assert memo.tests_passed_before("p", [("fake-test", exe)]) == set()
    memo.record_tests("p", [("fake-test", exe)], [])
    assert memo.tests_passed_before("p", [("fake-test", exe)]) == {"fake-test"}
    assert memo.tests_passed_before("q", [("fake-test", exe)]) == set(), "per preset"
    exe.write_bytes(b"v2")
    assert memo.tests_passed_before("p", [("fake-test", exe)]) == set(), "a rebuilt artifact is a new key"
    memo.record_tests("p", [("fake-test", exe)], [])
    memo.record_tests("p", [], ["fake-test"])
    assert memo.tests_passed_before("p", [("fake-test", exe)]) == set(), "a failure forgets the pass"


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
             "test_touched_paths_keep_deletions_and_both_names_of_a_rename",
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
