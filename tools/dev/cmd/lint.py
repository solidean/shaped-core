"""`lint` — the front door for every linter, each of which lives under its own tool directory.

`clang-tidy` runs the whitelist gates in tools/lint/, `shaped` runs our own parser-based rules in tools/shaped-linter/, and the two prose subcommands hand that same binary a plan or a path list.

docs/guides/prose.md is the prose loop, and docs/guides/building-and-testing.md the gates.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools import dev

from . import args as a
from .context import Context

NAME = "lint"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run the linters, apply a prose plan, or measure prose")
    lint_sub = p.add_subparsers(dest="lint_cmd", required=True)

    ct = lint_sub.add_parser("clang-tidy", help="Run the clang-tidy whitelist gates (must be zero to commit)")
    a.preset(ct)
    a.profile(ct)
    a.change_scope(ct, default_all=True)
    ct.add_argument("--fix", action="store_true", help="Let clang-tidy apply its fixes in place")
    ct.add_argument("--limit", type=int, default=None, metavar="N",
                    help="Max diagnostic lines before switching to a grouped-by-check digest")

    sl = lint_sub.add_parser("shaped", help="Run shaped-linter — our own rules over code and prose (tools/shaped-linter)")
    a.preset(sl)
    a.profile(sl)
    a.change_scope(sl, default_all=True)
    sl.add_argument("--fix", action="store_true", help="Let shaped-linter apply its suggested fixes in place")

    pa = lint_sub.add_parser("prose-apply", help="Apply a prose plan — many comment/doc rewrites in one pass")
    a.preset(pa)
    a.profile(pa)
    pa.add_argument("plan", help="Plan file to apply (conventionally under .tmp/)")
    pa.add_argument("--dry-run", action="store_true", help="Validate the plan and report, but write nothing")
    pa.add_argument("--stats", action="store_true",
                    help="Also report the prose delta — lines and words, before and after, per file and total")

    ps = lint_sub.add_parser("prose-stats", help="Report how much prose files carry — lines and words, per file and total")
    a.preset(ps)
    a.profile(ps)
    ps.add_argument("paths", nargs="+", help="Files or directories to measure (a directory is walked for lintable sources)")

    bi = lint_sub.add_parser("bless-includes",
                             help="Fill each .shaped-lint.yml's generated block with the includes the tree still needs blessed")
    a.preset(bi)
    a.profile(bi)
    bi.add_argument("--write", action="store_true", help="Rewrite the generated blocks in place, rather than printing them")
    return p


def run_clang_tidy(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    scope: dev.ChangeScope | None,
    fix: bool,
    limit: int | None = None,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Run tools/lint/clang-tidy.py against a preset's compilation database; return True if clean.

    Ensures the preset is configured first, since the runner needs compile_commands.json.
    Shared by `lint clang-tidy` and the `check` lint gate.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale — guarantees compile_commands.json

    runner = ctx.root / "tools" / "lint" / "clang-tidy.py"
    argv = ["uv", "run", str(runner), "--build-dir", str(preset.build_dir)]
    if scope is not None:
        argv += ["--commit", scope.revision] if scope.revision else ["--dirty-only"]
    if fix:
        argv.append("--fix")
    if limit is not None:
        argv += ["--limit", str(limit)]

    # clang-tidy diagnostics are the point, so mirror the child live; run_step still captures the log.
    result = dev.run_step(
        argv, step_type="lint", name="clang-tidy",
        build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def run_shaped_linter(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    scope: dev.ChangeScope | None,
    fix: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and run it over first-party sources; return True if clean.

    It parses each file itself, so no compilation database is needed and headers are linted directly.
    Shared by `lint shaped` and the `check` shaped-lint gate.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale

    build_results = dev.build([preset], ["shaped-linter"], root=ctx.root, auto_configure=True,
                              mirror=mirror, verbose=verbose)
    if not all(r.ok for r in build_results):
        dev.report.print_build_failure(build_results, [preset], ctx.root)
        return False

    exe = next((t.artifact for t in ctx.discover(preset)
                if t.name == "shaped-linter" and t.kind == "EXECUTABLE" and t.artifact), None)
    if exe is None:
        print(dev.console.red("shaped-linter: could not resolve the built executable"), file=sys.stderr)
        return False

    # A wider scope than clang-format's, prose included, so .md and .py are in — tools/dev/lib/quality/format.py owns it.
    files = dev.discover_lint_files(ctx.root, scope=scope)
    if not files:
        print(dev.console.green("shaped-linter: nothing to lint (no sources in scope)"), file=sys.stderr)
        return True

    # A scoped run is line-exact for prose, and docs/guides/prose.md says why that matters.
    # A spec file rather than argv, since the run already batches to stay under the command-line limit.
    changed_lines_spec: Path | None = None
    if scope is not None:
        spec = dev.format_changed_line_spec(dev.changed_line_ranges(ctx.root, scope))
        if spec:
            changed_lines_spec = preset.build_dir / "shaped-linter-changed-lines.txt"
            changed_lines_spec.parent.mkdir(parents=True, exist_ok=True)
            changed_lines_spec.write_text(spec, encoding="utf-8")

    # Batch to stay well under the OS command-line length limit on a whole-tree run.
    ok = True
    for start in range(0, len(files), 200):
        batch = files[start:start + 200]
        # The linter's own `auto` sees a pipe (run_step captures it), so it would drop color even while we mirror to a terminal.
        # Hand it dev.py's already-resolved decision instead.
        argv = [str(exe), "--color", "always" if dev.console.enabled() else "never"]
        if fix:
            argv.append("--fix")
        if changed_lines_spec is not None:
            argv += ["--changed-lines", str(changed_lines_spec)]
        argv += [str(f) for f in batch]
        result = dev.run_step(
            argv, step_type="lint", name="shaped-linter",
            build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
        )
        ok = ok and result.ok
    return ok


def run_prose_apply(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    plan: str,
    dry_run: bool,
    stats: bool = False,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and hand it a prose plan; return True if the whole plan applied (or validated).

    Applying is all-or-nothing across every file in the plan.
    tools/shaped-linter/readme.md has the plan grammar and the two validations; docs/guides/prose.md says when to reach for this at all.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale

    plan_path = Path(plan)
    if not plan_path.is_absolute():
        plan_path = Path.cwd() / plan_path
    plan_path = plan_path.resolve()
    if not plan_path.is_file():
        ctx.die(f"no such plan file: {plan}")

    build_results = dev.build([preset], ["shaped-linter"], root=ctx.root, auto_configure=True,
                              mirror=mirror, verbose=verbose)
    if not all(r.ok for r in build_results):
        dev.report.print_build_failure(build_results, [preset], ctx.root)
        return False

    exe = next((t.artifact for t in ctx.discover(preset)
                if t.name == "shaped-linter" and t.kind == "EXECUTABLE" and t.artifact), None)
    if exe is None:
        print(dev.console.red("shaped-linter: could not resolve the built executable"), file=sys.stderr)
        return False

    argv = [str(exe), "prose", "apply", "--color", "always" if dev.console.enabled() else "never"]
    if dry_run:
        argv.append("--dry-run")
    if stats:
        argv.append("--stats")
    argv.append(str(plan_path))

    # Plan paths are repo-relative, so the linter must run from the repo root.
    result = dev.run_step(
        argv, step_type="lint", name="prose-apply",
        build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def run_prose_stats(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    paths: list[str],
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and report how much prose the given files carry; return True if it ran.

    Extracted prose only, so a `///` marker and the code around it never register.
    The same measure `prose-apply --stats` reports a delta in, which is what makes a rework's budget checkable against the number it was set from.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale

    files = dev.expand_lint_paths(ctx.root, paths)
    if not files:
        ctx.die(f"no lintable files under {' '.join(paths)}")

    build_results = dev.build([preset], ["shaped-linter"], root=ctx.root, auto_configure=True,
                              mirror=mirror, verbose=verbose)
    if not all(r.ok for r in build_results):
        dev.report.print_build_failure(build_results, [preset], ctx.root)
        return False

    exe = next((t.artifact for t in ctx.discover(preset)
                if t.name == "shaped-linter" and t.kind == "EXECUTABLE" and t.artifact), None)
    if exe is None:
        print(dev.console.red("shaped-linter: could not resolve the built executable"), file=sys.stderr)
        return False

    # Repo-relative, so the table reads like the plan paths do; the linter runs from the root either way.
    argv = [str(exe), "prose", "stats", "--color", "always" if dev.console.enabled() else "never"]
    argv += [str(f.relative_to(ctx.root)) if f.is_relative_to(ctx.root) else str(f) for f in files]

    result = dev.run_step(
        argv, step_type="lint", name="prose-stats",
        build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def run_bless_includes(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    write: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and fill every .shaped-lint.yml's generated block; return True if it ran.

    The scan is repo-wide by design.
    A blessing is a claim about a whole library, so a dirty-only baseline would bless whatever this commit happened to touch and no more.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale

    build_results = dev.build([preset], ["shaped-linter"], root=ctx.root, auto_configure=True,
                              mirror=mirror, verbose=verbose)
    if not all(r.ok for r in build_results):
        dev.report.print_build_failure(build_results, [preset], ctx.root)
        return False

    exe = next((t.artifact for t in ctx.discover(preset)
                if t.name == "shaped-linter" and t.kind == "EXECUTABLE" and t.artifact), None)
    if exe is None:
        print(dev.console.red("shaped-linter: could not resolve the built executable"), file=sys.stderr)
        return False

    files = dev.discover_lint_files(ctx.root, scope=None)

    # One invocation, unlike `lint shaped`: a config's block is written from everything below it at once,
    # and a batched run would rewrite each block from one batch's share of the files.
    # That many paths do not fit on a command line, so they go in a spec file instead.
    spec = preset.build_dir / "shaped-linter-bless-files.txt"
    spec.parent.mkdir(parents=True, exist_ok=True)
    spec.write_text("\n".join(str(f) for f in files) + "\n", encoding="utf-8")

    argv = [str(exe), "bless-includes", "--color", "always" if dev.console.enabled() else "never",
            "--files-from", str(spec)]
    if write:
        argv.append("--write")

    result = dev.run_step(
        argv, step_type="lint", name="bless-includes",
        build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def _scope(args: argparse.Namespace, ctx: Context) -> dev.ChangeScope | None:
    """The requested scope, with a bad revision reported before anything is built."""
    scope = a.scope_from_args(args)
    if scope is not None and not scope.is_working_tree:
        try:
            dev.resolve_range(ctx.root, scope.revision)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))
    return scope


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.lint_cmd:
        case "clang-tidy":
            ok = run_clang_tidy(
                ctx, preset_specs=args.preset, scope=_scope(args, ctx), fix=args.fix, limit=args.limit,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "shaped":
            ok = run_shaped_linter(
                ctx, preset_specs=args.preset, scope=_scope(args, ctx), fix=args.fix,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "prose-apply":
            ok = run_prose_apply(
                ctx, preset_specs=args.preset, plan=args.plan, dry_run=args.dry_run, stats=args.stats,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "prose-stats":
            ok = run_prose_stats(
                ctx, preset_specs=args.preset, paths=args.paths,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "bless-includes":
            ok = run_bless_includes(
                ctx, preset_specs=args.preset, write=args.write,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown lint subcommand {args.lint_cmd!r}")
