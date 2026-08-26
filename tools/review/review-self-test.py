#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["pygments>=2.17", "markdown-it-py>=3"]
# ///

# The render path is the only part of the tool needing those two, and the only part nothing but the server reaches.
# The suite carries them rather than leaving the one layer a crash can hide in untested.

"""Self-test for the review tool: the interval algebra, change identity, coverage math and the block grammar.

Every test builds a throwaway git repository, so what is checked is real git output rather than a fixture of it.
Kept out of `dev.py test`, which drives the C++ nexus suites; `dev.py check` runs this as its own gate.

Run it directly with `uv run tools/review/review-self-test.py [-v]`.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.review.lib.annotate.index import RepoIndex  # noqa: E402
from tools.review.lib.annotate.table import build as build_tokens  # noqa: E402
from tools.review.lib.annotate.providers import CommitProvider  # noqa: E402
from tools.review.lib.annotate.glossary import GlossaryProvider, malformed_in, terms_in  # noqa: E402
from tools.review.lib.entry.generate import _tree_html as tree_html  # noqa: E402
from tools.review.lib.serve.app import _forge_commit_url as forge_url  # noqa: E402
from tools.review.lib.changeset import commits as commit_ingest  # noqa: E402
from tools.review.lib.changeset.ids import allocate, allocate_many, digest_of  # noqa: E402
from tools.review.lib.changeset.ingest import bulk_candidate, candidates_for, group_hunks, register  # noqa: E402
from tools.review.lib.changeset.ledger import Change, Ledger  # noqa: E402
from tools.review.lib.entry.answers import AnswerFile  # noqa: E402
from tools.review.lib.entry.askhash import hash_ask  # noqa: E402
from tools.review.lib.entry.grammar import ReviewParseError, ack_name  # noqa: E402
from tools.review.lib.entry.parse import parse_text  # noqa: E402
from tools.review.lib.goals.skeleton import thinly_discharged  # noqa: E402
from tools.review.lib.entry.write import (  # noqa: E402
    append_text, check_supersedes, compose, immutability_violations, stamp_rounds,
)
from tools.review.lib.render.markdown import render as render_markdown  # noqa: E402
from tools.review.lib.git.diffparse import parse as parse_diff  # noqa: E402
from tools.review.lib.git.run import Git  # noqa: E402
from tools.review.lib.space.intervals import IntervalList  # noqa: E402
from tools.review.lib.space.netspace import ADDED, REMOVED, build as build_net  # noqa: E402

VERBOSE = "-v" in sys.argv


# ---- a throwaway repository -------------------------------------------------


def git_init(root: Path) -> Git:
    for args in (
        ["init", "--quiet", "-b", "main"],
        ["config", "user.email", "self-test@example.com"],
        ["config", "user.name", "review self-test"],
        ["config", "commit.gpgsign", "false"],
    ):
        subprocess.run(["git", *args], cwd=root, check=True, capture_output=True)
    return Git(root)


def commit(root: Path, message: str, files: dict[str, str]) -> str:
    for name, content in files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if content is None:
            target.unlink(missing_ok=True)
        else:
            target.write_text(content, encoding="utf-8", newline="\n")
    subprocess.run(["git", "add", "-A"], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "commit", "--quiet", "-m", message], cwd=root, check=True, capture_output=True)
    out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True, capture_output=True, text=True)
    return out.stdout.strip()


def numbered(count: int, *, mark: str = "line") -> str:
    return "".join(f"{mark} {i}\n" for i in range(1, count + 1))


def compose_append(entry, addition: str) -> str:
    """What `review append` would write: the entry's text with `addition` spliced on the end."""
    return append_text(entry, addition)


# ---- intervals --------------------------------------------------------------


def test_interval_algebra(root: Path) -> None:
    a = IntervalList.of([(1, 5), (10, 12)])
    b = IntervalList.of([(4, 11)])

    assert len(a) == 8, len(a)
    assert a.union(b).spans == ((1, 12),), a.union(b).spans
    assert a.intersect(b).spans == ((4, 5), (10, 11)), a.intersect(b).spans
    assert a.subtract(b).spans == ((1, 3), (12, 12)), a.subtract(b).spans
    assert IntervalList().subtract(a).spans == ()
    assert a.subtract(a).spans == ()


def test_interval_normalization(root: Path) -> None:
    # Adjacent runs merge, because a set of integers has no notion of two runs that touch.
    merged = IntervalList.of([(3, 4), (1, 2), (9, 9)])
    assert merged.spans == ((1, 4), (9, 9)), merged.spans
    assert IntervalList.from_numbers([5, 3, 4, 9]).spans == ((3, 5), (9, 9))
    assert IntervalList.of([(5, 1)]).spans == ()


def test_interval_gap_closing(root: Path) -> None:
    runs = IntervalList.of([(1, 2), (20, 21), (60, 61)])
    assert runs.gaps_closed(20).spans == ((1, 21), (60, 61)), runs.gaps_closed(20).spans
    assert runs.gaps_closed(0).spans == runs.spans


# ---- change identity --------------------------------------------------------


def test_id_extends_on_collision(root: Path) -> None:
    digest = digest_of("libs/foo.cc", "@@\n+one")
    first = allocate(digest, set())
    second = allocate(digest, {first})
    third = allocate(digest, {first, second})

    assert len(first) == len("CHANGE-") + 5, f"a fresh id is five characters, got {first}"
    assert second.startswith(first), (first, second)
    assert third.startswith(second), (second, third)
    assert len({first, second, third}) == 3


def test_id_allocation_is_order_independent(root: Path) -> None:
    digests = [digest_of("a", str(i)) for i in range(12)]
    forward = allocate_many(digests, set())
    backward = allocate_many(list(reversed(digests)), set())
    assert forward == backward, "id allocation must not depend on the order the diff emitted its hunks"


def test_hunk_hash_survives_a_shift(root: Path) -> None:
    """A hunk whose content is unchanged keeps its id when unrelated lines above it move."""
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(120)})
    # The edit sits far from the top, so the prelude below stays a separate change rather than coalescing with it.
    body = numbered(120).replace("line 100\n", "line 100 CHANGED\n")
    head_one = commit(root, "edit", {"a.txt": body})

    net = build_net(git, base, head_one)
    before = candidates_for(git, base, head_one, context=3, gap=20, net=net)
    hunk_digests = {c.digest for c in before if c.kind == "hunk"}

    # Insert lines above the edit, which shifts every following line number but changes no hunk content.
    shifted = "prelude a\nprelude b\n" + body
    head_two = commit(root, "prelude", {"a.txt": shifted})
    net_two = build_net(git, base, head_two)
    after = candidates_for(git, base, head_two, context=3, gap=20, net=net_two)
    after_digests = {c.digest for c in after if c.kind == "hunk"}

    assert hunk_digests & after_digests, (
        "the unchanged hunk must keep its digest across a shift; "
        f"before={sorted(hunk_digests)} after={sorted(after_digests)}"
    )


# ---- coverage math ----------------------------------------------------------


