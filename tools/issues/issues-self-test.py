#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6"]
# ///

"""Self-test for the issue tracker: the file format, check, known, close, and claims against a real remote.

Every test builds throwaway git repositories with a bare repository as their remote, so a claim really pushes.
`dev.py check` runs it as its own gate; run it directly with `uv run tools/issues/issues-self-test.py [-v]`.
"""

from __future__ import annotations

import importlib.util
import io
import subprocess
import sys
import tempfile
import traceback
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

_spec = importlib.util.spec_from_file_location("issues", Path(__file__).with_name("issues.py"))
issues = importlib.util.module_from_spec(_spec)
# Registered before it runs: @dataclass resolves its own module through sys.modules, and Python 3.14 fails without it.
sys.modules["issues"] = issues
_spec.loader.exec_module(issues)

_spec = importlib.util.spec_from_file_location("issues_serve", Path(__file__).with_name("issues_serve.py"))
serve = importlib.util.module_from_spec(_spec)
sys.modules["issues_serve"] = serve
_spec.loader.exec_module(serve)

VERBOSE = "-v" in sys.argv


def git(cwd: Path, *argv: str) -> str:
    proc = subprocess.run(["git", "-c", "protocol.file.allow=always", "-C", str(cwd), *argv],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, f"git {' '.join(argv)}: {proc.stderr}"
    return proc.stdout.strip()


def cli(cwd: Path, *argv: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        try:
            code = issues.run(issues._parser().parse_args(list(argv)), cwd=cwd)
        except SystemExit as e:
            raise AssertionError(f"argparse refused {argv}: {err.getvalue()}") from e
    if VERBOSE:
        print(f"  $ issues {' '.join(argv)} -> {code}\n{out.getvalue()}{err.getvalue()}")
    return code, out.getvalue(), err.getvalue()


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))
    return path


def issue_text(title: str, *fm: str) -> str:
    return "\n".join(["---", "created: 2026-10-16", *fm, "---", "", f"# {title}", "", "Body.", ""])


def clone(base: Path, name: str, user: str) -> Path:
    git(base, "clone", "-q", "-c", "core.autocrlf=false", str(base / "remote.git"), name)
    repo = base / name
    git(repo, "config", "user.name", user)
    git(repo, "config", "user.email", f"{user.lower()}@example.com")
    return repo


def setup(base: Path) -> tuple[Path, Path]:
    """A remote with one pushed issue, cloned as Alice's `a` and Bob's `b`."""
    git(base, "init", "-q", "--bare", "-b", "main", "remote.git")
    git(base, "clone", "-q", str(base / "remote.git"), "seed")
    seed = base / "seed"
    git(seed, "config", "user.name", "Seed")
    git(seed, "config", "user.email", "seed@example.com")
    git(seed, "checkout", "-q", "-b", "main")
    write(seed / ".issues.toml", 'name = "demo"\nbranch = "main"\n')
    write(seed / "src" / "lib.cc", "one\ntwo\nthree\n")
    write(seed / "docs" / "issues" / "bug-crash.md", issue_text("It crashes", "refs:", "  - file: src/lib.cc:2-3"))
    write(seed / "docs" / "issues" / "feat-shiny.md", issue_text("Something shiny"))
    git(seed, "add", "-A")
    git(seed, "commit", "-q", "-m", "seed")
    git(seed, "push", "-q", "origin", "main")
    return clone(base, "a", "Alice"), clone(base, "b", "Bob")


# ---------------------------------------------------------------------------

def test_set_fields_touches_only_its_lines():
    text = "---\n# keep me\ncreated: 2026-10-16\nrefs:\n- file: a\n- file: b\nodd: [x, y]\n---\n\n# T\n"
    out = issues.set_fields(text, {"refs": "gone", "claimed-by": "Claude (for Alice)"})
    assert out == "---\n# keep me\ncreated: 2026-10-16\nrefs: gone\nodd: [x, y]\nclaimed-by: Claude (for Alice)\n---\n\n# T\n", out
    assert issues.set_fields("---\nclosed: x\n---\n\n# T\n", {"closed": None}) == "# T\n"
    assert issues.set_fields("# T\n", {"a": "b: c"}) == '---\na: "b: c"\n---\n# T\n'
    listed = issues.set_fields("---\ntags:\n  - a\nx: 1\n---\n# T\n", {"tags": ["a", "dev.py", "b c", "k:v"], "blocked-by": []})
    assert listed == '---\ntags: [a, dev.py, b c, "k:v"]\nx: 1\n---\n# T\n', listed
    assert issues._parse_meta(listed)[0]["tags"] == ["a", "dev.py", "b c", "k:v"]
    assert issues.set_fields(listed, {"tags": []}) == "---\nx: 1\n---\n# T\n"


