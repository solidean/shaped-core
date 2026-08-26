"""`run` — execute the `example` blocks and splice their output in.

This is the only place in the tool that ever spawns a process out of an entry, and it is a CLI step on purpose.
The server renders agent-authored files; a server that also executed commands out of them would be a different
thing with a different threat model, and there is no reason to combine the two.

The allowed command prefixes live in `review.toml` and default to empty.
Running examples through `dev.py` is a fact about shaped-core, and this tool reviews any git repository — so
nothing executes in a review where nobody said what may execute.

Output always lands in `attachments/` and the block points at it.
That is what makes the escape hatch free: a block the agent ran itself, with its own tool, differs from one this
command produced by a single key.
"""

from __future__ import annotations

import argparse
import subprocess
import time
from pathlib import Path

import tools.review as review

from . import args as a
from .context import Context

NAME = "run"

_TIMEOUT_SECONDS = 600


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run the example blocks and record what they printed")
    a.review_name(p)
    p.add_argument("entry", nargs="?", default="", help="only this entry; omit for every one")
    p.add_argument("--force", action="store_true", help="re-run blocks that already have output")
    p.add_argument("--dry-run", action="store_true", help="say what would run, and run nothing")
    return p


def _allowed(command: str, prefixes: list[str]) -> bool:
    return any(command.strip().startswith(prefix.strip()) for prefix in prefixes if prefix.strip())


def _capture(command: str, cwd: Path) -> tuple[int, str]:
    try:
        proc = subprocess.run(command, cwd=str(cwd), shell=True, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return 124, f"(no output: `{command}` did not finish within {_TIMEOUT_SECONDS}s)"
    except OSError as e:
        return 127, f"(could not run `{command}`: {e})"
    # Stored raw, stderr included.
    # The capture is evidence, and a stripping rule for another tool's preamble is exactly the kind of rule
    # that rots — a quiet flag belongs on that tool instead.
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    paths.create()
    entries = ctx.entries(paths)
    if args.entry:
        entries = [e for e in entries if args.entry in e.slug]
        if not entries:
            ctx.die(f"no entry matches {args.entry!r}")

    prefixes = cfg.run_prefixes
    head = ctx.git.rev_parse("HEAD") or ""
    ran = skipped = refused = 0

    for entry in entries:
        edits: list[tuple[int, int, str]] = []
        for block in entry.blocks:
            if block.type != "example" or not block.attrs.get("run"):
                continue
            command = block.attrs["run"]
            # `#` is a fragment separator, so a browser asking for the attachment would truncate the name there.
            safe = block.block_name.replace("#", "-")
            target = paths.attachments_dir / f"{entry.slug}-{safe}.txt"

            if block.attrs.get("output") and not args.force:
                skipped += 1
                continue
            if not _allowed(command, prefixes):
                refused += 1
                print(review.console.red(
                    f"{entry.slug}/{block.block_name}: `{command}` is not allowed here"))
                print(review.console.dim(
                    f"  add a prefix to `run_prefixes` in {ctx.rel(paths.config)} "
                    f"(currently: {', '.join(prefixes) or 'none'})"))
                continue
            if args.dry_run:
                print(f"{entry.slug}/{block.block_name}: would run `{command}`")
                continue

            print(f"{entry.slug}/{block.block_name}: {command}")
            code, output = _capture(command, ctx.repo)
            review.write_atomic(target, output)
            ran += 1

            # The sha and the time are what make the output a claim about a commit rather than a screenshot of one.
            attrs = {
                "output": f"attachments/{target.name}",
                "status": "ok" if code == 0 else "failed",
                "sha": head[:12],
                "at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            edits.append((block, attrs))
            if code != 0:
                print(review.console.yellow(f"  exit {code}; the output is recorded either way"))

        if edits and not args.dry_run:
            text = review.set_block_attrs(entry, edits)
            review.write_atomic(entry.path, text)

    if refused:
        print(review.console.red(f"\n{refused} block(s) refused"))
    print(review.console.green(f"{ran} run, {skipped} already had output"))
    if skipped and not args.force:
        print(review.console.dim("`--force` re-runs those"))
    if refused:
        raise SystemExit(1)
