"""`run` — build one executable target and run it, forwarding the rest of the command line.

The counterpart to `test` for everything that is not a test: a tool, a benchmark, a sample.
`*-test` targets are refused rather than bypassing `dev.py test`, which discovers, filters and records results.
`*-example` targets likewise belong to `dev.py example`, which resolves an example name across every example binary.
docs/guides/building-and-testing.md says why hand-writing an artifact path is the thing this replaces.
"""

from __future__ import annotations

import argparse
import sys

from tools import dev

from tools.dev.lib.toolchain import jsruntime as jsr

from . import args as a
from .context import Context

NAME = "run"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Build one executable target and run it (args after the target are forwarded)")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    a.jsruntime(p)
    a.profile(p)
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                   help="Kill the program after SECS (default: 0, no limit)")
    p.add_argument("--quiet", action="store_true",
                   help="Capture the program's output to the step log instead of mirroring it live")
    p.add_argument("target", help="Executable target to run (exact name, or a wildcard matching exactly one)")
    # Everything dev.py does not recognize is forwarded verbatim to the program, collected by the top-level parse_known_args into args.runner_args.
    # An optional leading `--` is dropped.
    # dev.py's own options still bind wherever they sit, at the cost that a program flag colliding with one of ours needs the `--` separator.
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    preset = presets[0]

    program_args = list(args.runner_args or [])
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]

    # Resolve the target before building, so a typo fails immediately rather than after a full build.
    targets = ctx.discover(preset, args.emsdk_path)
    name = _resolve_one(ctx, targets, args.target)

    if not args.no_build:
        results = dev.build([preset], [name], root=ctx.root,
                            auto_configure=not args.no_configure,
                            mirror=args.mirror_output, verbose=args.verbose,
                            emsdk_path=args.emsdk_path)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])
        # The artifact path can only be trusted after the build: a first-time configure discovers the target but has not linked it yet.
        targets = ctx.discover(preset, args.emsdk_path)

    artifact = next((t.artifact for t in targets if t.name == name and t.artifact), None)
    if artifact is None:
        ctx.die(f"target {name!r} has no built artifact for preset {preset.name!r}")

    # A wasm artifact is a .js loader plus a .wasm and cannot be executed directly.
    launcher = (
        jsr.LazyLauncher(jsr.JsRuntimeRequest.from_args(args), dev.emsdk_env(args.emsdk_path)).prefix()
        if jsr.needs_launcher(preset.is_emscripten, artifact)
        else []
    )

    # Mirrored by default: seeing the program's output IS the point of this command.
    result = dev.run_step(
        [*launcher, str(artifact), *program_args],
        step_type="run", name=name,
        build_dir=preset.build_dir, cwd=ctx.root,
        timeout=args.timeout if args.timeout else None,
        mirror=not args.quiet, verbose=args.verbose,
    )
    # The program's own exit code, not a pass/fail verdict: a linter's non-zero "found something" has to reach the caller intact.
    sys.exit(result.returncode)


def _resolve_one(ctx: Context, targets: list[dev.Target], spec: str) -> str:
    """The one executable target `spec` names, or exit with a diagnostic."""
    import fnmatch

    runnable = [t for t in targets if t.kind == "EXECUTABLE"]
    exact = [t for t in runnable if t.name == spec]
    matches = exact or [t for t in runnable if fnmatch.fnmatch(t.name, spec)]

    if not matches:
        available = sorted(t.name for t in runnable if not ctx.is_test_target(t) and not ctx.is_example_target(t))
        ctx.die(f"no executable target matches {spec!r}. Runnable: {', '.join(available)}")
    if len(matches) > 1:
        ctx.die(f"{spec!r} matches several targets: {', '.join(sorted(t.name for t in matches))}")

    target = matches[0]
    if ctx.is_test_target(target):
        ctx.die(f"{target.name!r} is a test binary — run it with `dev.py test`, which discovers, "
                f"filters and records results. `run` is for non-test executables.")
    if ctx.is_example_target(target):
        ctx.die(f"{target.name!r} is an example binary — run one of its examples with `dev.py example <name>`, "
                f"which resolves the name across every example binary. `run` is for plain executables.")
    return target.name