def test_notes_section_round_trips():
    text = "---\nx: 1\n---\n\n# T\n\nBody.\n\n```\n## Notes\n```\n"
    added = serve.set_notes(issues, text, "first\n\nsecond")
    assert added == text.rstrip("\n") + "\n\n## Notes\n\nfirst\n\nsecond\n", added
    assert serve.split_notes(issues, added) == ("Body.\n\n```\n## Notes\n```", "first\n\nsecond")
    middle = "# T\n\n## Notes\n\nold\n\n## Plan\n\nsteps\n"
    assert serve.set_notes(issues, middle, "new") == "# T\n\n## Notes\n\nnew\n\n## Plan\n\nsteps\n"
    assert serve.set_notes(issues, middle, "") == "# T\n\n## Plan\n\nsteps\n"
    assert serve.set_notes(issues, added, "  ") == text.rstrip("\n") + "\n"


def test_new_writes_a_valid_issue(base: Path):
    a, _ = setup(base)
    code, out, _ = cli(a, "new", "bug", "bad-input", "--in", "src", "-t", "Bad input", "--ref", "file:src/lib.cc:1",
                       "--tag", "x", "--priority", "next")
    assert code == 0 and out.strip() == "src/docs/issues/bug-bad-input.md", out
    text = (a / "src/docs/issues/bug-bad-input.md").read_text(encoding="utf-8")
    assert "priority: next" in text and "  - file: src/lib.cc:1" in text and "# Bad input" in text, text
    assert cli(a, "check")[0] == 0
    code, _, err = cli(a, "new", "bug-bad-input")
    assert code == 2 and "exists already" in err, err
    assert cli(a, "new", "nope", "x")[0] == 2


def test_check_finds_what_is_wrong(base: Path):
    a, _ = setup(base)
    write(a / "docs/issues/oops.md", "# T\n")
    write(a / "docs/issues/closed/2026/3/bug-old.md", issue_text("Old", "closed: 2026-03-01", "resolution: fixed"))
    write(a / "docs/issues/closed/2026/04/bug-older.md", issue_text("Older", "closed: 2026-03-01", "resolution: fixed"))
    write(a / "docs/issues/feat-child.md", issue_text("Child", "parent: feat-shiny", "blocked-by: [bug-ghost]"))
    write(a / "lib/docs/issues/bug-crash.md", issue_text("Twin"))
    write(a / "docs/issues/bug-notitle.md", "---\nclaimed-by: X\n---\n\nno title\n")
    write(a / ".gitignore", "/fetched/\n")
    write(a / "docs/issues/bug-stale-ref.md",
          issue_text("Stale", "refs:", "  - file: src/lib.cc:3-9", "  - file: gone.cc", "  - file: fetched/sdk.h"))
    write(a / "src/use.cc", "// issue: bug-ghost\nx(); // TEMPORARY(bug-older)\n// issue: other:bug-unknown-repo\n")
    code, out, _ = cli(a, "check")
    assert code == 1
    for expected in ("oops.md:1: the file name must be", "closed/2026/3/bug-old.md:1: a closed issue lives in",
                     "bug-older.md:3: closed 2026-03-01 belongs in closed/2026/03/", "feat-child.md:3: parent feat-shiny is not an epic",
                     "feat-child.md:4: blocked-by bug-ghost does not exist", "id bug-crash is used 2 times",
                     "bug-notitle.md:5: the first line after the frontmatter must be the title",
                     "bug-notitle.md:2: claimed-by and claimed-at go together",
                     "refs src/lib.cc:3-9, but src/lib.cc has 3 lines", "refs file gone.cc, which does not exist",
                     "src/use.cc:1: marker names bug-ghost, which does not exist",
                     "src/use.cc:2: marker names bug-older, which is closed"):
        assert expected in out, f"missing {expected!r} in:\n{out}"
    assert "bug-unknown-repo" not in out, "a marker into a repository that is not checked out is not judged"
    assert "fetched/sdk.h" not in out, "a ref to a path git ignores names a fetched file, which may be absent here"


