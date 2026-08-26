"""The two entries the tool writes for itself, and the rule that keeps them safe to rewrite.

Only the overview and the coverage report are generated, and every other entry is authored end to end:
the judgement about what matters in a change is the most useful thing an entry can carry, and nothing mechanical produces it.

A generated block carries `generated: <key>`, and regeneration replaces exactly those blocks.
Everything else in the file — including prose written directly underneath one — is left untouched,
so refreshing the overview after a `sync` never costs a sentence anyone wrote.
"""

from __future__ import annotations

import html
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

# Where the tree stops being an overview.
# Past this the leaves of the largest directories fold into a count, and the tail says what it dropped —
# a silent truncation reads as "that is the whole change" when it is not.
_MAX_ROWS = 100


def _esc(text: str) -> str:
    return html.escape(text, quote=True)


def _counts(net: LineSpace) -> dict[str, tuple[int, int]]:
    """Added and removed lines per path, which net line space already has keyed by side."""
    out: dict[str, tuple[int, int]] = {}
    for (side, path), lines in net.lines.items():
        added, removed = out.get(path, (0, 0))
        out[path] = (added + len(lines), removed) if side == "+" else (added, removed + len(lines))
    return out


def _fold(paths: list[str]) -> dict:
    """The touched files as a tree, with single-child directory chains folded onto one node.

    `libs/base/clean-core/src` is one row rather than four.
    A change is located by its path, so the tree is in path order rather than sorted by size — a tree ordered
    by how much changed is a chart, and a reader is looking for a place.
    """
    root: dict = {"dirs": {}, "files": []}
    for path in sorted(paths):
        parts = path.split("/")
        node = root
        for part in parts[:-1]:
            node = node["dirs"].setdefault(part, {"dirs": {}, "files": []})
        node["files"].append(parts[-1])

    def collapse(node: dict, name: str) -> tuple[str, dict]:
        while not node["files"] and len(node["dirs"]) == 1:
            child_name, child = next(iter(node["dirs"].items()))
            name, node = f"{name}/{child_name}" if name else child_name, child
        node["dirs"] = dict(collapse(child, child_name) for child_name, child in node["dirs"].items())
        return name, node

    _, folded = collapse(root, "")
    return folded


def _tree_html(paths: list[str], counts: dict[str, tuple[int, int]]) -> str:
    """The tree as rows: a row shows its own segment, and a file row is the link into it.

    Linked here rather than by the annotation pass.
    A tree row shows a basename while the link needs the whole path, and the pass matches on the literal text
    it can see — so the one place that knows both is the one that emits the row.
    """
    rows: list[str] = []
    dropped = [0, 0]  # files, directories

    def walk(node: dict, prefix: str, depth: int) -> None:
        indent = "  " * depth
        for name, child in node["dirs"].items():
            full = f"{prefix}{name}/"
            rows.append(f'<div class="tree-dir">{indent}<span>{_esc(name)}/</span></div>')
            walk(child, full, depth + 1)
        if len(rows) > _MAX_ROWS and node["files"]:
            dropped[0] += len(node["files"])
            dropped[1] += 1
            return
        for name in node["files"]:
            path = f"{prefix}{name}"
            added, removed = counts.get(path, (0, 0))
            rows.append(
                f'<div class="tree-file">{indent}'
                f'<a class="annot ref" href="/file/{_esc(path)}" target="_blank" rel="noopener"'
                f' data-path="{_esc(path)}" data-line="0" title="{_esc(path)}">{_esc(name)}</a>'
                f'<span class="tree-delta"><span class="add">+{added}</span>'
                f'<span class="del">-{removed}</span></span></div>'
            )

    walk(_fold(paths), "", 0)
    tail = ""
    if dropped[0]:
        tail = (f'<div class="tree-tail">… and {dropped[0]} more files under '
                f'{dropped[1]} director{"y" if dropped[1] == 1 else "ies"}</div>')
    return f'<div class="tree">{"".join(rows)}{tail}</div>'


def _commits_html(commits: list, merges: set[str]) -> str:
    """The commits as rows.

    Bare shas: the annotation pass links them and carries the popover.
    """
    rows = []
    for commit in commits[:_MAX_COMMITS]:
        merge = '<span class="commit-merge">merge</span>' if commit.sha in merges else ""
        rows.append(
            f'<div class="commit-row"><code class="commit-sha">{_esc(commit.short)}</code>'
            f'<span class="commit-subject">{_esc(commit.subject)}</span>{merge}'
            f'<span class="commit-who">{_esc(commit.author)}</span>'
            f'<span class="commit-when">{_esc(commit.date)}</span></div>'
        )
    if len(commits) > _MAX_COMMITS:
        rows.append(f'<div class="tree-tail">… and {len(commits) - _MAX_COMMITS} more commits</div>')
    return f'<div class="commits">{"".join(rows)}</div>'


def overview_body(git: Git, cfg: ReviewConfig, net: LineSpace) -> str:
    """The generated half of the overview: the range, its commits, and where the change lands.

    Emitted as HTML rather than as markdown.
    `generated:` already means the tool owns the block end to end — regeneration replaces it wholesale and nobody
    is expected to edit it — so it is the one place in the format where hand-readability is not the point.
    """
    commits = git.commits(cfg.base, cfg.head)
    merges = set(git.has_merges(cfg.base, cfg.head))
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
        + (f", {len(net.files)} file-level changes" if net.files else "")
        + (f", {len(merges)} of them merges" if merges else ""),
        "",
        "### Commits",
        "",
        _commits_html(commits, merges),
        "",
        "### Where it lands",
        "",
        _tree_html(paths, _counts(net)),
    ]
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
