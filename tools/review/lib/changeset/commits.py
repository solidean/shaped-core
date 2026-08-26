"""Ingesting hunks as an individual commit wrote them.

The net diff is the right default, but it blends concerns: two commits touching the same function collapse into one hunk,
and a review of that hunk cannot say which half it means.
Taking the hunks from a single commit instead keeps the two apart.

What the commit claims is not what it wrote, though.
Its added lines are carried forward to head and its removed lines back to base, and both are then intersected with net space,
so a claim can never escape the obligation the review actually has.
"""

from __future__ import annotations

from ..git.diffparse import parse
from ..git.run import EMPTY_TREE, Git
from ..space import linemap
from ..space.netspace import ADDED, REMOVED, LineSpace
from .ids import digest_of
from .ingest import Candidate, group_hunks


class MergeInRange(Exception):
    """A merge on the path being ingested, which line mapping cannot follow."""


def _parent_of(git: Git, sha: str) -> str:
    parent = git.rev_parse(sha + "^")
    return parent or EMPTY_TREE


def candidates_for_commit(
    git: Git, sha: str, *, base: str, head: str, context: int, gap: int,
    net: LineSpace, paths: list[str] | None = None,
) -> list[Candidate]:
    """Every change this one commit's hunks would create, claimed in net-diff line space."""
    parent = _parent_of(git, sha)
    forward = linemap.build(git, sha, head)
    backward = linemap.build(git, base, parent)

    display = parse(git.diff(parent, sha, context=context, paths=paths))
    short = sha[:8]
    out: list[Candidate] = []

    for file in display:
        commit_new_path = file.new_path or file.path
        commit_old_path = file.old_path or file.path

        for group in group_hunks(file.hunks, gap):
            added: list[int] = []
            removed: list[int] = []
            for hunk in group:
                hunk_added, hunk_removed = hunk.sides()
                added.extend(hunk_added)
                removed.extend(hunk_removed)

            claim = LineSpace.empty()
            if added:
                head_path, survived = linemap.map_forward(forward, commit_new_path, added)
                claim.add(ADDED, head_path, survived.intersect(net.get(ADDED, head_path)))
            if removed:
                base_path = linemap.source_path(backward, commit_old_path)
                reached = linemap.map_backward(backward, commit_old_path, removed)
                claim.add(REMOVED, base_path, reached.intersect(net.get(REMOVED, base_path)))

            if claim.is_empty:
                continue

            body = "\n".join(hunk.render() for hunk in group)
            claimed_added = sum(len(v) for (side, _), v in claim.lines.items() if side == ADDED)
            claimed_removed = sum(len(v) for (side, _), v in claim.lines.items() if side == REMOVED)
            out.append(Candidate(
                kind="commit-hunk",
                path=file.path,
                digest=digest_of(short, file.path, "\n".join(h.hash_body() for h in group)),
                summary=(f"{file.path}:{group[0].new_start}-{group[-1].new_end} "
                         f"(+{claimed_added}/-{claimed_removed}) in {short}"),
                claim=claim,
                body=body,
                provenance=sha,
            ))

    return out


