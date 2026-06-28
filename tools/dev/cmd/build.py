"""`build` — build targets for the selected preset(s)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools import dev

from . import args as a
from .context import Context

NAME = "build"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Build the project")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    p.add_argument("--target", "-t", action="append",
                   help="Target(s) to build: comma-list, repeatable, wildcards")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--keep-going", "-k", action="store_true",
                   help="Keep building after the first error (ninja -k 0) so one run surfaces every "
                        "independent failure instead of stopping at the first — pairs with --diag-archive.")
    p.add_argument("--diag-archive", metavar="FILE",
                   help="After building, bundle every .diag.json sidecar (one per compile/link, "
                        "written by diag-launcher) into a zip at FILE — the build-step analogue of "
                        "--merged-xml-report. Produced even when the build fails; extract at the repo "
                        "root and inspect with build_diag.")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    # Targets are resolved against the first preset (target sets match across presets).
    target_names = (
        ctx.resolve_target_names(presets[0], args.target, args.emsdk_path)
        if not args.no_configure else None
    )
    if target_names is None and args.target:
        # --no-configure: can't discover, pass the literal names through to cmake.
        target_names = [s.strip() for spec in args.target for s in spec.split(",") if s.strip()]

    results = dev.build(
        presets,
        target_names,
        root=ctx.root,
        auto_configure=not args.no_configure,
        mirror=args.mirror_output,
        verbose=args.verbose,
        emsdk_path=args.emsdk_path,
        keep_going=args.keep_going,
    )
    # Bundle the diag sidecars before the pass/fail gate: a failed build is
    # exactly when its per-invocation compiler errors are worth capturing.
    if args.diag_archive:
        n = dev.archive_diag([p.build_dir for p in presets], Path(args.diag_archive), ctx.root)
        print(f"Diagnostics archive written to {args.diag_archive} ({n} sidecar(s))", file=sys.stderr)
    if not all(r.ok for r in results):
        ctx.fail_build(results, presets)
    build_steps = [r for r in results if r.step_type == "build"]
    files = sum(dev.ninja_built_count(r.stdout_log) for r in build_steps)
    dev.report.summarize_build(build_steps, files, presets)
    sys.exit(0)