def test_default_ingest_covers_everything(root: Path) -> None:
    """The default sweep closes gate 1 by construction, whatever the change looks like."""
    git = git_init(root)
    base = commit(root, "base", {
        "a.txt": numbered(40),
        "b.txt": "kept\n",
        "gone.txt": "delete me\n",
    })
    body = numbered(40).replace("line 5\n", "line 5 edited\n").replace("line 33\n", "")
    head = commit(root, "work", {
        "a.txt": body,
        "gone.txt": None,
        "new.txt": "brand new\nsecond line\n",
    })

    net = build_net(git, base, head)
    assert len(net) > 0
    candidates = candidates_for(git, base, head, context=8, gap=20, net=net)

    claimed = net.__class__.empty()
    for candidate in candidates:
        claimed = claimed.union(candidate.claim)
    left = net.subtract(claimed)
    assert left.is_empty, f"default ingest left {len(left)} atoms unaccounted: {left.runs()}"


def test_removed_lines_are_keyed_on_the_base_side(root: Path) -> None:
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(10)})
    head = commit(root, "cut", {"a.txt": numbered(10).replace("line 4\n", "")})

    net = build_net(git, base, head)
    assert net.get(REMOVED, "a.txt").spans == ((4, 4),), net.get(REMOVED, "a.txt").spans
    assert not net.get(ADDED, "a.txt"), "a pure deletion adds nothing"


def test_context_lines_are_not_atoms(root: Path) -> None:
    """A wide display context must not inflate what the review is accountable for."""
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(60)})
    head = commit(root, "edit", {"a.txt": numbered(60).replace("line 30\n", "line 30 edited\n")})

    net = build_net(git, base, head)
    assert len(net) == 2, f"one edited line is one add plus one remove, got {len(net)}"

    candidates = candidates_for(git, base, head, context=8, gap=20, net=net)
    assert len(candidates) == 1
    assert len(candidates[0].claim) == 2, "the claim must be the changed lines, not the whole displayed span"


def test_file_atoms_are_accounted_for(root: Path) -> None:
    git = git_init(root)
    base = commit(root, "base", {"a.txt": "content\n"})
    subprocess.run(["git", "mv", "a.txt", "b.txt"], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "commit", "--quiet", "-m", "rename"], cwd=root, check=True, capture_output=True)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                          capture_output=True, text=True).stdout.strip()

    net = build_net(git, base, head)
    assert any(atom.kind == "rename" for atom in net.files), f"a pure rename must be an atom, got {net.files}"

    candidates = candidates_for(git, base, head, context=8, gap=20, net=net)
    claimed = net.__class__.empty()
    for candidate in candidates:
        claimed = claimed.union(candidate.claim)
    assert net.subtract(claimed).is_empty, "a rename must be claimable"


def test_binary_and_mode_changes_are_atoms(root: Path) -> None:
    """Git writes no `---`/`+++` for either, so the path can only come from the `diff --git` header.

    Without that fallback the file is dropped from the parsed diff entirely, which is a change with no atom
    and no id, under a coverage gate still reporting green.
    """
    git = git_init(root)
    (root / "img.bin").write_bytes(b"\x00\x01\x02before")
    (root / "run.sh").write_text("echo hi\n", encoding="utf-8", newline="\n")
    base = commit(root, "base", {"a.txt": "content\n"})

    (root / "img.bin").write_bytes(b"\x00\x01\x02after-and-longer")
    subprocess.run(["git", "update-index", "--chmod=+x", "run.sh"], cwd=root, check=True, capture_output=True)
    head = commit(root, "binary and mode", {})

    net = build_net(git, base, head)
    assert "img.bin" in net.paths(), f"a modified binary file must reach net space, got {net.paths()}"
    assert any(a.kind == "binary" for a in net.files), f"expected a binary atom, got {net.files}"
    assert any(a.kind == "mode" for a in net.files), f"expected a mode atom, got {net.files}"

    candidates = candidates_for(git, base, head, context=8, gap=20, net=net)
    claimed = net.__class__.empty()
    for candidate in candidates:
        claimed = claimed.union(candidate.claim)
    assert net.subtract(claimed).is_empty, "both must be claimable by the default sweep"


def test_commit_local_claim_survives_a_later_insertion(root: Path) -> None:
    """An insertion hunk is numbered by the line before it, so the boundary line must not take its delta.

    The line the earlier commit wrote is exactly that boundary once a later commit inserts under it,
    and mapping it forward by the insertion's length silently hands its claim to a line someone else wrote.
    """
    git = git_init(root)
    base = commit(root, "base", {"f.txt": "a\nb\nc\n"})
    first = commit(root, "insert P", {"f.txt": "a\nb\nP\nc\n"})
    head = commit(root, "insert under P", {"f.txt": "a\nb\nP\nQ\nR\nc\n"})

    net = build_net(git, base, head)
    assert net.get(ADDED, "f.txt").spans == ((3, 5),), net.get(ADDED, "f.txt").spans

    candidates = commit_ingest.candidates_for_commit(
        git, first, base=base, head=head, context=3, gap=20, net=net,
    )
    claimed = net.__class__.empty()
    for candidate in candidates:
        claimed = claimed.union(candidate.claim)
    assert claimed.get(ADDED, "f.txt").spans == ((3, 3),), (
        f"the commit wrote head line 3, so that is what it claims; got {claimed.get(ADDED, 'f.txt').spans}"
    )


def test_rest_only_claims_what_is_left(root: Path) -> None:
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(20), "b.txt": numbered(20)})
    head = commit(root, "edit", {
        "a.txt": numbered(20).replace("line 3\n", "line 3 edited\n"),
        "b.txt": numbered(20).replace("line 7\n", "line 7 edited\n"),
    })

    net = build_net(git, base, head)
    with tempfile.TemporaryDirectory(prefix="review-ledger-") as ledger_dir:
        ledger = Ledger(Path(ledger_dir) / "ledger.jsonl")

        # A bulk claim covers a.txt wholesale, so --rest must not create a hunk change for it.
        bulk = bulk_candidate(net, selector="a.txt", reason="mechanical", matches=lambda p: p == "a.txt")
        assert bulk is not None
        register(ledger, [bulk], round_number=1, write_body=lambda *_: None)

        everything = candidates_for(git, base, head, context=8, gap=20, net=net)
        result = register(ledger, everything, round_number=1, write_body=lambda *_: None,
                          only_uncovered=True)
        assert result.skipped_covered == 1, f"the bulk-covered hunk must be skipped, got {result.skipped_covered}"
        assert [c.path for c in result.created] == ["b.txt"], [c.path for c in result.created]
        assert net.subtract(ledger.covered()).is_empty

        # Re-running --rest is a no-op: the hunk it created is reused, and the bulk-covered one is skipped again.
        again = register(ledger, everything, round_number=1, write_body=lambda *_: None,
                         only_uncovered=True)
        assert not again.created, [c.summary for c in again.created]
        assert len(again.reused) == 1 and again.skipped_covered == 1, (again.reused, again.skipped_covered)


# ---- commit-local mapping ---------------------------------------------------


