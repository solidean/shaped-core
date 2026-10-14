"""Giving every change an identity.

Two diffs run, and the split between them is the whole design.
A `--unified=0` diff is the **authority** on what changed; a `--unified=<context>` diff is what a human **reads**.
A display hunk's claim is therefore its span intersected with net space, which drops its context lines automatically
and makes the default ingest cover the whole obligation by construction — there is no arithmetic to get wrong.

Coalescing only has an effect past twice the context width, because git already merges hunks whose context windows touch.
The default gap is set above that, so the knob does something rather than reading as a setting that works.
"""

from __future__ import annotations

import difflib
from dataclasses import dataclass, field

from ..core.paths import ReviewPaths
from ..git.diffparse import FileDiff, Hunk, parse
from ..git.run import Git
from ..space import linemap
from ..space.intervals import IntervalList
from ..space.netspace import ADDED, REMOVED, FileAtom, LineSpace, space_of
from .ids import allocate_many, digest_of
from .ledger import Change, Ledger


@dataclass
class Candidate:
    """A change the ingest would create, before it is given an id."""

    kind: str
    path: str
    digest: str
    summary: str
    claim: LineSpace
    body: str = ""
    provenance: str = "net"
    reason: str = ""
    supersedes: list[str] = field(default_factory=list)


@dataclass
class IngestResult:
    """What an ingest did, or would do under `--dry-run`."""

    created: list[Change] = field(default_factory=list)
    reused: list[Change] = field(default_factory=list)
    repointed: list[Change] = field(default_factory=list)
    candidates: list[Candidate] = field(default_factory=list)
    skipped_covered: int = 0

    @property
    def total(self) -> int:
        return len(self.created) + len(self.reused)


def group_hunks(hunks: list[Hunk], gap: int) -> list[list[Hunk]]:
    """Group a file's hunks into the units a reader should see as one change."""
    groups: list[list[Hunk]] = []
    for hunk in hunks:
        if groups and hunk.new_start - groups[-1][-1].new_end - 1 <= gap:
            groups[-1].append(hunk)
        else:
            groups.append([hunk])
    return groups


def _claim_of(group: list[Hunk], file: FileDiff, net: LineSpace) -> LineSpace:
    """The atoms a display group accounts for: its span, narrowed to what actually changed."""
    new_path = file.new_path or file.path
    old_path = file.old_path or file.path
    new_span = IntervalList.of((h.new_start, h.new_end) for h in group)
    old_span = IntervalList.of((h.old_start, h.old_end) for h in group)

    claim = LineSpace.empty()
    claim.add(ADDED, new_path, new_span.intersect(net.get(ADDED, new_path)))
    claim.add(REMOVED, old_path, old_span.intersect(net.get(REMOVED, old_path)))
    return claim


def _adopt_residue(groups: list[list[Hunk]], claims: list[LineSpace], file: FileDiff, net: LineSpace) -> None:
    """Give every atom of this file that no group's span reached to the group nearest it, in place.

    The two diffs do not always align a change the same way.
    Where the added text repeats text nearby, git's choice of which copy is "new" depends on the context width,
    so an atom the `--unified=0` diff reports can sit on a line the display diff calls unchanged and shows no hunk for.
    Without this the default sweep leaves it unaccounted, and `--rest` cannot help, since it draws from the same hunks.
    """
    if not groups:
        return
    new_path = file.new_path or file.path
    old_path = file.old_path or file.path

    for side, path, start_of, end_of in (
        (ADDED, new_path, lambda h: h.new_start, lambda h: h.new_end),
        (REMOVED, old_path, lambda h: h.old_start, lambda h: h.old_end),
    ):
        reached = IntervalList.of([])
        for claim in claims:
            reached = reached.union(claim.get(side, path))
        residue = net.get(side, path).subtract(reached)
        for start, end in residue:
            def distance(group: list[Hunk]) -> int:
                first, last = start_of(group[0]), end_of(group[-1])
                return max(first - end, start - last, 0)

            nearest = min(range(len(groups)), key=lambda i: distance(groups[i]))
            claims[nearest].add(side, path, IntervalList.span(start, end))


def _summary(path: str, group: list[Hunk], claim: LineSpace) -> str:
    added = sum(len(v) for (side, _), v in claim.lines.items() if side == ADDED)
    removed = sum(len(v) for (side, _), v in claim.lines.items() if side == REMOVED)
    first, last = group[0].new_start, group[-1].new_end
    return f"{path}:{first}-{last} (+{added}/-{removed})"


