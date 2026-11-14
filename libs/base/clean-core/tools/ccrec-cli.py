#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///

"""Looks inside a `.ccrec` recording from the command line, through the `ccrec` package beside this script.

    uv run libs/base/clean-core/tools/ccrec-cli.py summary run.ccrec
    uv run libs/base/clean-core/tools/ccrec-cli.py scopes run.ccrec [--top 20]
    uv run libs/base/clean-core/tools/ccrec-cli.py events run.ccrec [--thread <index>] [--limit 200]

`summary` is the header, the threads and what kinds of events each holds.
`scopes` is the scope names by total time, with their counts.
`events` prints the stream itself, one decoded event per line.
A script that wants more imports the package; libs/base/clean-core/docs/systems/recording-formats.md shows how.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ccrec  # noqa: E402


def _kind_name(kind) -> str:
    return kind.name if isinstance(kind, ccrec.EventKind) else f"kind#{kind}"


def summary(rec: ccrec.Recording) -> None:
    print(f"format version {rec.version}, {rec.event_count()} events in {len(rec.blocks)} blocks"
          f"{' (truncated)' if rec.is_truncated else ''}")
    print(f"cycle rate {rec.cycles_per_second / 1e9:.3f} GHz, {len(rec.descs)} sites, {len(rec.modules)} modules")
    for t in rec.threads:
        kinds = Counter(_kind_name(e.kind) for b in rec.blocks_of_thread(t.index) for e in b)
        listed = ", ".join(f"{k} {n}" for k, n in kinds.most_common())
        print(f"  thread {t.index} {t.name or '(unnamed)'} [tid {t.tid}]: {listed}")


def scopes(rec: ccrec.Recording, top: int) -> None:
    a = ccrec.analyze(rec)
    totals = a.total_cycles_by_name()
    counts = Counter(s.name for s in a.scopes if not s.is_open)
    for name, cycles in sorted(totals.items(), key=lambda p: -p[1])[:top]:
        print(f"{rec.secs(cycles) * 1e3:12.3f} ms  {counts[name]:8d}x  {name}")


def events(rec: ccrec.Recording, thread: int | None, limit: int) -> None:
    shown = 0
    for e in rec.sorted_events():
        if thread is not None and e.thread.index != thread:
            continue
        fields = ", ".join(f"{k}={v!r}" for k, v in e.fields().items())
        print(f"{e.cycles:>20} t{e.thread.index:<3} {_kind_name(e.kind):<16} {e.name}  {fields}")
        shown += 1
        if shown >= limit:
            break


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("summary", "scopes", "events"):
        p = sub.add_parser(name)
        p.add_argument("recording", type=Path)
        if name == "scopes":
            p.add_argument("--top", type=int, default=30)
        if name == "events":
            p.add_argument("--thread", type=int)
            p.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()

    try:
        rec = ccrec.load(args.recording)
    except ccrec.FormatError as error:
        print(f"{args.recording}: {error}", file=sys.stderr)
        return 1

    if args.command == "summary":
        summary(rec)
    elif args.command == "scopes":
        scopes(rec, args.top)
    else:
        events(rec, args.thread, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