def test_commit_local_claims_only_surviving_lines(root: Path) -> None:
    """A line a later commit rewrote belongs to that commit, not to the one being ingested."""
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(30)})
    first = commit(root, "first", {
        "a.txt": numbered(30).replace("line 5\n", "line 5 by first\n").replace("line 25\n", "line 25 by first\n"),
    })
    head = commit(root, "second", {
        "a.txt": numbered(30).replace("line 5\n", "line 5 by first\n").replace("line 25\n", "line 25 by second\n"),
    })

    net = build_net(git, base, head)
    found = commit_ingest.candidates_for_commit(
        git, first, base=base, head=head, context=3, gap=20, net=net,
    )
    claimed = net.__class__.empty()
    for candidate in found:
        claimed = claimed.union(candidate.claim)

    assert claimed.get(ADDED, "a.txt").contains(5), "the line first wrote and kept must be claimed"
    assert not claimed.get(ADDED, "a.txt").contains(25), "the line second rewrote must not be claimed by first"
    assert net.subtract(claimed), "the second commit's line must still be left for another change"


def test_bulk_by_commit_claims_what_the_commit_contributed(root: Path) -> None:
    """A mechanical commit becomes one claim, exact to the lines that still stand at head."""
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(20), "b.txt": numbered(20)})
    sweep = commit(root, "sweep", {
        "a.txt": numbered(20, mark="LINE"),
        "b.txt": numbered(20),
    })
    head = commit(root, "later", {
        "a.txt": numbered(20, mark="LINE"),
        "b.txt": numbered(20).replace("line 9\n", "line 9 edited\n"),
    })

    net = build_net(git, base, head)
    claimed = commit_ingest.commit_atoms(git, sweep, base=base, head=head, net=net)

    assert claimed.get(ADDED, "a.txt"), "the sweep's own lines must be claimed"
    assert not claimed.get(ADDED, "b.txt"), "a line another commit wrote must not be claimed by the sweep"
    assert net.subtract(claimed), "the later edit must be left for another change"

    candidate = commit_ingest.bulk_candidate_for_commits(
        git, [sweep], base=base, head=head, net=net, reason="mechanical rename", label="sweep",
    )
    assert candidate is not None
    assert candidate.kind == "bulk" and candidate.reason == "mechanical rename"
    assert not candidate.body, "a bulk claim writes no hunk body, which is the point of it"
    assert len(candidate.claim) == len(claimed)


def test_bulk_by_commit_narrows_by_path(root: Path) -> None:
    git = git_init(root)
    base = commit(root, "base", {"src/a.txt": numbered(10), "docs/b.txt": numbered(10)})
    sweep = commit(root, "sweep", {
        "src/a.txt": numbered(10, mark="LINE"),
        "docs/b.txt": numbered(10, mark="LINE"),
    })

    net = build_net(git, base, sweep)
    everything = commit_ingest.bulk_candidate_for_commits(
        git, [sweep], base=base, head=sweep, net=net, reason="all of it", label="sweep",
    )
    only_src = commit_ingest.bulk_candidate_for_commits(
        git, [sweep], base=base, head=sweep, net=net, reason="src only", label="sweep",
        matches=lambda p: p.startswith("src/"),
    )
    assert only_src is not None
    assert len(only_src.claim) < len(everything.claim)
    assert not only_src.claim.get(ADDED, "docs/b.txt")


def test_commit_local_claims_stay_inside_net_space(root: Path) -> None:
    """Work added and then removed inside the range is not in net space, so it carries no obligation."""
    git = git_init(root)
    base = commit(root, "base", {"a.txt": numbered(10)})
    scratch = commit(root, "add scratch", {"a.txt": numbered(10) + "scratch\n"})
    head = commit(root, "drop scratch", {"a.txt": numbered(10)})

    net = build_net(git, base, head)
    assert net.is_empty, "adding then removing the same line leaves nothing to account for"

    found = commit_ingest.candidates_for_commit(
        git, scratch, base=base, head=head, context=3, gap=20, net=net,
    )
    assert not found, f"a commit whose work did not survive claims nothing, got {[c.summary for c in found]}"


# ---- diff parsing -----------------------------------------------------------


def test_hunk_sides_walk_both_counters(root: Path) -> None:
    text = (
        "diff --git a/a.txt b/a.txt\n"
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -10,4 +10,4 @@ ctx\n"
        " keep\n"
        "-gone\n"
        "+fresh\n"
        " keep2\n"
    )
    files = parse_diff(text)
    assert len(files) == 1
    added, removed = files[0].hunks[0].sides()
    assert added == [11], added
    assert removed == [11], removed


def test_group_hunks_respects_the_gap(root: Path) -> None:
    text = (
        "diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n"
        "@@ -1,1 +1,1 @@\n-a\n+b\n"
        "@@ -50,1 +50,1 @@\n-c\n+d\n"
    )
    hunks = parse_diff(text)[0].hunks
    assert len(group_hunks(hunks, 20)) == 2, "hunks 49 lines apart are two changes at gap 20"
    assert len(group_hunks(hunks, 60)) == 1, "the same hunks are one change at gap 60"


# ---- the block grammar ------------------------------------------------------


ENTRY = """---
id: 040
title: a question
group: correctness
severity: bug
---

## context/delta

Some delta context.

## changes  CHANGE-AAAA CHANGE-BBBB
show: collapsed

Commentary.

## ask  pick-one
discharges: CHANGE-AAAA

Which way?
Note: this line is prose, not an attribute.

- radio: the first way  (recommended)
- radio: the second way
- check: also add a test
"""


