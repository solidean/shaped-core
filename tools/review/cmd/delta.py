"""`delta` — what the maintainer has answered since the last round.

This is the handover, and it is deliberately a *delta*.
Re-printing the entries would bury the two sentences that actually changed the work in everything the agent already wrote.

`--finalize` is the deliberate half: it freezes the round, moves the watermark, and writes the round's transcript.
Without it this command is a peek, which is what makes it safe to run while the maintainer is still working.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "delta"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Print the answers given since the last round")
    a.review_name(p)
    p.add_argument("--finalize", action="store_true",
                   help="freeze the round, move the watermark and write its transcript")
    p.add_argument("--round", type=int, default=0,
                   help="print the transcript of an already-finalized round instead")
    return p


def _entry_delta(entry: review.Entry, answers: review.AnswerFile, watermark: int) -> list[str]:
    fresh = [a for a in answers.since(watermark)]
    if not fresh:
        return []
    severity = f"/{entry.severity}" if entry.severity else ""
    out = [f"## {entry.id} {entry.title}   [{entry.group}{severity}]"]
    for answer in sorted(fresh, key=lambda a: a.name):
        block = entry.ask(answer.name)
        out.append(f"### {answer.name}")
        if block is not None and block.discharges:
            out.append(f"discharges: {' '.join(block.discharges)}")
        for choice in answer.selected:
            out.append(f"  chose: {choice}")
        if answer.text.strip():
            for line in answer.text.strip().splitlines():
                out.append(f"  said: {line}")
        if not answer.selected and not answer.text.strip():
            out.append("  (empty)")
    return out


def _progress(entries, pairs, ledger) -> list[str]:
    asks = answered = 0
    open_entries = 0
    for entry, answers in pairs:
        if entry.state != "open":
            continue
        entry_asks = entry.asks
        entry_answered = sum(1 for b in entry_asks if (a := answers.get(b.name)) and not a.is_empty)
        asks += len(entry_asks)
        answered += entry_answered
        if entry_answered < len(entry_asks):
            open_entries += 1

    discharged = set()
    for entry in entries:
        if entry.state == "open":
            discharged.update(entry.discharged_changes())
    live = ledger.live()
    covered = sum(1 for c in live if c.id in discharged or c.discharged_by_reason)

    entry_word = "entry" if open_entries == 1 else "entries"
    return [
        f"progress: {answered}/{asks} questions answered, "
        f"{covered}/{len(live)} changes discharged, "
        f"{open_entries} {entry_word} still open",
    ]


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)

    if args.round:
        transcript = paths.round_file(args.round)
        if not transcript.is_file():
            ctx.die(f"round {args.round} has not been finalized (no {ctx.rel(transcript)})")
        print(transcript.read_text(encoding="utf-8").rstrip("\n"))
        return

    ctx.stamp(paths, cfg)
    entries = ctx.entries(paths)
    pairs = [(entry, ctx.answers(paths, entry)) for entry in entries]
    ledger = ctx.ledger(paths)
    watermark = cfg.watermark
    limit = cfg.next_round

    if args.finalize:
        for entry, answers in pairs:
            if answers.finalize(cfg.next_round):
                answers.save()

    body: list[str] = []
    for entry, answers in pairs:
        lines = _entry_delta(entry, answers, watermark)
        if lines:
            body.append("\n".join(lines))

    header = [
        f"round {limit} of review {cfg.name}  (goals: {', '.join(cfg.goals)})",
        "",
    ]
    footer = ["", *_progress(entries, pairs, ledger)]

    if not body:
        text = "\n".join([*header, "no new answers since the last round.", *footer])
    else:
        text = "\n".join([*header, "\n\n".join(body), *footer])

    if args.finalize:
        cfg.watermark = cfg.next_round
        review.save(paths.config, cfg)
        review.write_atomic(paths.round_file(limit), text + "\n")
        review.record(paths.log, "finalize", round=limit)
        paths.signal.unlink(missing_ok=True)

    print(text)