def candidates_for(
    git: Git, base: str, head: str, *, context: int, gap: int,
    net: LineSpace, paths: list[str] | None = None,
) -> list[Candidate]:
    """Every change the default net-diff ingest would create over `paths` (or the whole range)."""
    display = parse(git.diff(base, head, context=context, paths=paths))
    out: list[Candidate] = []

    for file in display:
        path = file.path
        groups = group_hunks(file.hunks, gap)
        claims = [_claim_of(group, file, net) for group in groups]
        _adopt_residue(groups, claims, file, net)
        for group, claim in zip(groups, claims):
            if claim.is_empty:
                continue
            body = "\n".join(h.render() for h in group)
            digest = digest_of(path, "\n".join(h.hash_body() for h in group))
            out.append(Candidate(
                kind="hunk", path=path, digest=digest,
                summary=_summary(path, group, claim), claim=claim, body=body,
            ))

        for kind, discriminant in file.file_atoms():
            atom = FileAtom(path, kind, discriminant)
            if atom not in net.files:
                continue
            claim = LineSpace({}, frozenset({atom}))
            out.append(Candidate(
                kind="file", path=path, digest=digest_of(path, kind, discriminant),
                summary=atom.describe(), claim=claim,
            ))

    return out


def superseded_by_move(ledger: Ledger, candidates: list[Candidate], net: LineSpace) -> list[Change]:
    """The live changes a head move retires.

    A net hunk is retired as soon as no candidate carries its digest, even where its claim still meets net space.
    That is a hunk edited in place, which left live would claim its atoms a second time beside its successor.
    Every other kind also needs its claim to have left net space.
    No candidate ever carries a bulk claim or a commit-local hunk, since neither is cut from the net diff.
    Retired on the digest alone, every one of them would go on every sync.

    A change whose digest a candidate still carries is the same hunk at new line numbers, which `register` re-points.
    Superseding it instead would be permanent, since `register` leaves a superseded change alone and no ingest creates
    a second change for a known digest, so its atoms would stay unaccounted for good.
    """
    surviving = {c.digest for c in candidates}
    return [
        c for c in ledger.live()
        if c.digest not in surviving and (c.kind == "hunk" or c.claim.intersect(net).is_empty)
    ]


def returned_by_move(ledger: Ledger, candidates: list[Candidate]) -> list[Change]:
    """The superseded changes whose content a head move brought back, as reverting an edit does.

    Left superseded, such a change's atoms stay unaccounted for good, for the reason `superseded_by_move` gives.
    """
    known = ledger.by_digest()
    found: dict[str, Change] = {}
    for candidate in candidates:
        change = ledger.get(known.get(candidate.digest, ""))
        if change is not None and change.superseded:
            found[change.id] = change
    return list(found.values())


def _carried(claim: LineSpace, head_map: linemap.TreeMap, base_map: linemap.TreeMap) -> LineSpace:
    """A claim in an earlier range's numbering, in a later one's; a line the later range rewrote is dropped."""
    out = LineSpace.empty()
    for key in claim.lines:
        side = key[0]
        path = key[1]
        tree = head_map if side == ADDED else base_map
        lines = [n for span in claim.lines[key] for n in range(span[0], span[1] + 1)]
        survived = linemap.map_forward(tree, path, lines)[1]
        out.add(side, tree.target_path(path), survived)
    return out


def link_successors(
    candidates: list[Candidate], gone: list[Change], *, head_map: linemap.TreeMap, base_map: linemap.TreeMap,
) -> None:
    """Record on each candidate, in place, the retired hunks it is the edited form of.

    A retired claim is in the old range's numbering, so it is carried to the new one first.
    `head_map` carries its added lines from the old head to the new, `base_map` its removed lines from the old base.
    A candidate meeting what survived of a retired hunk replaces it.
    Compared raw instead, an insertion above a hunk shifts it off its own old numbers and onto some other hunk's.
    """
    carried = {change.id: _carried(change.claim, head_map, base_map) for change in gone if change.kind == "hunk"}
    for candidate in candidates:
        candidate.supersedes = [
            change_id for change_id in carried if not carried[change_id].intersect(candidate.claim).is_empty
        ]