def test_grammar_round_trip(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"), slug="040-a")

    assert entry.id == "040"
    assert entry.severity == "bug"
    assert [b.type for b in entry.blocks] == ["context/delta", "changes", "ask"]
    assert entry.blocks[1].change_ids == ["CHANGE-AAAA", "CHANGE-BBBB"]

    ask = entry.ask("pick-one")
    assert ask is not None
    assert ask.discharges == ["CHANGE-AAAA"]
    assert len(ask.options) == 3
    assert ask.options[0].recommended and not ask.options[1].recommended
    assert ask.options[0].label == "the first way", ask.options[0].label
    assert "Note: this line is prose" in ask.prose, "prose ends the prelude, so a later colon is not an attribute"
    assert "radio:" not in ask.prose, "option lines are not prose"


def test_unknown_attribute_is_an_error(root: Path) -> None:
    text = ENTRY.replace("discharges: CHANGE-AAAA", "discharge: CHANGE-AAAA")
    try:
        parse_text(text, Path("entry.md"))
    except ReviewParseError as e:
        assert "discharges" in e.remedy, f"the remedy must suggest the real attribute, got {e.remedy!r}"
        return
    raise AssertionError("a typo'd attribute must not degrade silently into prose")


def test_blank_first_line_escapes_the_prelude(root: Path) -> None:
    text = ENTRY.replace("## ask  pick-one\ndischarges: CHANGE-AAAA\n\nWhich way?",
                         "## ask  pick-one\n\ndischarge: this is prose\nWhich way?")
    entry = parse_text(text, Path("entry.md"))
    ask = entry.ask("pick-one")
    assert "discharge: this is prose" in ask.prose
    assert not ask.discharges


def test_changes_block_must_declare_how_it_opens(root: Path) -> None:
    """`show:` is required, because defaulting it would make the quiet choice the unconsidered one."""
    missing = "---\nid: 1\ntitle: t\n---\n\n## changes  CHANGE-AAAA\n\nCommentary.\n"
    try:
        parse_text(missing, Path("x.md"))
    except ReviewParseError as e:
        assert "how it opens" in str(e), str(e)
    else:
        raise AssertionError("a `changes` block with no `show:` must be an error")

    try:
        parse_text(missing.replace("## changes  CHANGE-AAAA\n", "## changes  CHANGE-AAAA\nshow: maybe\n"), Path("x.md"))
    except ReviewParseError as e:
        assert "unknown show" in str(e), str(e)
    else:
        raise AssertionError("an unknown `show:` value must be an error")

    for kind in ("visible", "collapsed"):
        entry = parse_text(missing.replace("## changes  CHANGE-AAAA\n", f"## changes  CHANGE-AAAA\nshow: {kind}\n"), Path("x.md"))
        assert entry.blocks[0].attrs["show"] == kind


def test_every_block_type_renders(root: Path) -> None:
    """The renderer is only reachable through the server, so nothing else exercises it.

    A fenced code block threw on every entry that had one for the life of the tool, and the page turned that
    into nav items that ignored clicks — a crash two layers away from where it was visible.
    """
    from tools.review.lib.changeset.ledger import Change
    from tools.review.lib.core.paths import ReviewPaths
    from tools.review.lib.render.entryview import render_entry

    body = "---\nid: 1\ntitle: t\nseverity: bug\n---\n"
    for kind in ("context/cold", "context/repo", "context/delta", "prose", "recommendation"):
        body += f"\n## {kind}\n\nSome prose with `a/b.cc:12` in it.\n"
    body += "\n## code\n\n```python:x.py\ndef f():\n    return 1\n```\n"
    body += "\n## changes  CHANGE-AAAA\nshow: visible\n\nCommentary.\n"
    body += "\n## ask  a-question\ndischarges: CHANGE-AAAA\n\nWhich way?\n\n- radio: this way  (recommended)\n- check: and that\n"

    entry = parse_text(body, root / "entries" / "010-x.md", slug="010-x")
    ledger = Ledger(root / "ledger.jsonl")
    ledger.changes["CHANGE-AAAA"] = Change(id="CHANGE-AAAA", digest="d", kind="hunk", path="x.py", summary="x.py:1-2")

    html = render_entry(
        entry, AnswerFile(root / "a.json"),
        repo=root, paths=ReviewPaths(root), ledger=ledger, hash_of=hash_ask,
    )
    for needle in ("tier-delta-rule", "recommendation", "<pre class=\"pg\">", "class=\"changes\"", "ask-form"):
        assert needle in html, f"{needle!r} missing from the rendered entry"


def test_duplicate_ask_names_are_rejected(root: Path) -> None:
    text = ENTRY + "\n## ask  pick-one\n\nAgain?\n\n- radio: no\n"
    try:
        parse_text(text, Path("entry.md"))
    except ReviewParseError as e:
        assert "duplicate" in str(e)
        return
    raise AssertionError("ask names are the answer key, so duplicates must be rejected")


FENCED_SAMPLE = """
## prose

The shape:

```
## example  clean-core/vector
run: uv run dev.py example clean-core/vector
```
"""

TWO_PROSE = """
## prose

One.

## prose

Two.
"""


def test_a_fenced_block_can_hold_a_block_heading(root: Path) -> None:
    """An entry about the block grammar wants to quote it, and `## ` starts a block wherever it lands.

    Written after an append was refused for a fenced `## example` sample that parsed as a real block.
    """
    entry = parse_text(ENTRY + FENCED_SAMPLE, Path("entry.md"))
    assert [b.type for b in entry.blocks].count("prose") == 1, "the fenced sample must not become a block"
    assert "## example" in entry.blocks[-1].prose


def test_an_unterminated_fence_is_an_error(root: Path) -> None:
    """Reading the rest of the entry as code is the failure the fence rule would otherwise introduce."""
    try:
        parse_text(ENTRY + "\n## prose\n\n```\nnever closed\n", Path("entry.md"))
    except ReviewParseError as e:
        assert "never closed" in str(e)
        return
    raise AssertionError("an unterminated fence must be an error rather than swallowing the entry")


def test_block_names_are_derived_and_only_indexed_when_they_repeat(root: Path) -> None:
    """A lone block of its type keeps the bare name; a second one indexes both, never only one of the two."""
    entry = parse_text(ENTRY, Path("entry.md"))
    assert entry.block("context-delta") is not None
    assert entry.block("changes") is not None

    two = parse_text(ENTRY + TWO_PROSE, Path("entry.md"))
    names = [b.block_name for b in two.blocks if b.type == "prose"]
    assert names == ["prose#1", "prose#2"], names
    assert two.block("prose#1") is two.block("prose"), "the unindexed spelling must alias to #1"


def test_an_ask_is_named_by_its_heading(root: Path) -> None:
    """An ask already carries a unique name, so `ask#1` would be a second identity for the same thing."""
    entry = parse_text(ENTRY, Path("entry.md"))
    assert entry.block("pick-one") is entry.ask("pick-one")


def test_a_block_name_that_collides_is_rejected(root: Path) -> None:
    """A name is what a comment and a `supersedes:` anchor on, so two blocks answering to one are unresolvable."""
    try:
        parse_text(ENTRY + "\n## prose\nname: pick-one\n\nCollides with the ask.\n", Path("entry.md"))
    except ReviewParseError as e:
        assert "named" in str(e)
        return
    raise AssertionError("two blocks of one round cannot share a name")


def test_block_names_are_scoped_to_a_round(root: Path) -> None:
    """Round-scoped ordinals are why an append never renumbers a block the maintainer already anchored on."""
    text = ENTRY + "\n## prose\nround: 1\n\nFirst.\n\n## prose\nround: 2\n\nSecond.\n"
    prose = [b for b in parse_text(text, Path("entry.md")).blocks if b.type == "prose"]
    assert [b.anchor for b in prose] == ["r1/prose", "r2/prose"], [b.anchor for b in prose]


def test_stamping_leaves_every_other_byte_alone(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    stamped_text = stamp_rounds(entry, 2)
    assert stamped_text is not None

    stamped = parse_text(stamped_text, Path("entry.md"))
    assert all(b.round == 2 for b in stamped.blocks)
    assert stamped.ask("pick-one").prose == entry.ask("pick-one").prose
    assert hash_ask(stamped.ask("pick-one")) == hash_ask(entry.ask("pick-one")), \
        "stamping a round must not change what an ask hashes to"

    # Stamping is idempotent: a block that already carries a round is not touched again.
    assert stamp_rounds(stamped, 3) is None


def test_stamping_preserves_crlf(root: Path) -> None:
    """A splice that silently converted the whole file's line endings would not be a splice."""
    from tools.review.lib.entry.parse import parse_file
    from tools.review.lib.core.atomic import write_atomic

    for newline in ("\r\n", "\n"):
        target = root / f"050-{'crlf' if newline == '\r\n' else 'lf'}.md"
        target.write_bytes(ENTRY.replace("\n", newline).encode("utf-8"))

        entry = parse_file(target)
        assert entry.newline == newline, (newline, entry.newline)
        write_atomic(target, stamp_rounds(entry, 1))

        after = target.read_bytes()
        assert b"round: 1" in after, "the stamp must still land"
        assert (b"\r\n" in after) == (newline == "\r\n"), f"{newline!r} was not preserved"
        assert after.count(b"\r") == after.count(b"\r\n"), "no stray carriage returns"


def test_ledger_resolves_a_bare_code(root: Path) -> None:
    """An entry may name a change by its code alone, which is what a person retypes."""
    with tempfile.TemporaryDirectory(prefix="review-ledger-") as ledger_dir:
        ledger = Ledger(Path(ledger_dir) / "ledger.jsonl")
        ledger.append(Change(id="CHANGE-ABCDE", digest="d", kind="hunk", path="a.txt"))

        assert ledger.resolve("CHANGE-ABCDE") is not None
        assert ledger.resolve("abcde") is not None, "the bare code, lowercased, must resolve"
        assert ledger.resolve("CHANGE-NOPE1") is None, "an id that is not there must not resolve"


def test_ask_hash_ignores_bookkeeping(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    widened = parse_text(ENTRY.replace("discharges: CHANGE-AAAA", "discharges: CHANGE-AAAA CHANGE-CCCC"),
                         Path("entry.md"))
    assert hash_ask(entry.ask("pick-one")) == hash_ask(widened.ask("pick-one")), \
        "adding a change id is bookkeeping the maintainer never read"

    reworded = parse_text(ENTRY.replace("Which way?", "Which way should this go?"), Path("entry.md"))
    assert hash_ask(entry.ask("pick-one")) != hash_ask(reworded.ask("pick-one"))

    reordered = parse_text(ENTRY.replace(
        "- radio: the first way  (recommended)\n- radio: the second way",
        "- radio: the second way\n- radio: the first way  (recommended)"), Path("entry.md"))
    assert hash_ask(entry.ask("pick-one")) != hash_ask(reordered.ask("pick-one")), \
        "reordering the options changes what was offered"


def test_finalized_ask_is_immutable(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    finalized = {"pick-one": hash_ask(entry.ask("pick-one"))}
    assert not immutability_violations(entry, finalized)

    reworded = parse_text(ENTRY.replace("Which way?", "Which way now?"), Path("entry.md"))
    assert immutability_violations(reworded, finalized) == ["pick-one"]


def test_compose_is_parseable(root: Path) -> None:
    text = compose({"id": "010", "title": "Generated"}, ["## prose\ngenerated: overview\n\nBody."])
    entry = parse_text(text, Path("010-x.md"))
    assert entry.id == "010"
    assert entry.blocks[0].attrs["generated"] == "overview"


# ---- answers ----------------------------------------------------------------


def test_every_css_variable_is_defined(root: Path) -> None:
    """A `var(--x)` naming nothing falls back to inherited colour, so the rule silently does nothing.

    It renders — usually as almost the right thing — which is why eyeballing the page does not catch it.
    Written after `var(--dim)` shipped into a panel whose real token is `--text-dim`.
    """
    css = (REPO_ROOT / "tools" / "review" / "assets" / "app.css").read_text(encoding="utf-8")

    defined = set(re.findall(r"^\s*(--[\w-]+)\s*:", css, re.M))
    assert defined, "expected app.css to define custom properties"

    # A second argument is a real fallback, so those are deliberate rather than typos.
    used = set(re.findall(r"var\(\s*(--[\w-]+)\s*\)", css))

    missing = sorted(used - defined)
    assert not missing, f"app.css uses undefined custom properties: {', '.join(missing)}"


def test_every_hidden_element_can_actually_hide(root: Path) -> None:
    """An element the page toggles with `hidden` must not be left visible by its own `display` rule.

    `[hidden] { display: none }` comes from the user agent stylesheet and loses to any id selector that sets `display`,
    so a veil styled `#x { display: flex }` is shown permanently and the attribute the code sets is inert.
    Nothing else catches this: the markup is right, the code is right, and only the two together are wrong.
    """
    assets = REPO_ROOT / "tools" / "review" / "assets"
    page = (assets / "page.html").read_text(encoding="utf-8")
    css = (assets / "app.css").read_text(encoding="utf-8")

    toggled = set(re.findall(r'id="([\w-]+)"[^>]*\shidden\b', page))
    assert toggled, "expected the page to toggle something by `hidden`"

    for name in sorted(toggled):
        sets_display = re.search(r"#" + re.escape(name) + r"\s*(?:,[^{]*)?\{[^}]*\bdisplay\s*:", css)
        if not sets_display:
            continue
        guard = "#" + name + "[hidden]"
        assert guard in css, f"{name} sets display but has no `{guard} {{ display: none }}`, so `hidden` cannot hide it"


def test_answers_keep_a_reworded_question_s_answer(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    with tempfile.TemporaryDirectory(prefix="review-answers-") as answer_dir:
        path = Path(answer_dir) / "040-a.json"
        answers = AnswerFile.load(path, "040-a")
        block = entry.ask("pick-one")

        answers.upsert(block, selected=["the first way"], text="because", round_number=1)
        answers.finalize(1)
        answers.save()

        reloaded = AnswerFile.load(path, "040-a")
        assert not reloaded.get("pick-one").tentative
        assert reloaded.get("pick-one").round == 1

        reworded = parse_text(ENTRY.replace("Which way?", "Which way, really?"), Path("entry.md"))
        reloaded.upsert(reworded.ask("pick-one"), selected=[], text="new take", round_number=2)
        assert len(reloaded.orphans) == 1, "the finalized answer moves aside rather than being overwritten"
        assert next(iter(reloaded.orphans.values())).text == "because"
        assert reloaded.get("pick-one").text == "new take"


def test_answers_keep_a_finalized_answer_under_unchanged_wording(root: Path) -> None:
    """Re-answering an ask that was never reworded must still not overwrite the finalized answer.

    The reworded case is the obvious one and was always handled; this is the one that looks like a plain update
    and is how a round already handed back loses the text it quoted.
    """
    entry = parse_text(ENTRY, Path("entry.md"))
    with tempfile.TemporaryDirectory(prefix="review-answers-") as answer_dir:
        answers = AnswerFile.load(Path(answer_dir) / "040-a.json", "040-a")
        block = entry.ask("pick-one")

        answers.upsert(block, selected=["the first way"], text="the answer that round 1 quoted", round_number=1)
        answers.finalize(1)

        answers.upsert(block, selected=[], text="a second-round rethink", round_number=2)

        assert answers.get("pick-one").text == "a second-round rethink"
        assert len(answers.orphans) == 1, "the finalized answer must survive a same-wording re-answer"
        kept = answers.orphans["pick-one@r1"]
        assert kept.text == "the answer that round 1 quoted", kept.text
        assert kept.round == 1 and not kept.tentative


def test_an_entry_with_no_ask_carries_an_acknowledgement(root: Path) -> None:
    """Being read is progress, and an entry nobody opened must not look like one that is settled."""
    front = "---\nid: 050\ntitle: t\ngroup: docs\nstate: open\n---\n\n"

    silent = parse_text(front + "## prose\n\nNothing to decide.\n", Path("e.md"))
    assert [b.name for b in silent.asks] == [ack_name(1)]
    assert silent.acknowledgement is not None

    reference = parse_text(front + "## auto-acknowledge\n\nA listing.\n\n## prose\n\nBody.\n", Path("e.md"))
    assert reference.asks == [], "an opted-out entry asks nothing at all"
    assert reference.acknowledgement is None

    asking = parse_text(ENTRY, Path("e.md"))
    assert [b.name for b in asking.asks] == ["pick-one"], "a round that asks something needs no acknowledgement"

    closed = parse_text(front.replace("state: open", "state: obsolete") + "## prose\n\nGone.\n", Path("e.md"))
    assert closed.acknowledgement is None, "an entry that is no longer open is not waiting to be read"


def test_append_refuses_to_write_what_would_not_parse(root: Path) -> None:
    """The addition is validated against the merged result, not on its own.

    `append` writes to a file the server is reading, so a malformed block has to fail before the write
    rather than leave an entry the page cannot render.
    """
    entry = parse_text(ENTRY, Path("entry.md"))

    good = compose_append(entry, "## prose\n\nA second point.\n")
    merged = parse_text(good, Path("entry.md"))
    assert len(merged.blocks) == len(entry.blocks) + 1
    assert [b.name for b in merged.asks] == [b.name for b in entry.asks], "appending prose adds no ask"

    bad = compose_append(entry, "## not-a-block\n\nx\n")
    try:
        parse_text(bad, Path("entry.md"))
    except ReviewParseError as e:
        assert "not-a-block" in str(e), e
    else:
        raise AssertionError("an unknown block type must not parse")


def test_append_keeps_an_existing_answer_addressable(root: Path) -> None:
    """A follow-up appended beside a finalized ask must not disturb the ask that was already answered."""
    entry = parse_text(ENTRY, Path("entry.md"))
    original = [b.name for b in entry.asks]

    merged = parse_text(
        compose_append(entry, "## ask  the-followup\nfollows: pick-one\n\nAnd now?\n\n- radio: yes\n"),
        Path("entry.md"),
    )

    assert [b.name for b in merged.asks] == original + ["the-followup"]
    assert merged.ask("pick-one") is not None, "the answered ask is still there under its own name"
    assert merged.ask("the-followup").attrs["follows"] == "pick-one"


def test_the_last_artifact_block_is_the_one_that_publishes(root: Path) -> None:
    """A redraft appends rather than rewrites, so the newest draft is what `post` would send.

    The earlier ones stay as the record of what was shown and turned down, and must not end up concatenated into the comment.
    """
    text = (
        "---\nid: 985\ntitle: draft\ngroup: finalize\nstate: open\n---\n\n"
        "## artifact\nround: 1\n\nThe first draft.\n\n"
        "## artifact\nround: 2\n\nThe redraft.\n"
    )
    entry = parse_text(text, Path("985-draft.md"))
    blocks = [b for b in entry.blocks if b.type == "artifact"]

    assert len(blocks) == 2
    assert blocks[-1].prose.strip() == "The redraft.", "the last block wins"
    assert "first draft" not in blocks[-1].prose


def test_a_change_only_an_orientation_entry_claims_is_reported(root: Path) -> None:
    """Accounted for and not read is invisible in both coverage gates, so it gets its own line.

    An orientation ask legitimately discharges what it names — it is answering "is this the right change to review" —
    and says nothing about the contents.
    Written after a 35-line file went through a whole review exactly that way.
    """

    def entry_of(slug: str, group: str, ids: list[str]):
        head = "---\nid: 010\ntitle: t\ngroup: " + group + "\nstate: open\n---\n\n"
        body = "## ask  a\ndischarges: " + " ".join(ids) + "\n\nWell?\n\n- radio: yes\n"
        return parse_text(head + body, Path(slug + ".md"))

    orientation = entry_of("010-orientation", "meta", ["CHANGE-AAA", "CHANGE-BBB"])
    finding = entry_of("040-a-finding", "correctness", ["CHANGE-BBB"])

    thin = thinly_discharged([orientation, finding])

    assert list(thin) == ["CHANGE-AAA"], thin
    assert thin["CHANGE-AAA"] == ["010-orientation"]
    assert "CHANGE-BBB" not in thin, "a change some finding engaged with is not thin, whatever else also claims it"


def test_material_added_after_an_answered_ask_needs_its_own_acknowledgement(root: Path) -> None:
    """An entry can gain material in a later round without gaining a question.

    A redrafted artifact, a correction, a note.
    One acknowledgement per entry would already be answered from the round that asked something,
    so the new material would arrive silently under a green tick.
    """
    front = "---\nid: 985\ntitle: t\ngroup: docs\nstate: open\n---\n\n"

    asked = parse_text(front + "## prose\nround: 3\n\nBody.\n\n## ask  decide\nround: 3\n\nWell?\n\n- radio: yes\n", Path("e.md"))
    assert [b.name for b in asked.asks] == ["decide"], "the round that asked something needs no acknowledgement"

    redrafted = parse_text(
        front
        + "## prose\nround: 3\n\nBody.\n\n## ask  decide\nround: 3\n\nWell?\n\n- radio: yes\n\n"
        + "## prose\nround: 6\n\nA redraft, asking nothing.\n",
        Path("e.md"),
    )

    names = [b.name for b in redrafted.asks]
    assert names == ["decide", ack_name(6)], names
    assert redrafted.acknowledgement is not None
    assert "gained material" in redrafted.acknowledgement.prose

    # The earlier round's answer is not an orphan when the later acknowledgement supersedes it.
    with tempfile.TemporaryDirectory(prefix="review-ack-") as answer_dir:
        answers = AnswerFile.load(Path(answer_dir) / "985.json", "985")
        answers.upsert(asked.acknowledgement or asked.ask("decide"), selected=["yes"], text="", round_number=3)
        answers.answers[ack_name(3)] = answers.answers.pop("decide")
        moved = answers.reconcile(redrafted)
        assert moved == [], "a superseded acknowledgement is the mechanism working, not a vanished ask"


def test_answers_orphan_a_vanished_ask(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    with tempfile.TemporaryDirectory(prefix="review-answers-") as answer_dir:
        answers = AnswerFile.load(Path(answer_dir) / "x.json", "x")
        answers.upsert(entry.ask("pick-one"), selected=["the first way"], text="", round_number=1)

        without = parse_text(ENTRY[:ENTRY.index("## ask")], Path("entry.md"))
        moved = answers.reconcile(without)
        assert moved == ["pick-one"]
        assert not answers.answers and len(answers.orphans) == 1


SUPERSEDE = """
## prose
round: 1

The first telling.

## prose
round: 2
supersedes: prose

The ==corrected== telling.
"""


def _index_of(root: Path, paths: list[str]) -> RepoIndex:
    """An index over exactly these paths, without needing a repository behind them."""
    return RepoIndex({p: root for p in paths})


REPO_FILES = [
    "tools/review/lib/render/markdown.py",
    "tools/review/lib/entry/parse.py",
    "libs/base/clean-core/src/clean-core/container/vector.hh",
    "libs/base/clean-core/docs/TODO.md",
    "tools/review/TODO.md",
]


def _tokens(root: Path, body: str, paths: list[str] = REPO_FILES):
    entry = parse_text(ENTRY + "\n## prose\n\n" + body + "\n", Path("entry.md"))
    return build_tokens(entry, _index_of(root, paths))


def test_a_reference_resolves_three_ways(root: Path) -> None:
    """The exact path, a unique suffix, and a bare basename — the last two are how people actually write one."""
    for written in ("tools/review/lib/render/markdown.py", "lib/render/markdown.py", "markdown.py"):
        tokens = _tokens(root, f"See `{written}` for it.")
        assert len(tokens) == 1, (written, tokens)
        assert tokens[0].path == "tools/review/lib/render/markdown.py", (written, tokens[0])
        assert not tokens[0].problem


def test_an_ambiguous_reference_is_a_problem(root: Path) -> None:
    """Always fixable by writing a longer path, so failing on it costs nothing and buys a guarantee."""
    tokens = _tokens(root, "See `TODO.md`.")
    assert tokens[0].problem and "names 2 files" in tokens[0].problem, tokens[0].problem


def test_prose_that_merely_holds_a_dot_is_not_a_reference(root: Path) -> None:
    """`git.has_merges` is prose about code.

    Only the repository can say which dotted words are file names.
    """
    assert not _tokens(root, "`git.has_merges` already knows, and `sr::window.headless` does too.")
    assert not _tokens(root, "The route is `/favicon.ico`, and the scheme is `vscode://file/x`.")


def test_bare_prose_is_not_scanned(root: Path) -> None:
    """The repo's convention backticks a path; scanning running text makes every sentence a candidate."""
    assert not _tokens(root, "The file markdown.py is where it lives.")
    assert _tokens(root, "The file `markdown.py` is where it lives.")


def test_a_fenced_block_is_scanned(root: Path) -> None:
    """A path in a code comment is exactly the case the round asked for."""
    tokens = _tokens(root, "```cpp\n// see libs/base/clean-core/src/clean-core/container/vector.hh\nint x;\n```")
    assert len(tokens) == 1 and tokens[0].path.endswith("vector.hh"), tokens


def test_new_and_old_say_what_a_path_asserts(root: Path) -> None:
    """A single 'might not exist' marker would let a stale `new:` sit forever, which is what the strictness is for."""
    fresh = _tokens(root, "It becomes `new:tools/review/lib/refs/index.py`.")[0]
    assert fresh.css == "ref-new" and not fresh.problem
    assert fresh.label == "tools/review/lib/refs/index.py", "the prefix is stripped on render"

    stale = _tokens(root, "It becomes `new:markdown.py`.")[0]
    assert "already exists" in stale.problem, stale.problem

    gone = _tokens(root, "It removes `old:tools/review/lib/render/linkify.py`.")[0]
    assert gone.css == "ref-old" and not gone.problem


def test_a_missing_path_is_a_problem(root: Path) -> None:
    tokens = _tokens(root, "See `tools/review/lib/nope.py`.")
    assert "not a file in this repository" in tokens[0].problem


def test_a_line_reference_carries_its_line(root: Path) -> None:
    token = _tokens(root, "See `markdown.py:63`.")[0]
    assert token.line == 63 and token.href.endswith("#L63"), token


GLOSSARY = """
## prose
glossary: true

**atom** — one unit of change the review must account for.

**line space** (spaces) — the net set of added and removed lines.
"""


def test_a_glossary_block_declares_its_terms(root: Path) -> None:
    """Marked rather than scraped, so a paragraph that is not a term can be reported instead of dropped."""
    entry = parse_text(ENTRY + GLOSSARY, Path("018-glossary.md"), slug="018-glossary")
    terms = terms_in(entry)
    assert [t.term for t in terms] == ["atom", "line space"], terms
    assert terms[1].aliases == ("spaces",)
    assert not malformed_in(entry)


def test_a_paragraph_that_is_not_a_term_is_reported(root: Path) -> None:
    """A term nobody finds out is missing is exactly what marking the block is for."""
    text = ENTRY + "\n## prose\nglossary: true\n\n**atom**: the wrong dash entirely.\n"
    problems = malformed_in(parse_text(text, Path("018-glossary.md"), slug="018-glossary"))
    assert len(problems) == 1 and "not `**term**" in problems[0], problems


def test_a_term_is_matched_in_whatever_form_the_text_used(root: Path) -> None:
    """Whole-word, case-insensitive, longest first, with naive plurals both ways."""
    entry = parse_text(ENTRY + GLOSSARY, Path("018-glossary.md"), slug="018-glossary")
    provider = GlossaryProvider(terms=terms_in(entry))
    found = {t.text for t in provider.tokens("Every Atom in the line spaces, but not atomic or spacer.")}
    assert "Atom" in found and "line spaces" in found, found
    assert not any(f in ("atomic", "spacer") for f in found), found


def test_the_glossary_entry_does_not_annotate_itself(root: Path) -> None:
    """Underlining a definition inside its own definition says nothing."""
    entry = parse_text(ENTRY + GLOSSARY, Path("018-glossary.md"), slug="018-glossary")
    provider = GlossaryProvider(terms=terms_in(entry))
    assert not provider.tokens("an atom here", skip_entry="018-glossary")


def test_the_tree_folds_single_child_chains(root: Path) -> None:
    """`a/b` on one line with `c` and `d` under it, and `e` back at the root — a location, not a histogram."""
    html = tree_html(["a/b/c.txt", "a/b/d.txt", "e.txt"], {"a/b/c.txt": (3, 1)})
    rows = re.findall(r'<div class="tree-(dir|file)">((?:(?!</div>).)*)', html)
    shown = [(kind, re.sub("<[^>]+>", "", body).strip()) for kind, body in rows]
    assert shown[0] == ("dir", "a/b/"), shown
    assert [s for k, s in shown if k == "file"] == ["c.txt+3-1", "d.txt+0-0", "e.txt+0-0"], shown


def test_the_tree_says_what_it_dropped(root: Path) -> None:
    """A silent truncation reads as 'that is the whole change' when it is not."""
    many = [f"pkg/dir{n // 20}/file{n}.txt" for n in range(400)]
    html = tree_html(many, {})
    assert "more files under" in html, "a capped tree has to name what it left out"


def test_a_commit_sha_is_confirmed_against_git(root: Path) -> None:
    """Asking git rather than writing a better regex is what drops a blob id out of a lockfile diff."""
    asked = []

    def confirm(candidates):
        asked.append(list(candidates))
        return {"cefb3b9a"}

    provider = CommitProvider(confirm=confirm)
    tokens = provider.tokens("landed in cefb3b9a, unlike deadbeef, and short ab12 is a word")
    assert [t.text for t in tokens] == ["cefb3b9a"], tokens
    assert "ab12" not in asked[0], "fewer than seven hex characters is a word, not a candidate"
    assert "deadbeef" in asked[0], "git decides, not the length"


def test_a_forge_url_is_derived_or_absent(root: Path) -> None:
    """A repository with no forge is a valid thing to review, so this degrades to no link."""
    assert forge_url("git@github.com:solidean/shaped-core.git", "abc") == \
        "https://github.com/solidean/shaped-core/commit/abc"
    assert forge_url("https://github.com/solidean/shaped-core.git", "abc") == \
        "https://github.com/solidean/shaped-core/commit/abc"
    assert forge_url("", "abc") == ""
    assert forge_url("/srv/git/bare.git", "abc") == ""


def test_a_superseded_block_leaves_the_live_ones(root: Path) -> None:
    """Rounds stay immutable and the file stays append-only, so a correction is a new block rather than an edit."""
    entry = parse_text(ENTRY + SUPERSEDE, Path("entry.md"))
    prose = [b for b in entry.blocks if b.type == "prose"]
    assert prose[0].is_superseded and prose[0].superseded_by == "r2/prose"
    assert not prose[1].is_superseded
    assert prose[0] not in entry.live_blocks and prose[1] in entry.live_blocks


def test_supersedes_must_name_an_earlier_block_in_this_entry(root: Path) -> None:
    """Within one entry is what keeps the struck original and its replacement on the same screen."""
    try:
        parse_text(ENTRY + "\n## prose\nsupersedes: nothing-like-this\n\nA correction.\n", Path("entry.md"))
    except ReviewParseError as e:
        assert "no earlier block" in str(e)
        return
    raise AssertionError("a supersedes naming nothing must be an error, not a silent no-op")


def test_a_superseded_ask_discharges_nothing(root: Path) -> None:
    """Otherwise a replaced ask would double-count against the coverage gate."""
    text = ENTRY + "\n## ask  pick-again\nsupersedes: pick-one\ndischarges: CHANGE-AAAA\n\nWhich way now?\n\n- radio: yes\n"
    entry = parse_text(text, Path("entry.md"))
    assert [b.name for b in entry.asks] == ["pick-again"]
    assert entry.discharged_changes() == ["CHANGE-AAAA"], "the replacement discharges; the original no longer does"


def test_an_answered_ask_cannot_be_superseded(root: Path) -> None:
    """Otherwise the answer sits under a question the maintainer never saw, which is what immutability prevents."""
    text = ENTRY + "\n## ask  pick-again\nsupersedes: pick-one\n\nReworded.\n\n- radio: yes\n"
    entry = parse_text(text, Path("entry.md"))
    try:
        check_supersedes(entry, {"pick-one"})
    except ReviewParseError as e:
        assert "follows: pick-one" in str(e)
        return
    raise AssertionError("superseding an answered ask must be refused, naming `follows:` as the remedy")


def test_a_new_span_is_marked_and_a_code_span_is_not(root: Path) -> None:
    """The rephrase case is why this exists: three words changed, unreadable as a diff."""
    html = render_markdown("The ==corrected== telling, not `a == b`.")
    assert '<mark class="new">corrected</mark>' in html, html
    assert "<mark" not in html.split("<code>")[1], "a code span holding == must be left alone"


def test_a_comment_is_tentative_until_the_round_is_finalized(root: Path) -> None:
    """Nothing reaches the agent until a round is sent, so a remark can still be deleted right up to that point."""
    entry = parse_text(ENTRY, Path("entry.md"))
    answers = AnswerFile.load(Path(root) / "x.json", "040-a")

    block = entry.blocks[0]
    made = answers.comment(comment_id="", text="why this way?", block=block.anchor,
                           change="", offset=-1, round_number=1)
    assert made.id == "c1"
    assert not answers.comments_since(0), "a tentative comment is not part of a round"

    answers.finalize(1)
    assert [c.id for c in answers.comments_since(0)] == ["c1"]
    assert not answers.comments_since(1), "the watermark excludes the round it names"


def test_a_finalized_comment_cannot_be_edited(root: Path) -> None:
    """The round that quoted it has to keep reading correctly, exactly as a finalized answer does."""
    answers = AnswerFile.load(Path(root) / "x.json", "040-a")
    answers.comment(comment_id="", text="the original", block="r1/prose", change="", offset=-1, round_number=1)
    answers.finalize(1)
    answers.comment(comment_id="c1", text="a rewrite", block="r1/prose", change="", offset=-1, round_number=2)
    assert answers.comments["c1"].text == "the original"


def test_deleting_a_comment_is_saving_it_empty(root: Path) -> None:
    answers = AnswerFile.load(Path(root) / "x.json", "040-a")
    answers.comment(comment_id="", text="never mind", block="r1/prose", change="", offset=-1, round_number=1)
    assert answers.comment(comment_id="c1", text="  ", block="r1/prose", change="", offset=-1, round_number=1) is None
    assert not answers.comments
    # Ids stay monotonic, so a later comment never reuses the deleted one's anchor.
    answers.comment(comment_id="", text="a second thought", block="r1/prose", change="", offset=-1, round_number=1)
    assert list(answers.comments) == ["c1"]


def test_a_comment_is_addressed_by_reference(root: Path) -> None:
    """Outstanding is computed from `addresses:`, the way an undischarged change is, rather than tracked."""
    entry = parse_text(ENTRY, Path("entry.md"))
    assert not entry.addressed_comments()

    answered = parse_text(ENTRY + "\n## prose\naddresses: c1 c2\n\nBoth noted; no change to the first.\n",
                          Path("entry.md"))
    assert answered.addressed_comments() == {"c1", "c2"}


def test_a_comment_survives_a_round_trip_through_the_answers_file(root: Path) -> None:
    path = Path(root) / "x.json"
    answers = AnswerFile.load(path, "040-a")
    answers.comment(comment_id="", text="on this hunk", block="", change="CHANGE-AAAA", offset=3, round_number=2)
    answers.save()

    reloaded = AnswerFile.load(path, "040-a")
    comment = reloaded.comments["c1"]
    assert comment.is_line and comment.change == "CHANGE-AAAA" and comment.offset == 3
    assert "CHANGE-AAAA" in comment.where() and "line 4" in comment.where()


def test_since_reports_only_finalized_answers(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    with tempfile.TemporaryDirectory(prefix="review-answers-") as answer_dir:
        answers = AnswerFile.load(Path(answer_dir) / "x.json", "x")
        answers.upsert(entry.ask("pick-one"), selected=["the first way"], text="", round_number=1)
        assert not answers.since(0), "a tentative answer is not part of a round"
        answers.finalize(1)
        assert [a.name for a in answers.since(0)] == ["pick-one"]
        assert not answers.since(1), "the watermark excludes the round it names"


# ---- harness ----------------------------------------------------------------


def main() -> int:
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    failed = 0
    for test in tests:
        with tempfile.TemporaryDirectory(prefix="review-self-test-") as workdir:
            try:
                test(Path(workdir))
            except Exception as e:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first.
                failed += 1
                print(f"FAIL {test.__name__}: {type(e).__name__}: {e}")
            else:
                if VERBOSE:
                    print(f"ok   {test.__name__}")

    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
