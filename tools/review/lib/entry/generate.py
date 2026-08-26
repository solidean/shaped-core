"""The two entries the tool writes for itself, and the rule that keeps them safe to rewrite.

Only the overview and the coverage report are generated, and every other entry is authored end to end:
the judgement about what matters in a change is the most useful thing an entry can carry, and nothing mechanical produces it.

A generated block carries `generated: <key>`, and regeneration replaces exactly those blocks.
Everything else in the file — including prose written directly underneath one — is left untouched,
so refreshing the overview after a `sync` never costs a sentence anyone wrote.
"""

from __future__ import annotations

import re
from pathlib import Path

from ..changeset.ledger import Ledger
from ..core.config import ReviewConfig
from ..git.run import Git
from ..space.netspace import LineSpace
from .parse import Entry, parse_file
from .write import compose, restore_newlines, write_entry

OVERVIEW_SLUG = "015-changes"
COVERAGE_SLUG = "990-coverage"

_MAX_COMMITS = 40
_MAX_DIRS = 25


def _tree(paths: list[str]) -> list[str]:
    """The touched files folded to a directory count, which is the shape of a change at a glance."""
    counts: dict[str, int] = {}
    for path in paths:
        directory = path.rsplit("/", 1)[0] if "/" in path else "."
        counts[directory] = counts.get(directory, 0) + 1
    ordered = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    lines = [f"  {count:>4}  {directory}/" for directory, count in ordered[:_MAX_DIRS]]
    if len(ordered) > _MAX_DIRS:
        lines.append(f"  ...   and {len(ordered) - _MAX_DIRS} more directories")
    return lines


def overview_body(git: Git, cfg: ReviewConfig, net: LineSpace) -> str:
    """The generated half of the overview: the range, its commits, and where the change lands."""
    commits = git.commits(cfg.base, cfg.head)
    paths = net.paths()
    added = sum(len(v) for (side, _), v in net.lines.items() if side == "+")
    removed = sum(len(v) for (side, _), v in net.lines.items() if side == "-")

    lines = [
        "## prose",
        "generated: overview",
        "",
        f"**{cfg.base_spec}..{cfg.head_spec}** — `{cfg.base[:12]}..{cfg.head[:12]}`",
        "",
        f"{len(commits)} commits, {len(paths)} files, +{added} / -{removed} lines"
        + (f", {len(net.files)} file-level changes" if net.files else ""),
        "",
        "### Commits",
        "",
    ]
    for commit in commits[:_MAX_COMMITS]:
        lines.append(f"- `{commit.short}` {commit.subject}  <sub>{commit.author}, {commit.date}</sub>")
    if len(commits) > _MAX_COMMITS:
        lines.append(f"- ... and {len(commits) - _MAX_COMMITS} more")

    lines += ["", "### Where it lands", "", "```"]
    lines += _tree(paths)
    lines += ["```"]
    return "\n".join(lines)


COVERAGE_TITLE = "Coverage & Finalize"
OVERVIEW_TITLE = "Changes"


def coverage_body(cfg: ReviewConfig, net: LineSpace, ledger: Ledger, discharged: set[str]) -> str:
    """The generated coverage report: identity first, then discharge."""
    uncovered = net.subtract(ledger.covered())
    live = ledger.live()
    bulk = [c for c in live if c.discharged_by_reason]
    open_changes = [c for c in live if c.id not in discharged and not c.discharged_by_reason]

    lines = [
        "## prose",
        "generated: coverage",
        "",
        f"**{len(net) - len(uncovered)}/{len(net)}** atoms have a change id, across **{len(live)}** changes.",
    ]
    if uncovered.is_empty:
        lines.append("Every change in the range is accounted for.")
    else:
        lines.append(f"**{len(uncovered)} atoms still have no id** — `review ingest {cfg.name} --rest` gives them one.")

    lines += ["", f"**{len(live) - len(open_changes)}/{len(live)}** changes are discharged by an entry or a bulk reason."]
    if bulk:
        lines += ["", "### Accepted wholesale", ""]
        for change in bulk:
            lines.append(f"- `{change.id}` {change.path} — {change.reason}")
    if open_changes:
        lines += ["", f"### Not yet discharged ({len(open_changes)})", ""]
        for change in open_changes[:60]:
            lines.append(f"- `{change.id}` {change.summary}")
        if len(open_changes) > 60:
            lines.append(f"- ... and {len(open_changes) - 60} more")
    return "\n".join(lines)


def _replace_generated(entry: Entry, key: str, body: str) -> str:
    """Splice a regenerated block over the one carrying the same key, leaving every other byte alone."""
    for block in entry.blocks:
        if block.attrs.get("generated") == key:
            tail = "\n\n" if entry.text[block.end - 2:block.end] != "\n\n" else ""
            spliced = entry.text[:block.start] + body.strip() + "\n" + tail + entry.text[block.end:]
            return restore_newlines(entry, spliced)
    return restore_newlines(entry, entry.text.rstrip("\n") + "\n\n" + body.strip() + "\n")


def _replace_front_title(text: str, body_start: int, title: str) -> str:
    """Rewrite a generated entry's `title:`, and only within its front matter.

    Bounded to `body_start` so a `title:` line inside someone's prose is never touched.
    The character class excludes both line endings rather than using `.`, which matches a carriage return and would eat it.
    """
    head, rest = text[:body_start], text[body_start:]
    updated, count = re.subn(r"(?m)^title:[^\r\n]*", "title: " + title, head, count=1)
    return (updated if count else head) + rest


# What a generated entry declares about itself: it is a listing to consult, so being read is not something to record.
AUTO_ACK_BLOCK = "## auto-acknowledge\n\nGenerated by the tool and consulted rather than read through.\n"


def ensure(path: Path, front: dict[str, str], key: str, body: str, *, title: str = "") -> Entry:
    """Create a generated entry, or refresh only its generated block if it is already there.

    `title` is refreshed too where given, because a title carrying a live number is stale the moment the block under it moves.
    """
    if path.is_file():
        existing = parse_file(path)
        text = _replace_generated(existing, key, body)
        if title:
            text = _replace_front_title(text, existing.body_start, title)
        return write_entry(path, text)
    return write_entry(path, compose(front, [AUTO_ACK_BLOCK, body]))


def overview_front(cfg: ReviewConfig) -> dict[str, str]:
    return {
        "id": "015",
        "title": OVERVIEW_TITLE,
        "group": "meta" if cfg.has_changeset else "framing",
        "state": "open",
    }


def coverage_front(title: str = COVERAGE_TITLE) -> dict[str, str]:
    return {"id": "990", "title": title, "group": "finalize", "state": "open"}
