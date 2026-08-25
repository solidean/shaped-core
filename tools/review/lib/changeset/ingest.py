"""Giving every change an identity.

Two diffs run, and the split between them is the whole design.
A `--unified=0` diff is the **authority** on what changed; a `--unified=<context>` diff is what a human **reads**.
A display hunk's claim is therefore its span intersected with net space, which drops its context lines automatically
and makes the default ingest cover the whole obligation by construction — there is no arithmetic to get wrong.

Coalescing only has an effect past twice the context width, because git already merges hunks whose context windows touch.
The default gap is set above that, so the knob does something rather than reading as a setting that works.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from ..git.diffparse import FileDiff, Hunk, parse
from ..git.run import Git
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


@dataclass
class IngestResult:
    """What an ingest did, or would do under `--dry-run`."""

    created: list[Change] = field(default_factory=list)
    reused: list[Change] = field(default_factory=list)
    candidates: list[Candidate] = field(default_factory=list)
    skipped_covered: int = 0

    @property
    def total(self) -> int:
        return len(self.created) + len(self.reused)


def _group_hunks(hunks: list[Hunk], gap: int) -> list[list[Hunk]]:
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
        for group in _group_hunks(file.hunks, gap):
            claim = _claim_of(group, file, net)
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


def register(
    ledger: Ledger, candidates: list[Candidate], *, round_number: int,
    write_body, only_uncovered: bool = False,
) -> IngestResult:
    """Turn candidates into ledger rows, reusing the id any already-known digest had.

    `write_body` takes (change_id, text) and is what puts a hunk on disk; a bulk or file change has none.
    """
    result = IngestResult()
    known = ledger.by_digest()
    covered = ledger.covered() if only_uncovered else LineSpace.empty()

    fresh: list[Candidate] = []
    for candidate in candidates:
        existing_id = known.get(candidate.digest)
        if existing_id and (change := ledger.get(existing_id)) is not None:
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