def interdiff(old: str, new: str) -> str:
    """The unified diff from one hunk body to another, empty when they are the same.

    Each line carries two markers, as `git range-diff` prints them.
    The outer says how the body changed, the inner what the hunk itself does.
    """
    lines = list(difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm=""))
    # The `---` / `+++` pair names no file here, and a diff view would draw it as one removed and one added line.
    return "\n".join(lines[2:])


@dataclass(frozen=True)
class Interdiff:
    """What a change altered against one hunk it replaced."""

    replaced: str
    text: str


def interdiffs(change: Change, paths: ReviewPaths) -> list[Interdiff]:
    """One interdiff per hunk this change replaced, where both bodies are still on disk."""
    new_file = paths.change_diff(change.id)
    if not change.supersedes or not new_file.is_file():
        return []
    new = new_file.read_text(encoding="utf-8", errors="replace")
    out: list[Interdiff] = []
    for old_id in change.supersedes:
        old_file = paths.change_diff(old_id)
        if old_file.is_file():
            text = interdiff(old_file.read_text(encoding="utf-8", errors="replace"), new)
            if text:
                out.append(Interdiff(replaced=old_id, text=text))
    return out


def register(
    ledger: Ledger, candidates: list[Candidate], *, round_number: int,
    write_body, only_uncovered: bool = False,
) -> IngestResult:
    """Turn candidates into ledger rows, reusing the id any already-known digest had.

    `write_body` takes (change_id, text) and is what puts a hunk on disk; a bulk or file change has none.

    A reused change is also RE-POINTED at the candidate's claim when the two differ.
    An id is derived from content and a claim from position, so a head move leaves a hunk the author never touched
    with the same id at different line numbers — and a claim left in the old coordinates covers atoms that no longer
    exist while the ones that do go unaccounted.
    Coverage then reads red for a review that is complete, and no further ingest can clear it, since the digest is
    already known and this branch is what it lands in.
    """
    result = IngestResult()
    known = ledger.by_digest()
    covered = ledger.covered() if only_uncovered else LineSpace.empty()

    fresh: list[Candidate] = []
    for candidate in candidates:
        existing_id = known.get(candidate.digest)
        if existing_id and (change := ledger.get(existing_id)) is not None:
            # A superseded or absorbed change is left alone: it is a record of something that WAS claimed, and
            # re-pointing it would quietly bring it back to life.
            if not change.superseded and not change.absorbed_by and change.claim != candidate.claim:
                change.claim = candidate.claim
                change.summary = candidate.summary
                if candidate.body:
                    write_body(change.id, candidate.body)
                ledger.append(change)
                result.repointed.append(change)
            result.reused.append(change)
            continue
        if only_uncovered and candidate.claim.subtract(covered).is_empty:
            result.skipped_covered += 1
            continue
        fresh.append(candidate)

    assigned = allocate_many([c.digest for c in fresh], ledger.ids())
    for candidate in fresh:
        change = Change(
            id=assigned[candidate.digest],
            digest=candidate.digest,
            kind=candidate.kind,
            path=candidate.path,
            summary=candidate.summary,
            provenance=candidate.provenance,
            reason=candidate.reason,
            round=round_number,
            has_body=bool(candidate.body),
            claim=candidate.claim,
            supersedes=list(candidate.supersedes),
        )
        if candidate.body:
            write_body(change.id, candidate.body)
        ledger.append(change)
        result.created.append(change)

    return result


def bulk_candidate(net: LineSpace, *, selector: str, reason: str, matches) -> Candidate | None:
    """One change covering everything under a selector, with no hunk body on disk.

    `matches` decides whether a path belongs, so the caller owns the selector language.
    """
    claim = LineSpace.empty()
    for (side, path), spans in net.lines.items():
        if matches(path):
            claim.add(side, path, spans)
    claim.files = frozenset(a for a in net.files if matches(a.path))
    if claim.is_empty:
        return None

    lines = sum(len(v) for v in claim.lines.values())
    files = len({p for _, p in claim.lines} | {a.path for a in claim.files})
    return Candidate(
        kind="bulk", path=selector, digest=digest_of("bulk", selector, reason),
        summary=f"{selector} — {files} file(s), {lines} line(s), accepted wholesale",
        claim=claim, provenance="bulk", reason=reason,
    )
