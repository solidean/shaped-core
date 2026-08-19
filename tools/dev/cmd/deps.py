"""`deps` — what we pin under extern/, and the licenses we ship.

Thin wiring: the logic lives in tools/deps/deps.py, which declares its own pyyaml so dev.py stays dependency-free.
`list` reaches the network by default and is deliberately not a `check` gate; `licenses --check` is offline, and is one.
"""

from __future__ import annotations

import argparse

from tools import dev

from . import args as a
from .context import Context

NAME = "deps"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="List external dependencies and collect their licenses")
    deps_sub = p.add_subparsers(dest="deps_cmd", required=True)

    ls = deps_sub.add_parser("list", help="Every dependency, its pin, and what upstream offers now")
    a.profile(ls)
    ls.add_argument("--offline", action="store_true", help="Manifests and installed pins only, no network")
    ls.add_argument("--refresh", action="store_true", help="Ignore the day-old cache of upstream lookups")
    ls.add_argument("--json", action="store_true", dest="as_json", help="Machine-readable output")

    lic = deps_sub.add_parser("licenses", help="Regenerate docs/licenses/ from the dependency manifests")
    a.profile(lic)
    lic.add_argument("--check", action="store_true", help="Verify without writing (no network); what `check` runs")
    return p


def _run_deps(ctx: Context, *, argv: list[str], name: str, verbose: bool = False) -> bool:
    """Run tools/deps/deps.py; return True on a clean exit.

    No preset is involved — nothing here is per-build — so the run log goes under the plain build/ root.
    """
    runner = ctx.root / "tools" / "deps" / "deps.py"
    color = "always" if dev.console.enabled() else "never"
    result = dev.run_step(
        ["uv", "run", str(runner), "--color", color, *argv],
        step_type="deps", name=name,
        build_dir=ctx.root / "build", cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def run_licenses(ctx: Context, *, check: bool, verbose: bool = False) -> bool:
    """Regenerate or verify docs/licenses/; return True if clean.

    Shared by `deps licenses` and the `check` deps-licenses gate.
    """
    return _run_deps(ctx, argv=["licenses"] + (["--check"] if check else []), name="deps-licenses", verbose=verbose)


def run_list(ctx: Context, *, offline: bool, refresh: bool, as_json: bool, verbose: bool = False) -> bool:
    argv = ["list"]
    if offline:
        argv.append("--offline")
    if refresh:
        argv.append("--refresh")
    if as_json:
        argv.append("--json")
    return _run_deps(ctx, argv=argv, name="deps-list", verbose=verbose)


def run(args: argparse.Namespace, ctx: Context) -> None:
    verbose = getattr(args, "verbose", False)
    match args.deps_cmd:
        case "list":
            ok = run_list(ctx, offline=args.offline, refresh=args.refresh, as_json=args.as_json, verbose=verbose)
            raise SystemExit(0 if ok else 1)
        case "licenses":
            ok = run_licenses(ctx, check=args.check, verbose=verbose)
            raise SystemExit(0 if ok else 1)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown deps subcommand {args.deps_cmd!r}")
