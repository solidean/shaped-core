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
    thin = False

    if cfg.has_changeset:
        problems.extend(ctx.check_references(paths, entries))

    # A file reference the tool cannot resolve renders as plain text, which is indistinguishable from one nobody
    # meant as a reference — so the check goes looking rather than waiting to be tripped over.
    problems.extend(ctx.reference_problems(paths, entries))

    # A paragraph in a glossary block that is not a term is a term nobody finds out is missing,
    # which is the whole reason the block is marked rather than scraped.
    problems.extend(review.glossary_problems(entries))

    for entry in entries:
        warnings.extend(review.word_warnings(entry))
        warnings.extend(mojibake_warnings(entry))
        answers = ctx.answers(paths, entry)
        # The tiers exist so an entry can be answered on its own, so they are owed while it is still waiting for an answer.
        # An entry whose asks are all finalized will not be answered again, and adding tiers to it would edit a question
        # the maintainer has already read — which is the thing the immutability check exists to prevent.
        awaiting = any(a is None or a.tentative for a in (answers.get(b.name) for b in entry.asks))
        if awaiting and review.requires_context(entry.group):
            absent = review.missing_context_tiers(entry)
            if absent:
                thin = True
                problems.append(f"{entry.slug}: no {', '.join(absent)}")
        # A follow-up belongs under the ask it follows, where the answer it responds to is on screen above it.
        # Naming an ask in another entry usually means a new entry was opened where a round should have been appended,
        # which splits one thread across two files and makes the second restate the first's context.
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

        for name in sorted(answers.answers):
            if entry.ask(name) is None:
                warnings.append(f"{entry.slug}: an answer to {name!r} has no ask; `delta` will orphan it")

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

    if thin:
        # Said once rather than per entry: a rule repeated twelve times reads as twelve rules.
        print(review.console.dim(
            f"\nEvery entry outside {', '.join(sorted(review.CONTEXT_EXEMPT_GROUPS))} carries all three context tiers"
            f" while it is still waiting for an answer, so it can be answered on its own, out of order."
        ))
        print(review.console.dim(
            "Scope each tier to that entry's own subject rather than to the change as a whole,"
            " or every cold tier restates the same paragraph and nobody opens one again."
        ))

    if problems:
        print(review.console.red(f"\n{len(problems)} problem(s) across {len(entries)} entries"))
        raise SystemExit(1)
    if not args.quiet:
        # A design review has no ledger, so `check_references` never ran — claiming references resolve would be
        # reporting a check that did not happen.
        changes = ", every change id resolves" if cfg.has_changeset else ""
        print(review.console.green(f"{len(entries)} entries parse, every file reference resolves{changes}"))
