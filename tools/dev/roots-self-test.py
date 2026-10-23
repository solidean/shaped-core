#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for a Layout's extra roots — tools/dev/lib/quality/roots.py and the crossref and lint discovery over them.

An extra root fails quietly in both directions.
A submodule nobody lists is a tree the gates never read, and a glob that matches too much is a gate that reads what it should not.
Each case builds a throwaway superproject whose submodule `extern/api` has a local bare repository as its remote.

    uv run tools/dev/roots-self-test.py [-v]
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import traceback
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.core.layout import ExtraRoot, Layout  # noqa: E402
from tools.dev.lib.quality.changes import ChangeScope  # noqa: E402
from tools.dev.lib.quality.crossrefs import check_crossrefs  # noqa: E402
from tools.dev.lib.quality.format import discover_lint_files, lint_changed_line_ranges, prose_only_lint_files  # noqa: E402
from tools.dev.lib.quality.roots import glob_matches  # noqa: E402

VERBOSE = "-v" in sys.argv

BASE_LAYOUT = Layout(
    source_roots=("libs",),
    lint_roots=("libs", "docs"),
    lint_root_files=("README.md",),
    crossref_scan_dirs=("libs", "docs"),
    crossref_root_segments=("docs/", "libs/"),
    library_dir="libs",
)

API = ExtraRoot("extern/api", submodule=True, checks=("crossrefs", "prose"), exclude=("src/**",))
LAYOUT = replace(BASE_LAYOUT, extra_roots=(API,))


def git(cwd: Path, *argv: str) -> str:
    # autocrlf off for the clones too, which a submodule add makes before its config can be set; a converted checkout reads as dirty.
    proc = subprocess.run(["git", "-c", "protocol.file.allow=always", "-c", "core.autocrlf=false", "-C", str(cwd), *argv],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, f"git {' '.join(argv)}: {proc.stderr}"
    return proc.stdout.strip()


def identity(repo: Path) -> None:
    git(repo, "config", "user.name", "T")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "core.autocrlf", "false")
    git(repo, "config", "commit.gpgsign", "false")


def write(repo: Path, name: str, text: str) -> None:
    p = repo / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


class World:
    """`parent` has `docs/` of its own and the submodule at `extern/api`, whose remote is `api.git`.

    The submodule's readme links its own guide root-relative, and its `src/` holds a broken link the layout excludes.
    """

    def __init__(self, base: Path):
        git(base, "init", "-q", "--bare", "-b", "main", "api.git")
        git(base, "clone", "-q", str(base / "api.git"), "seed")
        seed = base / "seed"
        identity(seed)
        git(seed, "checkout", "-q", "-b", "main")
        write(seed, "README.md", "# api\n\nSee [the guide](docs/guide.md#usage).\n")
        write(seed, "docs/guide.md", "# guide\n\n## usage\n\nBack to [the readme](../README.md).\n")
        write(seed, "src/gen.md", "[generated](nowhere.md)\n")
        write(seed, "src/gen.cc", "// nothing\n")
        git(seed, "add", "-A")
        git(seed, "commit", "-q", "-m", "seed")
        git(seed, "push", "-q", "origin", "main")

        git(base, "init", "-q", "-b", "main", "parent")
        self.parent = base / "parent"
        identity(self.parent)
        write(self.parent, "docs/index.md", "# index\n\nThe [api readme](../extern/api/README.md).\n")
        git(self.parent, "submodule", "add", "-q", "-b", "main", str(base / "api.git"), "extern/api")
        git(self.parent, "add", "-A")
        git(self.parent, "commit", "-q", "-m", "add api")
        self.api = self.parent / "extern/api"
        identity(self.api)

    def rel(self, paths) -> set[str]:
        return {Path(p).resolve().relative_to(self.parent.resolve()).as_posix() for p in paths}


# ---------------------------------------------------------------------------

def test_globs(base: Path) -> None:
    assert glob_matches("src/**", "src/a/b.md")
    assert glob_matches("src", "src/a/b.md"), "a directory glob takes its subtree"
    assert glob_matches("src/", "x/src/b.md"), "a glob without an inner slash matches at any depth"
    assert not glob_matches("/src", "x/src/b.md"), "a leading slash anchors"
    assert glob_matches("*.md", "a/b/c.md")
    assert not glob_matches("*.md", "a/b/c.mdx")
    assert glob_matches("docs/**/*.md", "docs/c.md")
    assert not glob_matches("docs/*.md", "docs/a/c.md"), "a single star does not cross a slash"
    assert glob_matches("apigen/target/**", "apigen/target/debug/x.md")
    assert not glob_matches("apigen/target/**", "apigen/src/x.md")


def test_an_unknown_check_is_refused(base: Path) -> None:
    try:
        ExtraRoot("extern/api", checks=("prose", "links"))
    except ValueError:
        return
    raise AssertionError("an unknown check name must not be accepted silently")


def test_crossrefs_scan_the_submodule_against_its_own_root(base: Path) -> None:
    w = World(base)
    write(w.api, "docs/broken.md", "[gone](docs/missing.md)\n")
    # Resolves only against the submodule's root: relative to the file it would land in a docs/docs/ that does not exist.
    write(w.api, "docs/rooted.md", "[guide](docs/guide.md#usage)\n")

    result = check_crossrefs(w.parent, layout=LAYOUT)
    offenders = [o for o in result.offenders]
    assert len(offenders) == 1, offenders
    assert offenders[0].startswith("extern/api/docs/broken.md:1: broken link"), offenders


