#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

"""Self-test for the review tool: the interval algebra, change identity, coverage math and the block grammar.

Every test builds a throwaway git repository, so what is checked is real git output rather than a fixture of it.
Kept out of `dev.py test`, which drives the C++ nexus suites; `dev.py check` runs this as its own gate.

Run it directly with `uv run tools/review/review-self-test.py [-v]`.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.review.lib.changeset import commits as commit_ingest  # noqa: E402
from tools.review.lib.changeset.ids import allocate, allocate_many, digest_of  # noqa: E402
from tools.review.lib.changeset.ingest import bulk_candidate, candidates_for, group_hunks, register  # noqa: E402
from tools.review.lib.changeset.ledger import Ledger  # noqa: E402
from tools.review.lib.entry.answers import AnswerFile  # noqa: E402
from tools.review.lib.entry.askhash import hash_ask  # noqa: E402
from tools.review.lib.entry.grammar import ReviewParseError  # noqa: E402
from tools.review.lib.entry.parse import parse_text  # noqa: E402
from tools.review.lib.entry.write import compose, immutability_violations, stamp_rounds  # noqa: E402
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


def test_duplicate_ask_names_are_rejected(root: Path) -> None:
    text = ENTRY + "\n## ask  pick-one\n\nAgain?\n\n- radio: no\n"
    try:
        parse_text(text, Path("entry.md"))
    except ReviewParseError as e:
        assert "duplicate" in str(e)
        return
    raise AssertionError("ask names are the answer key, so duplicates must be rejected")


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


def test_answers_orphan_a_vanished_ask(root: Path) -> None:
    entry = parse_text(ENTRY, Path("entry.md"))
    with tempfile.TemporaryDirectory(prefix="review-answers-") as answer_dir:
        answers = AnswerFile.load(Path(answer_dir) / "x.json", "x")
        answers.upsert(entry.ask("pick-one"), selected=["the first way"], text="", round_number=1)

        without = parse_text(ENTRY[:ENTRY.index("## ask")], Path("entry.md"))
        moved = answers.reconcile(without)
        assert moved == ["pick-one"]
        assert not answers.answers and len(answers.orphans) == 1


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