def test_claim_pushes_exactly_one_commit(base: Path):
    a, b = setup(base)
    remote = base / "remote.git"
    before = git(remote, "rev-parse", "main")
    write(a / "wip.txt", "unpushed work\n")
    git(a, "add", "wip.txt")
    git(a, "commit", "-q", "-m", "local work")
    wip = git(a, "rev-parse", "HEAD")
    write(a / "dirty.txt", "uncommitted\n")

    code, _, err = cli(a, "claim", "bug-crash")
    assert code == 0, err
    tip = git(remote, "rev-parse", "main")
    assert git(remote, "log", "-1", "--format=%P %s", "main") == f"{before} claim bug-crash"
    assert subprocess.run(["git", "-C", str(remote), "cat-file", "-e", wip]).returncode != 0, "the local commit stayed local"
    claimed = git(remote, "show", "main:docs/issues/bug-crash.md")
    assert "claimed-by: Alice" in claimed and "claimed-at: 20" in claimed, claimed
    assert git(a, "log", "-1", "--format=%P", "HEAD") == f"{wip} {tip}", "merged into the diverged local branch"
    assert (a / "dirty.txt").is_file() and "claimed-by: Alice" in (a / "docs/issues/bug-crash.md").read_text(encoding="utf-8")

    # Bob's checkout is stale; the claim is judged against the remote, so he is refused.
    code, _, err = cli(b, "claim", "bug-crash")
    assert code == 2 and "claimed by Alice" in err, err
    code, _, err = cli(b, "release", "bug-crash")
    assert code == 2 and "not Bob" in err, err
    assert cli(a, "release", "bug-crash")[0] == 0
    assert "claimed-by" not in git(remote, "show", "main:docs/issues/bug-crash.md")


def test_claim_fast_forwards_a_clean_checkout(base: Path):
    a, _ = setup(base)
    assert cli(a, "claim", "feat-shiny", "--as", "Claude (for Alice)")[0] == 0
    assert git(a, "rev-parse", "HEAD") == git(base / "remote.git", "rev-parse", "main")
    assert "claimed-by: Claude (for Alice)" in (a / "docs/issues/feat-shiny.md").read_text(encoding="utf-8")
    code, out, _ = cli(a, "list", "--mine")
    assert "feat-shiny" in out and "@Claude (for Alice)" in out, out


def test_claim_retries_when_someone_pushes_first(base: Path):
    a, b = setup(base)
    real_git, raced = issues._git, []

    def racing_git(root, *argv, **kw):
        if argv[:1] == ("push",) and not raced:
            raced.append(True)
            write(a / "other.txt", "x\n")
            git(a, "add", "other.txt")
            git(a, "commit", "-q", "-m", "someone else")
            git(a, "push", "-q", "origin", "main")
        return real_git(root, *argv, **kw)

    issues._git = racing_git
    try:
        code, _, err = cli(b, "claim", "feat-shiny")
    finally:
        issues._git = real_git
    assert code == 0 and raced, err
    assert git(base / "remote.git", "log", "-2", "--format=%s", "main").splitlines() == ["claim feat-shiny", "someone else"]


def test_claim_of_a_new_issue_adds_it(base: Path):
    _, b = setup(base)
    assert cli(b, "new", "perf", "slow-path")[0] == 0
    code, _, err = cli(b, "claim", "perf-slow")
    assert code == 0, err
    assert "claimed-by: Bob" in git(base / "remote.git", "show", "main:docs/issues/perf-slow-path.md")
    assert git(b, "status", "--porcelain") == "", "the file is now tracked, as pushed"