def test_crossrefs_without_extra_roots_are_unchanged(base: Path) -> None:
    w = World(base)
    write(w.api, "docs/broken.md", "[gone](docs/missing.md)\n")
    result = check_crossrefs(w.parent, layout=BASE_LAYOUT)
    assert result.ok, result.offenders
    assert result.md_files == 1, "only the project's own docs/index.md is a source"


def test_full_lint_takes_the_selected_kinds_and_prunes_junk(base: Path) -> None:
    w = World(base)
    write(w.api, "build/out.md", "generated\n")
    write(w.api, "tool.py", "# a script\n")

    files = w.rel(discover_lint_files(w.parent, scope=None, layout=LAYOUT))
    assert "extern/api/README.md" in files, files
    assert "extern/api/docs/guide.md" in files, files
    assert "extern/api/src/gen.md" not in files, "excluded by the layout"
    assert "extern/api/build/out.md" not in files, "a junk directory below the root is pruned"
    assert "extern/api/tool.py" in files, "`prose` takes the comments of code too"
    assert prose_only_lint_files(w.parent, discover_lint_files(w.parent, scope=None, layout=LAYOUT), layout=LAYOUT), \
        "and only the prose rules run over them"

    code = replace(BASE_LAYOUT, extra_roots=(replace(API, checks=("code",)),))
    files = w.rel(discover_lint_files(w.parent, scope=None, layout=code))
    assert "extern/api/tool.py" in files, files
    assert "extern/api/README.md" not in files, files
    assert not prose_only_lint_files(w.parent, discover_lint_files(w.parent, scope=None, layout=code), layout=code), \
        "`code` runs every rule"

    assert not any(f.startswith("extern/") for f in w.rel(discover_lint_files(w.parent, scope=None, layout=BASE_LAYOUT)))


def test_scoped_lint_sees_the_submodules_working_tree(base: Path) -> None:
    w = World(base)
    write(w.api, "docs/guide.md", "# guide\n\n## usage\n\nBack to [the readme](../README.md).\nA new line.\n")
    write(w.api, "docs/fresh.md", "new\n")
    write(w.api, "src/gen.md", "edited, but excluded\n")

    scope = ChangeScope()
    files = w.rel(discover_lint_files(w.parent, scope=scope, layout=LAYOUT))
    assert files == {"extern/api/docs/guide.md", "extern/api/docs/fresh.md"}, files

    ranges = {Path(p).resolve().relative_to(w.parent.resolve()).as_posix(): r
              for p, r in lint_changed_line_ranges(w.parent, scope, layout=LAYOUT).items()}
    assert ranges["extern/api/docs/guide.md"] == [(6, 6)], ranges
    assert ranges["extern/api/docs/fresh.md"] == [(1, 0xFFFFFFFF)], ranges


def test_since_scope_is_the_submodules_unpushed_work(base: Path) -> None:
    w = World(base)
    git(w.api, "checkout", "-q", "main")
    write(w.api, "docs/pushed.md", "pushed\n")
    git(w.api, "add", "-A")
    git(w.api, "commit", "-q", "-m", "pushed")
    git(w.api, "push", "-q", "origin", "main")
    write(w.api, "docs/local.md", "local\n")
    git(w.api, "add", "-A")
    git(w.api, "commit", "-q", "-m", "local")

    head = git(w.parent, "rev-parse", "HEAD")
    files = w.rel(discover_lint_files(w.parent, scope=ChangeScope(since=head), layout=LAYOUT))
    assert files == {"extern/api/docs/local.md"}, files


def test_since_scope_on_a_detached_head_measures_from_the_recorded_branch(base: Path) -> None:
    """`git submodule update` leaves a detached HEAD, which has no `@{upstream}`."""
    w = World(base)
    git(w.api, "checkout", "-q", "--detach")
    write(w.api, "docs/local.md", "local\n")
    git(w.api, "add", "-A")
    git(w.api, "commit", "-q", "-m", "local")

    head = git(w.parent, "rev-parse", "HEAD")
    files = w.rel(discover_lint_files(w.parent, scope=ChangeScope(since=head), layout=LAYOUT))
    assert files == {"extern/api/docs/local.md"}, files


def test_revision_scope_follows_the_gitlink(base: Path) -> None:
    w = World(base)
    git(w.api, "checkout", "-q", "main")
    write(w.api, "docs/bumped.md", "bumped\n")
    git(w.api, "add", "-A")
    git(w.api, "commit", "-q", "-m", "bumped")
    git(w.parent, "add", "extern/api")
    git(w.parent, "commit", "-q", "-m", "bump api")

    files = w.rel(discover_lint_files(w.parent, scope=ChangeScope(revision="HEAD"), layout=LAYOUT))
    assert files == {"extern/api/docs/bumped.md"}, files

    files = w.rel(discover_lint_files(w.parent, scope=ChangeScope(revision="HEAD~1"), layout=LAYOUT))
    assert "extern/api/README.md" in files, "adding the submodule is a range from nothing"
    assert "extern/api/src/gen.md" not in files, files


# ---------------------------------------------------------------------------

def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        with tempfile.TemporaryDirectory(prefix="roots-self-test-") as tmp:
            try:
                t(Path(tmp).resolve())
            except Exception as e:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first
                failed += 1
                print(f"FAIL {t.__name__}: {type(e).__name__}: {e}")
                if VERBOSE:
                    traceback.print_exc()
            else:
                if VERBOSE:
                    print(f"ok   {t.__name__}")

    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
