"""`status` — where a review stands, in one call.

Answers the four questions asked at the top of every round: is a server actually up, which round is next,
what is still open, and what has been typed but not handed back.

`list` says which reviews exist; this says what is happening inside one.
The server line probes the port rather than trusting `.served`, because a killed `serve` leaves its marker behind
and a url nobody is listening on is worse than no url.
"""

from __future__ import annotations

import argparse
import json

import tools.review as review

from . import args as a
from .context import Context
from .serve import is_up

NAME = "status"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Where this review stands: server, round, what is open")
    a.review_name(p)
    a.as_json(p)
    return p


def collect(ctx: Context, name: str) -> dict:
    paths, cfg = ctx.open(name)
    entries = ctx.entries(paths)

    served = review.read_json(paths.served_marker)
    url = str(served.get("url", ""))
    live = is_up(url)

    open_entries: list[dict] = []
    total = answered = tentative = 0
    for entry in entries:
        answers = ctx.answers(paths, entry)
        asks = entry.asks
        total += len(asks)
        waiting = []
        for block in asks:
            a_ = answers.get(block.name)
            if a_ is not None and not a_.is_empty:
                answered += 1
                if a_.tentative:
                    tentative += 1
            else:
                waiting.append(block.name)
        if waiting:
            open_entries.append({"entry": entry.slug, "title": entry.title, "asks": waiting})

    out = {
        "name": cfg.name,
        "round": cfg.next_round,
        "watermark": cfg.watermark,
        "repo": cfg.repo,
        "server": {"url": url, "live": live, "stale_marker": bool(url) and not live},
        "entries": len(entries),
        "asks": total,
        "answered": answered,
        "tentative": tentative,
        "open": open_entries,
        "unaddressed": [
            {"entry": slug, "id": c.id, "where": c.where()}
            for slug, c in ctx.unaddressed_comments(paths, entries)
        ],
    }

    if cfg.has_changeset:
        ledger = ctx.ledger(paths)
        live_changes = ledger.live()
        discharged = ctx.discharged(entries)
        out["changes"] = len(live_changes)
        out["discharged"] = sum(1 for c in live_changes if c.id in discharged or c.discharged_by_reason)
    return out


def run(args: argparse.Namespace, ctx: Context) -> None:
    s = collect(ctx, args.name)

    if args.json:
        print(json.dumps(s, indent=2))
        return

    print(f"{s['name']}  round {s['round']}  ({s['entries']} entries)")

    server = s["server"]
    if server["live"]:
        print(f"  server     {server['url']}")
    elif server["stale_marker"]:
        print(review.console.yellow(f"  server     recorded at {server['url']}, not answering — `review restart {s['name']}`"))
    else:
        print(review.console.dim(f"  server     not running — `review serve {s['name']}`"))

    print(f"  answered   {s['answered']}/{s['asks']}" + (f", {s['tentative']} not yet handed back" if s["tentative"] else ""))
    if "changes" in s:
        print(f"  changes    {s['discharged']}/{s['changes']} discharged")

    # Reported before it blocks: `validate` refuses to hand back a round while one of these is unanswered.
    if s["unaddressed"]:
        print(review.console.yellow(f"  comments   {len(s['unaddressed'])} with no answer yet"))
        for row in s["unaddressed"]:
            print(review.console.yellow(f"    {row['entry']}  {row['id']}  on {row['where']}"))

    if s["open"]:
        print(f"  open       {len(s['open'])} entr{'y' if len(s['open']) == 1 else 'ies'}")
        for row in s["open"]:
            print(f"    {row['entry']}  {', '.join(row['asks'])}")
    elif s["tentative"]:
        print(review.console.dim("  every ask has an answer, none of it handed back yet"))
    else:
        print(review.console.green("  nothing open"))