def test_publish_files_pushes_only_the_chosen_files(base: Path):
    a, b = setup(base)
    remote = base / "remote.git"
    write(a / "wip.txt", "unpushed\n")
    git(a, "add", "wip.txt")
    git(a, "commit", "-q", "-m", "local work")
    wip = git(a, "rev-parse", "HEAD")
    write(a / "docs/issues/bug-crash.md", issue_text("It crashes", "priority: next"))
    write(a / "docs/issues/feat-shiny.md", issue_text("Shiny, edited but not chosen"))
    write(a / "docs/issues/chore-new.md", issue_text("New"))
    repo = issues.Workspace(a).top
    with redirect_stdout(io.StringIO()):
        issues.publish_files(repo, ["docs/issues/bug-crash.md", "docs/issues/chore-new.md"], "issues: triage")
    assert git(remote, "log", "-1", "--format=%s", "main") == "issues: triage"
    assert git(remote, "diff", "--name-only", "main~1", "main").splitlines() == ["docs/issues/bug-crash.md", "docs/issues/chore-new.md"]
    assert subprocess.run(["git", "-C", str(remote), "cat-file", "-e", wip]).returncode != 0, "the local commit stayed local"
    status = git(a, "status", "--porcelain")
    assert status == "M docs/issues/feat-shiny.md", f"the unchosen edit is still outstanding, and nothing else: {status!r}"

    # Bob edits a file the remote has changed since his HEAD: refused rather than overwritten.
    write(b / "docs/issues/bug-crash.md", issue_text("It crashes", "priority: someday"))
    try:
        with redirect_stdout(io.StringIO()):
            issues.publish_files(issues.Workspace(b).top, ["docs/issues/bug-crash.md"], "issues: triage")
        raise AssertionError("expected a refusal")
    except issues.IssueError as e:
        assert "differs between HEAD" in str(e), e


def test_serve_edits_files_and_pushes_on_request(base: Path):
    import json
    import threading
    import urllib.request

    a, _ = setup(base)
    write(a / "docs/issues/epic-big.md", issue_text("Big"))
    server, state = serve.make_server(issues, a)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}"

    def call(path: str, body: dict | None = None, token: str | None = state.token) -> tuple[int, dict]:
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(url + path, data=data, headers={"Content-Type": "application/json", **(
            {"X-Issues-Token": token} if token else {})})
        try:
            with urllib.request.urlopen(req) as res:
                return res.status, json.loads(res.read())
        except urllib.error.HTTPError as e:
            raw = e.read()
            return e.code, json.loads(raw) if raw.startswith(b"{") else {}

    try:
        code, snap = call("/api/issues")
        assert code == 200 and {i["qid"] for i in snap["issues"]} == {"bug-crash", "feat-shiny", "epic-big"}, snap
        assert call("/api/edit", {"qid": "bug-crash", "fields": {"priority": "next"}}, token=None)[0] == 403
        assert call("/api/edit", {"qid": "bug-crash", "fields": {"priority": "next", "tags": ["x"]}, "notes": "seen"})[0] == 200
        text = (a / "docs/issues/bug-crash.md").read_text(encoding="utf-8")
        assert "priority: next\ntags: [x]\n---" in text and text.endswith("## Notes\n\nseen\n"), text
        assert git(a, "log", "-1", "--format=%s") == "seed", "an edit commits nothing"

        code, err = call("/api/edit", {"qid": "feat-shiny", "fields": {"parent": "bug-crash"}})
        assert code == 400 and "is not an epic" in err["error"], err
        assert "parent" not in (a / "docs/issues/feat-shiny.md").read_text(encoding="utf-8"), "a refused edit is taken back"
        assert call("/api/edit", {"qid": "feat-shiny", "fields": {"parent": "epic-big", "blocked-by": ["bug-crash"]}})[0] == 200
        shiny = next(i for i in call("/api/issues")[1]["issues"] if i["qid"] == "feat-shiny")
        assert shiny["parent_qid"] == "epic-big" and shiny["blocked"], shiny

        assert call("/api/rename-tag", {"from": "x", "to": "y"})[1] == {"renamed": 1}
        code, pending = call("/api/pending")
        assert sorted(p["rel"] for p in pending) == ["docs/issues/bug-crash.md", "docs/issues/epic-big.md", "docs/issues/feat-shiny.md"]
        assert all(p["touched"] == (p["rel"] != "docs/issues/epic-big.md") for p in pending), pending
        code, err = call("/api/publish", {"paths": [str(a / "src/lib.cc")]})
        assert code == 400 and "not an issue file" in err["error"], err

        code, err = call("/api/publish", {"paths": [p["path"] for p in pending if p["touched"]]})
        assert code == 400 and "names epic-big, which is not committed" in err["error"], err
        code, out = call("/api/publish", {"paths": [p["path"] for p in pending], "message": "issues: triage"})
        assert code == 200, out
        assert git(base / "remote.git", "log", "-1", "--format=%s", "main") == "issues: triage"
        assert call("/api/pending")[1] == [] and git(a, "status", "--porcelain") == ""
    finally:
        server.shutdown()
        server.server_close()


