"""`lint` — the entry point for shaped-core's linting framework.

Thin wiring: each linter lives under its own tool directory and this command shells out to it.
`clang-tidy` runs the whitelist gates (tools/lint/clang-tidy.py), `shaped` runs our own parser-based rules
(tools/shaped-linter), and `prose-apply` hands that same binary a plan of prose rewrites to apply at once.
The clang-tidy gates are a strict whitelist that must be
zero before a commit — distinct from the root .clang-tidy, which is the broader IDE incubator.

`check` reuses `run_clang_tidy` and `run_shaped_linter` (dirty-only) as pre-commit gates — see
tools/dev/cmd/check.py.
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
    p = sub.add_parser(NAME, help="Run the linters (clang-tidy gates; shaped-linter; more to come)")
    lint_sub = p.add_subparsers(dest="lint_cmd", required=True)

    ct = lint_sub.add_parser("clang-tidy", help="Run the clang-tidy whitelist gates (must be zero to commit)")
    a.preset(ct)
    ct.add_argument("--dirty-only", action="store_true",
                    help="Only lint git-dirty/untracked .cc sources (the next commit's files)")
    ct.add_argument("--fix", action="store_true", help="Let clang-tidy apply its fixes in place")
    ct.add_argument("--limit", type=int, default=None, metavar="N",
                    help="Max diagnostic lines before switching to a grouped-by-check digest (default: 200)")

    sl = lint_sub.add_parser("shaped", help="Run shaped-linter — our own C++ parser-based rules (tools/shaped-linter)")
    a.preset(sl)
    sl.add_argument("--dirty-only", action="store_true",
                    help="Only lint git-dirty/untracked .cc/.hh sources (the next commit's files)")
    sl.add_argument("--fix", action="store_true", help="Let shaped-linter apply its suggested fixes in place")

    pa = lint_sub.add_parser("prose-apply", help="Apply a prose plan — many comment/doc rewrites in one pass")
    a.preset(pa)
    pa.add_argument("plan", help="Plan file to apply (conventionally under .tmp/)")
    pa.add_argument("--dry-run", action="store_true", help="Validate the plan and report, but write nothing")
    return p


def run_clang_tidy(
    ctx: Context,
    *,
    preset_specs: list[str] | None,
    dirty_only: bool,
    fix: bool,
    limit: int | None = None,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Run tools/lint/clang-tidy.py against a preset's compilation database; return True if clean.

    Resolves the preset and ensures it is configured (so compile_commands.json exists), then shells out
    to the standalone runner.
    Shared by `lint clang-tidy` and the `check` lint gate.
    """
    preset = ctx.resolve_presets(preset_specs)[0]
    ctx.discover(preset)  # (re)configure if stale — guarantees compile_commands.json

    runner = ctx.root / "tools" / "lint" / "clang-tidy.py"
    argv = ["uv", "run", str(runner), "--build-dir", str(preset.build_dir)]
    if dirty_only:
        argv.append("--dirty-only")
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
    dirty_only: bool,
    fix: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and run it over first-party C++ sources; return True if clean.

    shaped-linter is our own parser-based linter (tools/shaped-linter) — the custom-rules sibling of the
    clang-tidy gates.
    It parses each file itself, so it lints headers directly (no compilation database).
    Dirty-only in `check` so a not-yet-clean tree adopts it incrementally, exactly like the clang-tidy
    gates.
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

    # Its own scope, wider than clang-format's: it lints prose too, so .md and .py are in and so is docs/.
    files = dev.discover_lint_files(ctx.root, dirty_only=dirty_only)
    if not files:
        print(dev.console.green("shaped-linter: nothing to lint (no sources in scope)"), file=sys.stderr)
        return True

    # Dirty-only is line-exact for prose: editing one section of a long file must not drag that file's older, unrelated prose into the gate.
    # A spec file rather than argv, since the run already batches to stay under the command-line limit.
    changed_lines_spec: Path | None = None
    if dirty_only:
        spec = dev.format_changed_line_spec(dev.changed_line_ranges(ctx.root))
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
    mirror: bool = False,
    verbose: bool = False,
) -> bool:
    """Build shaped-linter and hand it a prose plan; return True if the whole plan applied (or validated).

    The plan names line spans across many files and the prose to put there, so a surface-wide rework of
    comments and docs lands in ONE invocation instead of hundreds of edits.
    Applying is all-or-nothing, and the linter rejects any span whose edit changed code rather than prose.
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
    argv.append(str(plan_path))

    # Plan paths are repo-relative, so the linter must run from the repo root.
    result = dev.run_step(
        argv, step_type="lint", name="prose-apply",
        build_dir=preset.build_dir, cwd=ctx.root, mirror=True, verbose=verbose,
    )
    return result.ok


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.lint_cmd:
        case "clang-tidy":
            ok = run_clang_tidy(
                ctx, preset_specs=args.preset, dirty_only=args.dirty_only, fix=args.fix, limit=args.limit,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "shaped":
            ok = run_shaped_linter(
                ctx, preset_specs=args.preset, dirty_only=args.dirty_only, fix=args.fix,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case "prose-apply":
            ok = run_prose_apply(
                ctx, preset_specs=args.preset, plan=args.plan, dry_run=args.dry_run,
                mirror=args.mirror_output, verbose=args.verbose,
            )
            raise SystemExit(0 if ok else 1)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown lint subcommand {args.lint_cmd!r}")