def commit_atoms(
    git: Git, sha: str, *, base: str, head: str, net: LineSpace, paths: list[str] | None = None,
) -> LineSpace:
    """Every atom this commit contributed that still stands at head, without reading a single hunk body.

    The same carrying as `candidates_for_commit`, but over the commit's whole diff at once.
    This is what lets a mechanical commit be accepted wholesale: the claim is exact, and no diff reaches anyone's context.
    """
    parent = _parent_of(git, sha)
    forward = linemap.build(git, sha, head)
    backward = linemap.build(git, base, parent)

    claimed = LineSpace.empty()
    for file in parse(git.diff(parent, sha, context=0, paths=paths)):
        commit_new_path = file.new_path or file.path
        commit_old_path = file.old_path or file.path

        added: list[int] = []
        removed: list[int] = []
        for hunk in file.hunks:
            hunk_added, hunk_removed = hunk.sides()
            added.extend(hunk_added)
            removed.extend(hunk_removed)

        if added:
            head_path, survived = linemap.map_forward(forward, commit_new_path, added)
            claimed.add(ADDED, head_path, survived.intersect(net.get(ADDED, head_path)))
        if removed:
            base_path = linemap.source_path(backward, commit_old_path)
            reached = linemap.map_backward(backward, commit_old_path, removed)
            claimed.add(REMOVED, base_path, reached.intersect(net.get(REMOVED, base_path)))

        # A file-level change is contributed whole, so it needs no mapping — only a check that head still carries it.
        for kind, _ in file.file_atoms():
            match = next((a for a in net.files if a.kind == kind and a.path in (commit_new_path, commit_old_path)), None)
            if match is not None:
                claimed.files = claimed.files | {match}

    return claimed


def bulk_candidate_for_commits(
    git: Git, shas: list[str], *, base: str, head: str, net: LineSpace,
    reason: str, label: str, matches=None, paths: list[str] | None = None,
) -> Candidate | None:
    """One change covering everything these commits contributed, with no hunk bodies on disk.

    `matches` narrows it further by path, so "the formatting sweep, but only under libs/" is one claim rather than two.
    """
    claimed = LineSpace.empty()
    for sha in shas:
        claimed = claimed.union(commit_atoms(git, sha, base=base, head=head, net=net, paths=paths))

    if matches is not None:
        narrowed = LineSpace.empty()
        for (side, path), spans in claimed.lines.items():
            if matches(path):
                narrowed.add(side, path, spans)
        narrowed.files = frozenset(a for a in claimed.files if matches(a.path))
        claimed = narrowed

    if claimed.is_empty:
        return None

    lines = sum(len(v) for v in claimed.lines.values())
    files = len({p for _, p in claimed.lines} | {a.path for a in claimed.files})
    return Candidate(
        kind="bulk", path=label,
        digest=digest_of("bulk-commits", "\x1f".join(sorted(shas)), label, reason),
        summary=f"{label} — {len(shas)} commit(s), {files} file(s), {lines} line(s), accepted wholesale",
        claim=claimed, provenance="bulk", reason=reason,
    )


def shrinkage(git: Git, sha: str, candidates: list[Candidate], paths: list[str] | None = None) -> tuple[int, int]:
    """(lines the commit wrote, lines of those that still reach net space).

    Reporting this is what keeps the mapping from being a black box:
    a commit whose work was rewritten later claims less than its diff suggests, and the number says so.
    """
    parent = _parent_of(git, sha)
    written = 0
    # A GitError here is not survivable: swallowing it reported zero lines written, and `collect` then dropped
    # every candidate this commit produced without saying so.
    for file in parse(git.diff(parent, sha, context=0, paths=paths)):
        for hunk in file.hunks:
            hunk_added, hunk_removed = hunk.sides()
            written += len(hunk_added) + len(hunk_removed)
    claimed = sum(len(c.claim) for c in candidates)
    return written, claimed


def collect(
    git: Git, shas: list[str], *, base: str, head: str, context: int, gap: int,
    net: LineSpace, paths: list[str] | None = None,
) -> tuple[list[Candidate], list[str]]:
    """Candidates for several commits, plus a per-commit note on how much of each still reaches net space."""
    out: list[Candidate] = []
    notes: list[str] = []
    for sha in shas:
        found = candidates_for_commit(
            git, sha, base=base, head=head, context=context, gap=gap, net=net, paths=paths,
        )
        written, claimed = shrinkage(git, sha, found, paths)
        killed = max(written - claimed, 0)
        if not written:
            continue
        notes.append(
            f"{sha[:8]}: {written} lines written, {claimed} still reach head"
            + (f", {killed} rewritten by a later commit" if killed else "")
        )
        out.extend(found)
    return out, notes