def test_close_moves_and_relinks(base: Path):
    a, _ = setup(base)
    write(a / "docs/notes.md", "See [the crash](issues/bug-crash.md#repro) and [shiny](issues/feat-shiny.md).\n")
    write(a / "docs/issues/feat-shiny.md", issue_text("Shiny", "parent: epic-big") + "Related: [notes](../notes.md).\n")
    write(a / "docs/issues/epic-big.md", issue_text("Big"))
    code, _, err = cli(a, "close", "feat-shiny", "--date", "2026-03-05", "--resolution", "obsolete")
    assert code == 0, err
    moved = a / "docs/issues/closed/2026/03/feat-shiny.md"
    text = moved.read_text(encoding="utf-8")
    assert "closed: 2026-03-05\nresolution: obsolete" in text and "[notes](../../../../notes.md)" in text, text
    notes = (a / "docs/notes.md").read_text(encoding="utf-8")
    assert "(issues/bug-crash.md#repro)" in notes and "(issues/closed/2026/03/feat-shiny.md)" in notes, notes
    assert cli(a, "check")[0] == 0
    code, out, _ = cli(a, "show", "epic-big")
    assert "children: 1 of 1 closed" in out, out
    assert cli(a, "reopen", "feat-shiny")[0] == 0
    assert (a / "docs/issues/feat-shiny.md").is_file() and "closed:" not in (a / "docs/issues/feat-shiny.md").read_text()
    assert cli(a, "check")[0] == 0


def test_known_matches_refs_and_markers(base: Path):
    a, _ = setup(base)
    write(a / "docs/issues/bug-flaky.md", issue_text("Flaky", "refs:", "  - test: kernels-test/segment*",
                                                  "  - symbol: sol::h256::intersect", "  - file: src/"))
    write(a / "src/more.cc", "a\nb // issue: feat-shiny\n")

    def hits(query: str) -> str:
        return cli(a, "known", query)[1]

    assert "bug-crash" in hits("src/lib.cc:3") and "bug-crash" not in hits("src/lib.cc:1")
    assert "bug-flaky" in hits("src/lib.cc:1"), "a directory ref covers its files"
    assert "feat-shiny" in hits("src/more.cc:1") and "marked at src/more.cc:2" in hits("src/more.cc:1"), "a marker counts for its file"
    assert "bug-flaky" in hits("kernels-test/segment-triangle")
    assert "bug-flaky" in hits("intersect") and "bug-flaky" in hits("h256::intersect")
    assert cli(a, "known", "nothing-like-this")[0] == 1


def test_submodules_are_their_own_repositories(base: Path):
    a, _ = setup(base)
    git(base, "init", "-q", "-b", "main", "lib")
    lib = base / "lib"
    git(lib, "config", "user.name", "L")
    git(lib, "config", "user.email", "l@example.com")
    write(lib / "docs/issues/bug-deep.md", issue_text("Deep"))
    git(lib, "add", "-A")
    git(lib, "commit", "-q", "-m", "lib")
    git(a, "submodule", "add", "-q", "-b", "main", str(lib), "extern/lib")
    write(a / "src/x.cc", "// issue: lib:bug-deep\n")

    assert "lib:bug-deep" not in cli(a, "list")[1], "not listed by default"
    assert "lib:bug-deep" in cli(a, "--with-submodules", "list")[1]
    write(a / ".issues.toml", 'name = "demo"\nsubmodules = ["extern/lib"]\n')
    assert "lib:bug-deep" in cli(a, "list")[1]
    assert "lib:bug-deep" in cli(a, "show", "bug-deep")[1]
    assert cli(a, "check")[0] == 0, "a qualified marker resolves into the submodule"
    assert cli(a, "--top-only", "list")[1].count("bug-deep") == 0


# ---------------------------------------------------------------------------

def main() -> int:
    tests = [(name, fn) for name, fn in globals().items() if name.startswith("test_")]
    failed = 0
    for name, fn in tests:
        with tempfile.TemporaryDirectory(prefix="issues-test-") as tmp:
            try:
                fn(Path(tmp)) if fn.__code__.co_argcount else fn()
                print(f"ok    {name}")
            except Exception:
                failed += 1
                print(f"FAIL  {name}")
                traceback.print_exc()
    print(f"{len(tests) - failed} of {len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
