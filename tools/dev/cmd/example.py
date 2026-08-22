"""`example` — list the repo's examples, or build and run exactly one.

An example is a nexus `EXAMPLE` declaration: a runnable demonstration of an API in practice, in its own selection bucket.
Every build compiles them and nothing runs them automatically, so this is how one gets executed.

Resolution is a cross-binary name lookup, not a target lookup: an example name says nothing about the binary carrying it, so every `*-example` target is built and probed.
`--target` narrows which binaries are probed; the match never does.
docs/guides/examples.md is the concept behind all of it.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.pipeline.examples import collect_examples, is_example_target, resolve_example

from . import args as a
from .context import Context

NAME = "example"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run one example, or list them all (args after the match are forwarded)")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    a.profile(p)
    p.add_argument("--target", "-t", action="append",
                   help="Example binary target(s) to consider: comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                   help="Kill the example after SECS (default: 0, no limit — an example may be interactive)")
    p.add_argument("--background", action="store_true",
                   help="Ask the example not to steal focus: its windows appear without being activated. "
                        "Sets SC_REQUEST_BACKGROUND=1, which sr::window_system reads (sr::background_request_env_var).")
    p.add_argument("--test-args", metavar="LINE",
                   help="A command line for the example itself, reachable from its body through "
                        "nx::current_args(). Forwarded as one string and tokenized by the runner, so the "
                        "example's own flags never collide with dev.py's. Replaces whatever the example "
                        "declared with nx::config::args.")
    p.add_argument("match", nargs="?",
                   help="Example name, or a substring of one. Must select exactly one; omit to list them all.")
    # Everything dev.py does not recognize is forwarded verbatim to the example, collected into args.runner_args.
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    preset = presets[0]

    program_args = list(args.runner_args or [])
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]

    # One string, tokenized by the runner, so the example's own flags never have to survive dev.py's
    # argument handling — and never collide with the --examples the launch below already passes.
    if args.test_args is not None:
        program_args = ["--test-args", args.test_args, *program_args]

    wanted = ctx.resolve_target_names(preset, args.target, args.emsdk_path) if args.target else None

    # Every example binary is built before probing: a listing can only come from an artifact that exists.
    # Incremental, so this is cheap once the corpus is built, and it is also what keeps a stale name from resolving.
    if not args.no_build:
        targets = ctx.discover(preset, args.emsdk_path)
        names = wanted if wanted is not None else [t.name for t in targets if is_example_target(t)]
        if not names:
            ctx.die("no *-example targets are configured. Is SC_BUILD_EXAMPLES on?")
        results = dev.build([preset], names, root=ctx.root,
                            auto_configure=not args.no_configure,
                            mirror=args.mirror_output, verbose=args.verbose,
                            emsdk_path=args.emsdk_path)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])

    # Discovered after the build: a first-time configure knows the target but has not linked its artifact yet.
    targets = ctx.discover(preset, args.emsdk_path)
    examples = collect_examples(preset, targets, root=ctx.root, binary_names=wanted)

    if args.match is None:
        _print_listing(ctx, examples)
        return

    example, diagnostic = resolve_example(examples, args.match)
    if example is None:
        ctx.die(diagnostic)

    artifact = next((t.artifact for t in targets if t.name == example.target and t.artifact), None)
    if artifact is None:
        ctx.die(f"target {example.target!r} has no built artifact for preset {preset.name!r}")

    print(console.dim(f"{example.name}  ({example.target}, {example.file}:{example.line})"), file=sys.stderr)

    # An environment variable rather than a flag: the example binary is a program dev.py did not write, and nexus has no say over what a window system does.
    # sr::window_system reads this one; see docs/guides/examples.md.
    env = None
    if args.background:
        env = {**os.environ, "SC_REQUEST_BACKGROUND": "1"}

    # The exact name plus the bucket flag: an example is never swept, so it must be named to run.
    # Mirrored, because watching the example run is the entire point of the command.
    result = dev.run_step(
        [str(artifact), example.name, "--examples", *program_args],
        step_type="example", name=example.target,
        build_dir=preset.build_dir, cwd=_working_directory(ctx, example), env=env,
        timeout=args.timeout if args.timeout else None,
        mirror=True, verbose=args.verbose,
    )
    sys.exit(result.returncode)


def _working_directory(ctx: Context, example) -> Path:
    """The directory an example runs in: the one its source sits in.

    An example's relative paths — the document it keeps, the assets it loads — should resolve next to the example
    rather than next to whatever the user happened to invoke dev.py from.
    That also lets the example's own directory carry the .gitignore for what a run leaves behind.

    Falls back to the repo root when the recorded source path is not on this machine, which is what a binary built
    elsewhere reports.
    """
    directory = Path(example.file).parent
    return directory if directory.is_dir() else ctx.root


def _print_listing(ctx: Context, examples: list) -> None:
    """Print every example, grouped by the binary carrying it."""
    if not examples:
        ctx.die("no examples found. Is SC_BUILD_EXAMPLES on, and has anything declared an EXAMPLE?")

    by_target: dict[str, list] = {}
    for e in examples:
        by_target.setdefault(e.target, []).append(e)

    for target in sorted(by_target):
        print(console.dim(target))
        for e in by_target[target]:
            print(f"  {e.name}")
            print(console.dim(f"      {ctx.rel(Path(e.file))}:{e.line}"))

    print(console.dim(f"\n{len(examples)} example(s). Run one with: dev.py example <name>"))
