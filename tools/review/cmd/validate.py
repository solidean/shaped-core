"""`validate` — check every entry before handing the review over.

The other commands surface a problem when they happen to trip over it.
This one goes looking, which is what you want before serving a round: a mistyped change id is a discharge
that silently does not discharge, and the coverage report would report progress that was never made.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "validate"

# UTF-8 bytes that were decoded as cp1252 and written back out.
#
# An em dash becomes `â€”`, a right quote `â€™`, a non-breaking space `Â `.
# None of these is text anyone types on purpose, and the tool itself produced them until `append` stopped decoding
# stdin through the locale — an entry file is hand-editable, so the check stays whatever the writers do.
MOJIBAKE_MARKERS = ("â€", "Ã¢", "Ãƒ", "Â ")


def mojibake_warnings(entry) -> list[str]:
    """One warning per line that looks like UTF-8 read as cp1252."""
    out: list[str] = []
    for number, line in enumerate(entry.text.splitlines(), start=1):
        for marker in MOJIBAKE_MARKERS:
            if marker in line:
                out.append(
                    f"{entry.slug}:{number}: {marker!r} looks like UTF-8 decoded as cp1252 — "
                    f"the line probably lost an em dash or a quote"
                )
                break
    return out


def orphan_answer_warnings(entry, answers) -> list[str]:
    """One warning per answer whose ask is gone, which `delta` will move aside.

    An acknowledgement is not one: it stops being offered once a later round asks something, and `reconcile` keeps
    its answer, so warning about it contradicts what the next `delta` does.
    """
    return [
        f"{entry.slug}: an answer to {name!r} has no ask; `delta` will orphan it"
        for name in sorted(answers.answers)
        if entry.ask(name) is None and not review.is_ack_name(name)
    ]


def stranded_attribute_warnings(entry) -> list[str]:
    """One warning per ask whose body opens, after the blank line that ends a prelude, with one of its own attributes.

    The blank line is the grammar's escape for prose that must start with `something:`, so the parser is right to
    read it as question text — but for an ask it is nearly always a `discharges:` that silently discharges nothing.
    """
    out: list[str] = []
    for block in entry.asks:
        first = next((line for line in block.prose.splitlines() if line.strip()), "")
        m = review.ATTR_RE.match(first)
        if m and m.group(1) in review.BLOCK_TYPES["ask"]:
            out.append(
                f"{entry.slug}: ask {block.name!r} opens its text with `{m.group(1)}:`, which a blank line above it "
                f"made prose — delete the blank line to make it an attribute"
            )
    return out


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Check every entry parses and every reference resolves")
    a.review_name(p)
    p.add_argument("--quiet", action="store_true", help="report nothing when everything is fine")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    entries = ctx.entries(paths)

    problems: list[str] = []
    warnings: list[str] = []

    if cfg.has_changeset:
        problems.extend(ctx.check_references(paths, entries))

    # A file reference the tool cannot resolve renders as plain text, which is indistinguishable from one nobody
    # meant as a reference — so the check goes looking rather than waiting to be tripped over.
    problems.extend(ctx.reference_problems(paths, entries))

    # A paragraph in a glossary block that is not a term is a term nobody finds out is missing,
    # which is the whole reason the block is marked rather than scraped.
    problems.extend(review.glossary_problems(entries))

    for entry in entries:
        warnings.extend(mojibake_warnings(entry))
        warnings.extend(stranded_attribute_warnings(entry))
        answers = ctx.answers(paths, entry)
        open_asks = {b.name for b in entry.asks if (answers.get(b.name) is None or answers.get(b.name).tentative)}
        if not review.is_orientation(entry.group):
            for round_number in review.missing_intro_rounds(entry, open_asks):
                warnings.append(
                    f"{entry.slug}: round {round_number} asks something with no `intro` — open it with what the entry "
                    f"is about and the options, before any fact or trade-off"
                )
        # A follow-up belongs under the ask it follows, where the answer it responds to is on screen above it.
        # Naming an ask in another entry usually means a new entry was opened where a round should have been appended,
        # which splits one thread across two files and makes the second restate what the first established.
        for block in entry.asks:
            target = block.attrs.get("follows", "")
            if not target or entry.ask(target) is not None:
                continue

            # A superseded ask IS in this entry; it has just been retired, so `entry.ask` no longer finds it.
            # Saying it is "not an ask in this entry" sends the reader looking for a missing entry, when what they have
            # is a redundant attribute: `supersedes:` already retires the question, so `follows:` adds nothing.
            retired = any(b.is_ask and b.name == target for b in entry.blocks)
            if retired:
                warnings.append(
                    f"{entry.slug}: ask {block.name!r} follows {target!r}, which this entry has superseded — "
                    f"`supersedes:` already retires it, so `follows:` is redundant here"
                )
            else:
                warnings.append(
                    f"{entry.slug}: ask {block.name!r} follows {target!r}, which is not an ask in this entry — "
                    f"a follow-up usually belongs appended to the entry it follows"
                )

        warnings.extend(orphan_answer_warnings(entry, answers))

    # A comment is written expecting an answer, so the agent may not hand back another round while one is unanswered.
    # The gate sits here rather than on the maintainer's send: they wrote the remark, and blocking their own send on it
    # would be the tool refusing to deliver a message because the message exists.
    for slug, comment in ctx.unaddressed_comments(paths, entries):
        problems.append(
            f"{slug}: comment {comment.id} (on {comment.where()}) has no answer — "
            f"append a block with `addresses: {comment.id}`, which a block that declines to act also satisfies"
        )

    groups = set(review.groups_for(cfg.goals))
    unplaced = sorted({e.group for e in entries} - groups)
    if unplaced:
        warnings.append(
            f"groups outside this review's skeleton: {', '.join(unplaced)}. "
            f"This review's groups are: {', '.join(review.groups_for(cfg.goals))}"
        )

    for warning in warnings:
        print(review.console.yellow(f"warning: {warning}"))
    for problem in problems:
        print(review.console.red(f"error: {problem}"))

    if problems:
        print(review.console.red(f"\n{len(problems)} problem(s) across {len(entries)} entries"))
        raise SystemExit(1)
    if not args.quiet:
        # A design review has no ledger, so `check_references` never ran — claiming references resolve would be
        # reporting a check that did not happen.
        changes = ", every change id resolves" if cfg.has_changeset else ""
        print(review.console.green(f"{len(entries)} entries parse, every file reference resolves{changes}"))
