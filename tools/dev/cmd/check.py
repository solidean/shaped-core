"""`check` — the project's pre-commit registry: which gates exist, and in what order they run.

The generic `Check` type and the runner are tools/dev/lib/quality/checks.py; this file is the list, and the growth point for a new gate.
docs/guides/building-and-testing.md documents what each gate does, and the argument behind the ordering.
"""

from __future__ import annotations

import argparse
import platform
import sys
from pathlib import Path

from tools import dev
from tools.dev import Watch, console
from tools.dev.lib.pipeline.eligibility import nexus_filter, query_listing
from tools.dev.lib.toolchain import jsruntime as jsr

from .test import THOROUGH_TIMEOUT_FACTOR

from . import args as a
from . import issues
from .context import Context

NAME = "check"

# What one preset's whole test run is expected to stay inside.
#
# 120 s, which every preset currently clears — so a green run is green rather than green-with-a-warning.
# The suite is slower than it should be and that is tracked separately; this budget is the ceiling a run must not
# cross, not the target it should hit.
# Tighten it as the suite gets faster; raising it to silence a warning is how this stops meaning anything.
_SLOW_TEST_BUDGET_S = 120.0


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run the pre-commit checks, in registry order (lint, format, crossrefs, test)")
    a.profile(p)
    p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    p.add_argument("--fix", action="store_true",
                   help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    a.change_scope(p, default_all=False)
    p.add_argument("--no-test", action="store_true",
                   help="Skip the test suite (build + run); just the static checks")
    p.add_argument("--list", action="store_true", help="List registered checks and exit")
    return p


def _branch_base(ctx: Context) -> str | None:
    """The merge base with the integration branch (`Policy.default_branch`), or None when there is none.

    On the local integration branch only its `origin/` counterpart is a base: the unpushed commits plus the working tree, what a push sends.
    Comparing the local branch with itself would inspect nothing, so it is never a candidate there.
    """
    import subprocess

    branch = ctx.policy.default_branch
    try:
        head = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=str(ctx.root),
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    on_branch = head.returncode == 0 and head.stdout.strip() == branch

    for base in (f"origin/{branch}",) if on_branch else (f"origin/{branch}", branch):
        try:
            out = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=str(ctx.root),
                                 capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    return None


def _build_checks(ctx: Context, *, memo: dev.CheckMemo | None = None) -> list[dev.Check]:
    """The pre-commit registry: project policy for which gates exist.

    `memo` lets the test gate skip a binary that passed before on an identical artifact, and record each pass.
    """

    def clang_tidy_stamp() -> str:
        import subprocess as sp
        exe = dev.resolve_tool("clang-tidy", "CLANG_TIDY", ctx.root / "build")
        if not exe:
            return ""
        try:
            return sp.run([str(exe), "--version"], capture_output=True, text=True, timeout=20).stdout.strip()
        except (OSError, sp.SubprocessError):
            return ""

    def clang_format_stamp() -> str:
        from tools.dev.lib.quality.format import clang_format_version, find_clang_format
        exe = find_clang_format()
        return f"{exe} {clang_format_version(exe)}" if exe else ""

    def check_format(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # --fix rewrites in place; without it clang-format only reports.
        try:
            result = dev.run_format(
                ctx.root,
                check=not fix,
                scope=scope,
                allow_different_version=False,
                layout=ctx.policy.layout,
                mirror=mirror,
                verbose=verbose,
            )
        except dev.FormatSetupError as e:
            ctx.die(str(e))
        return dev.report.summarize_format(result, ctx.root)

    def check_lint(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        from .lint import run_clang_tidy
        return run_clang_tidy(
            ctx, preset_specs=None, scope=scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_shaped_lint(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        from .lint import run_shaped_linter
        return run_shaped_linter(
            ctx, preset_specs=None, scope=scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_crossrefs(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # A moved file breaks links in other, untouched files, so this is always repo-wide: fix and scope are both ignored.
        return dev.report.summarize_crossrefs(dev.check_crossrefs(ctx.root, layout=ctx.policy.layout), ctx.root)

    def check_deps_licenses(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # A dependency's license is a property of the manifest set, not of the next commit's files, so this is repo-wide too.
        # `--check` writes nothing and reaches no network, which is what makes it cheap enough to sit here; fix and scope are ignored.
        from .deps import run_licenses

        return run_licenses(ctx, check=True, verbose=verbose)

    def check_review(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The review tool's own suite: coverage math, change identity and the entry grammar.
        # It builds throwaway git repositories and reaches no network, so it is cheap enough to sit here.
        # Not fixable and not scopable, so fix and scope are ignored.
        runner = ctx.root / "tools" / "review" / "review-self-test.py"
        result = dev.run_step(
            ["uv", "run", str(runner)],
            step_type="review", name="review-self-test",
            build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
        )
        return result.ok

    def check_issues(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # Not fixable and not scopable: a file deleted anywhere can break a ref in an untouched issue.
        return issues.gate(ctx, mirror=mirror, verbose=verbose)

    def check_issues_selftest(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The tracker's own suite: claims against a real bare remote, check, known and close, in throwaway repositories.
        result = dev.run_step(
            ["uv", "run", str(ctx.root / "tools" / "issues" / "issues-self-test.py")],
            step_type="issues", name="issues-self-test",
            build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
        )
        return result.ok

    def check_dev_selftest(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # dev.py's own machinery, which nothing else exercises: the job profile's layout, the change-scope resolver, and
        # the live progress region.
        # The region is the one that most needs it — it exists only as escape sequences on a stream, so a frame that
        # miscounts its own height has no other way of being caught.
        # extern/'s manifest resolution runs here for the same reason: which upstream is available on which machine is
        # answered on every configure and reported by nothing, so a wrong answer surfaces as a link error on one runner.
        # Not fixable and not scopable, so fix and scope are ignored.
        # clean-simd's link-map check runs after every link of a dispatching binary, where a parser that silently stopped
        # matching would look exactly like a clean link.
        ok = True
        runners = (
            (Path("tools") / "dev" / "profile-self-test.py", []),
            (Path("tools") / "dev" / "changes-self-test.py", []),
            (Path("tools") / "dev" / "roots-self-test.py", []),
            (Path("tools") / "dev" / "jsruntime-self-test.py", []),
            (Path("tools") / "dev" / "ui-self-test.py", []),
            (Path("tools") / "dev" / "symbolize-self-test.py", []),
            (Path("tools") / "dev" / "submodules-self-test.py", []),
            (Path("extern") / "manifest-self-test.py", []),
            (Path("libs") / "base" / "clean-simd" / "tools" / "check-link-map.py", ["--self-test"]),
        )
        for runner, args in runners:
            result = dev.run_step(
                ["uv", "run", str(ctx.root / runner), *args],
                step_type="selftest", name=runner.name.removesuffix(".py"),
                build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
            )
            ok = ok and result.ok
        return ok

    def check_shader_grammar(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The binding pass exists twice -- in C++ for the runtime rewriter, in Python for the shader-package
        # generator -- and one shared corpus is what keeps the two agreeing.
        #
        # BOTH halves run here, since a gate that ran only one would let a divergence through and
        # leave it to the test suite, and the suite is the thing this gate runs ahead of.
        # The C++ half runs shaped-shader-library-test, which the gate declares in `builds` and the runner built ahead.
        # Not fixable and not scopable, so fix and scope are ignored.
        runner = ctx.root / "libs" / "graphics" / "shaped-shader-library" / "cmake" / "binding-grammar-self-test.py"
        result = dev.run_step(
            ["uv", "run", str(runner)],
            step_type="lint", name="binding-grammar-self-test",
            build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
        )
        if not result.ok:
            return False

        # The host code SGL groups generate, fed describe-shaped input directly, so every arm runs without a package.
        result = dev.run_step(
            ["uv", "run", str(runner.with_name("sgl-host-code-self-test.py"))],
            step_type="lint", name="sgl-host-code-self-test",
            build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
        )
        if not result.ok:
            return False

        presets = ctx.resolve_presets([ctx.default_preset_name()])
        records = dev.test(
            presets, ["shaped-shader-library-test"], root=ctx.root,
            test_name="the binding corpus parses as it says it does",
            timeout=60.0, write_xml=True, mirror=mirror, verbose=verbose,
        )

        # This gate names ONE test in one binary, and a runner told to filter on a name that matches nothing
        # exits 0 -- so the step passes having run nothing, and renaming that TEST turns the gate silently green.
        # Count the cases rather than trust the exit code.
        ran = sum(r["junit"]["tests"] for r in records if r.get("junit"))
        if ran == 0:
            dev.ui.write_line(console.red(
                "shader-grammar: the corpus test did not run -- this gate selects it by name, so zero cases "
                "means it was renamed or removed, not that it passed"))
            return False

        return dev.report.summarize_tests(records, presets, ctx.root)

    def check_sgl_prelude(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # SGL's builtins live in a C++ registry, and prelude/builtins.sgl is that registry written out and committed.
        # slib's impl/pipeline_fields.hh is the prelude's mirror of sg's raster pipeline description, written out the same way.
        # The `sgl` tool compares each pair, so this gate declares it in `builds` and the runner built it ahead.
        # --fix rewrites the file instead.
        # That fixer stands behind `format` and breaks nothing by it: what it writes is SGL, which no other gate reads,
        # so the ordering argument about fixers and `format` does not reach it.
        # Repo-wide by nature, so scope is ignored.
        prelude = ctx.root / "libs" / "graphics" / "shaped-graphics-language" / "prelude" / "builtins.sgl"
        fields = (ctx.root / "libs" / "graphics" / "shaped-shader-library" / "src" / "shaped-shader-library" / "impl"
                  / "pipeline_fields.hh")
        generated = [("prelude", prelude, "the builtin registry"),
                     ("pipeline-fields", fields, "the prelude's mirror of sg's raster pipeline description")]
        presets = ctx.resolve_presets([ctx.default_preset_name()])
        preset = presets[0]

        # The tool is gated on SC_BUILD_TOOLS, and a wasm artifact is no program this gate can simply start.
        # The library's own test pins the same two texts, so a tree that skips here still catches drift in `test`.
        has_tool = any(t.name == "sgl" and t.kind == "EXECUTABLE" for t in ctx.discover(preset))
        if not has_tool or preset.is_emscripten:
            reason = "is an emscripten preset" if has_tool else "has no `sgl` target (SC_BUILD_TOOLS is off)"
            dev.ui.write_line(f"sgl-prelude: skipped -- preset {preset.name!r} {reason}; "
                              f"the shaped-graphics-language tests compare the same texts")
            return True

        artifact = next((t.artifact for t in ctx.discover(preset) if t.name == "sgl" and t.artifact), None)
        if artifact is None:
            dev.ui.write_line(console.red(f"sgl-prelude: target 'sgl' has no built artifact for preset {preset.name!r}"))
            return False

        ok = True
        for command, path, source in generated:
            result = dev.run_step(
                [str(artifact), command, "--write" if fix else "--check", str(path)],
                step_type="lint", name=f"sgl-{command}",
                build_dir=preset.build_dir, cwd=ctx.root, mirror=mirror, verbose=verbose,
            )
            rel = path.relative_to(ctx.root).as_posix()
            if result.ok:
                dev.ui.write_line(f"sgl-prelude: {rel} {'written from' if fix else 'is in sync with'} {source}")
                continue

            # The tool's own message says where the texts part; it was captured to the step log, so it is repeated here.
            for log in (result.stdout_log, result.stderr_log):
                text = log.read_text(encoding="utf-8", errors="replace").rstrip() if log.exists() else ""
                if text and not mirror:
                    dev.ui.write_line(text)
            dev.ui.write_line(console.red(f"sgl-prelude: run `uv run dev.py check sgl-prelude --fix` to regenerate {rel}"))
            ok = False
        return ok

    def check_sgl_vscode_bundle(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The SGL VS Code extension commits its esbuild bundle, so users link the folder without an `npm install`.
        # This gate rebuilds the bundle from the pinned lockfile and compares bytes; --fix copies the fresh one over.
        # Its `watches` are the bundle's inputs, since `npm ci` is slow and nothing else feeds them.
        import os
        import shutil
        import tempfile
        from pathlib import Path

        extension = ctx.root / "libs" / "graphics" / "shaped-graphics-language" / "tools" / "vscode-extension"
        committed = extension / "dist" / "extension.js"

        # emsdk bundles a node and its npm, which is how a machine without a system node still runs this gate.
        # `npm.cmd` by name on Windows: the extensionless `npm` beside it is a shell script CreateProcess cannot start.
        env = dev.emsdk_env() or dict(os.environ)
        npm = shutil.which("npm.cmd" if os.name == "nt" else "npm", path=env.get("PATH") or env.get("Path"))
        if npm is None:
            dev.ui.write_line("sgl-vscode-bundle: skipped -- npm not found (install node, or emsdk)")
            return True

        result = dev.run_step(
            [npm, "ci", "--no-audit", "--no-fund"],
            step_type="lint", name="sgl-vscode-npm-ci",
            build_dir=ctx.root / "build", cwd=extension, env=env, mirror=mirror, verbose=verbose,
        )
        if not result.ok:
            return False

        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "extension.js"
            # The same flags as package.json's `bundle` script.
            result = dev.run_step(
                [npm, "exec", "--", "esbuild", "src/extension.js", "--bundle", "--platform=node", "--format=cjs",
                 "--external:vscode", "--minify", f"--outfile={fresh}"],
                step_type="lint", name="sgl-vscode-bundle",
                build_dir=ctx.root / "build", cwd=extension, env=env, mirror=mirror, verbose=verbose,
            )
            if not result.ok:
                return False

            rel = committed.relative_to(ctx.root).as_posix()
            if committed.is_file() and committed.read_bytes() == fresh.read_bytes():
                dev.ui.write_line(f"sgl-vscode-bundle: {rel} is in sync with its sources")
                return True
            if fix:
                committed.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(fresh, committed)
                dev.ui.write_line(f"sgl-vscode-bundle: rebuilt {rel}")
                return True
        dev.ui.write_line(console.red(f"sgl-vscode-bundle: {rel} is stale -- run `uv run dev.py check --fix` to rebuild it"))
        return False

    def committed_codegen(name: str, script: Path):
        # A library that commits what its generator writes: the generator compares (or, under --fix, rewrites) them
        # itself, and formats with the repo's clang-format, so it runs before `format` and what it writes is already
        # what `format` would leave.
        def run(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
            from tools.dev.lib.quality.format import find_clang_format

            clang_format = find_clang_format()
            if clang_format is None:
                dev.ui.write_line(f"{name}: skipped -- clang-format not found")
                return True
            result = dev.run_step(
                [sys.executable, str(ctx.root / script), "--write" if fix else "--check", "--clang-format", clang_format],
                step_type="lint", name=name,
                build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
            )
            if not result.ok:
                dev.ui.write_line(console.red(f"{name}: the committed files differ from the generator's output "
                                              "-- run `uv run dev.py check --fix`"))
            return result.ok

        return run

    def check_tests(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The variants come from dev.py's Policy tables, and a platform with no sibling for one of them simply contributes none.
        # Not fixable, so fix and scope are ignored.
        system = platform.system()
        specs = [ctx.default_preset_name()]
        for sibling in (
            ctx.policy.default_debug.get(system),
            ctx.policy.default_release.get(system),
            ctx.policy.default_singlethreaded.get(system),
            ctx.policy.default_sanitize.get(system),
            ctx.policy.default_sanitize_thread.get(system),
        ):
            if sibling:
                specs.append(sibling)
        presets = ctx.resolve_presets(specs)

        results = dev.build(presets, None, root=ctx.root, auto_configure=True, mirror=mirror, verbose=verbose)
        if not all(r.ok for r in results):
            dev.report.print_build_failure(results, presets, ctx.root)
            return False

        test_targets = [t for t in ctx.discover(presets[0]) if ctx.is_test_target(t)]
        if not test_targets:
            print(console.red("No test binaries found (expected '*-test' executables)"), file=sys.stderr)
            return False

        # Everything is built above, so a compile break anywhere still fails here; the selection is what RUNS.
        candidates = [t.name for t in test_targets]
        names = _select_test_binaries(ctx, presets[0], scope, candidates)
        thorough = _thorough_triggers(ctx, presets[0], scope, candidates)
        if not names and not thorough:
            dev.ui.write_line("test: no test binary can be affected by this change")
            return True

        # Per preset, since the memo answers per artifact: a binary that passed before on identical bytes is skipped
        # on that preset alone, and a pass or failure is recorded the same way.
        records: list[dict] = []
        for preset in presets:
            to_run = list(names)
            artifacts: dict[str, Path] = {}
            if memo is not None and names:
                artifacts = {t.name: t.artifact for t in ctx.discover(preset) if t.name in names and t.artifact}
                done = memo.tests_passed_before(preset.name, artifacts.items())
                if done:
                    dev.ui.write_line(console.dim(
                        f"test: {preset.name}: {len(done)} of {len(names)} binaries passed before on identical artifacts"))
                to_run = [n for n in names if n not in done]
            if not to_run:
                continue
            preset_records = dev.test(
                [preset], to_run, root=ctx.root,
                test_name=None, timeout=60.0, write_xml=True, mirror=mirror, verbose=verbose,
            )
            records += preset_records
            if memo is not None:
                memo.record_tests(
                    preset.name,
                    [(r["name"], artifacts[r["name"]]) for r in preset_records if r["returncode"] == 0 and r["name"] in artifacts],
                    [r["name"] for r in preset_records if r["returncode"] != 0],
                )
        ok = dev.report.summarize_tests(records, presets, ctx.root, table=False) if names else True

        # The thorough tests the change declared itself about, at full strength and on the default preset alone.
        # Counted rather than trusted: a filter that matches nothing exits 0, and a renamed test would otherwise pass unrun.
        for binary, tests in thorough.items():
            dev.ui.write_line(console.dim(f"test: {binary} --thorough for {len(tests)} test(s) that rerun on this change"))
            thorough_records = dev.test(
                presets[:1], [binary], root=ctx.root, test_name=None,
                extra_args=["--thorough", *(nexus_filter(t) for t in tests)],
                timeout=60.0 * THOROUGH_TIMEOUT_FACTOR, write_xml=True, mirror=mirror, verbose=verbose,
            )
            ran = sum(r["junit"]["tests"] for r in thorough_records if r.get("junit"))
            if ran < len(tests):
                dev.ui.write_line(console.red(f"test: {binary} ran {ran} of the {len(tests)} thorough test(s) selected by name"))
                ok = False
            ok = dev.report.summarize_tests(thorough_records, presets[:1], ctx.root, table=False) and ok
            records += thorough_records

        # After the verdict, and never part of it: this is a measurement of the run that just happened.
        # `check` is the longest command anyone here runs, and a single total cannot say which preset or which half.
        dev.report.summarize_check_timing(presets, results, records, slow_test_s=_SLOW_TEST_BUDGET_S)
        return ok

    # THE ORDER IS FIXED among the fixers: under --fix they run in this order, after the read-only gates, so what
    # they rewrite is formatted in the same pass.
    # Every gate says what it is about: `watches` are the globs a change must touch for it to run, `always` is the
    # explicit opt-out of selection for a gate whose inputs are the whole tree.
    # docs/guides/building-and-testing.md has the argument.
    cxx = ("**/*.cc", "**/*.hh")
    return [
        # The build files too: a flag change relints, cheaply, since the scope then holds no .cc of its own.
        dev.Check("lint", "clang-tidy gates on what this branch changed (--dirty-only, --commit or --all to rescope)",
                  True, check_lint,
                  watches=(*cxx, "tools/lint/**", "**/.clang-tidy", "**/CMakeLists.txt", "**/*.cmake", "CMakePresets.json"),
                  stamp=clang_tidy_stamp),
        dev.Check("shaped-lint", "shaped-linter's own rules on what this branch changed in code and prose "
                                 "(--dirty-only, --commit or --all to rescope)",
                  True, check_shaped_lint,
                  watches=(*cxx, "**/*.md", "**/*.py", "**/.shaped-lint.yml", "tools/shaped-linter/**")),
        dev.Check("sgl-vscode-bundle",
                  "the SGL VS Code extension's committed dist/extension.js is what esbuild makes of its sources "
                  "(--fix rebuilds it)",
                  True, check_sgl_vscode_bundle,
                  watches=("libs/graphics/shaped-graphics-language/tools/vscode-extension/**",)),
        dev.Check("fixed-int-gen",
                  "typed-geometry's generated fixed_int headers and golden tests are what gen-fixed-int.py writes "
                  "(--fix regenerates them)",
                  True, committed_codegen("fixed-int-gen", Path("libs/base/typed-geometry/tools/gen-fixed-int.py")),
                  watches=("libs/base/typed-geometry/**",)),
        dev.Check("simd-gen",
                  "clean-simd's generated register and fixed layers are what gen-simd.py writes (--fix regenerates them)",
                  True, committed_codegen("simd-gen", Path("libs/base/clean-simd/tools/gen-simd.py")),
                  watches=("libs/base/clean-simd/**",)),
        dev.Check("format", "clang-format our C++ sources, last so it formats what the linters fixed "
                            "(--dirty-only, --commit or --all to rescope)",
                  True, check_format,
                  watches=(*cxx, "**/.clang-format"), stamp=clang_format_stamp),
        # A moved or deleted file breaks links in files the change never touched, so no glob can narrow this.
        dev.Check("crossrefs", "validate doc<->code cross-references repo-wide", False, check_crossrefs, always=True),
        dev.Check("deps-licenses", "verify docs/licenses/ matches the extern/ manifests, and each license is on the allowlist",
                  False, check_deps_licenses,
                  watches=("extern/**", "docs/licenses/**", "tools/dev/**")),
        dev.Check("dev-selftest", "dev.py's own self-tests (job profile, change scope, progress region, dependency manifests, "
                                  "clean-simd's link-map check)",
                  False, check_dev_selftest,
                  watches=("dev.py", "tools/dev/**", "extern/**", "libs/base/clean-simd/tools/**")),
        # An issue marker can sit in any source file, and a deleted file anywhere can break a ref in an untouched issue.
        dev.Check("issues", "every issue file and every issue marker in the code is valid (docs/guides/issue-tracking.md)",
                  False, check_issues, always=True),
        dev.Check("issues-selftest", "run the issue tracker's own suite (claims against a real remote, check, known, close)",
                  False, check_issues_selftest,
                  watches=("tools/issues/**", "tools/dev/cmd/issues.py")),
        dev.Check("review", "run the review tool's own suite (coverage math, change identity, the entry grammar)",
                  False, check_review,
                  watches=("tools/review/**", "review.py")),
        dev.Check("shader-grammar",
                  "run the shared binding corpus against both halves of the binding pass",
                  False, check_shader_grammar,
                  watches=("libs/graphics/shaped-shader-library/**",),
                  builds=("shaped-shader-library-test",)),
        dev.Check("sgl-prelude",
                  "SGL's prelude/builtins.sgl and slib's impl/pipeline_fields.hh are what `sgl` generates (--fix rewrites them)",
                  True, check_sgl_prelude,
                  watches=("libs/graphics/shaped-graphics-language/**", "libs/graphics/shaped-shader-library/**",
                           "libs/graphics/shaped-graphics/**"),
                  builds=("sgl",)),
        dev.Check("test",
                  "build + run the full suite on the debug, default, release, single-threaded "
                  "(and where supported, sanitizer) presets",
                  False, check_tests, requires_green=True, always=True),
    ]


# A change to one of these feeds every target, so it runs every test binary.
_BUILD_INPUTS = Watch.of([
    "**/CMakeLists.txt", "**/*.cmake", "CMakePresets.json", "tools/cmake/**",
    # The harness files that run the tests, as opposed to the rest of dev.py, which the dev-selftest gate covers.
    "tools/dev/lib/pipeline/test.py", "tools/dev/cmd/test.py", "tools/dev/cmd/check.py",
])

# A change to one of these is read by no test, so it runs none.
# A file under a library that is not listed here still counts as that library, however it is named.
_PROSE = Watch.of([
    "**/*.md", "docs/**", ".claude/**", ".github/**",
    "dev.py", "review.py", "tools/dev/**", "tools/review/**", "tools/issues/**", "tools/lint/**",
    "**/.shaped-lint.yml", "**/.clang-format", "**/.clang-tidy", ".clangd", ".gitignore", ".gitattributes", ".editorconfig",
])


def _thorough_triggers(ctx: Context, preset: dev.Preset, scope: dev.ChangeScope | None,
                       candidates: list[str]) -> dict[str, list[str]]:
    """Per binary, the tests whose `rerun_on` globs the change touched, from each binary's own listing.

    A glob matching no tracked file is an error: the file it named has moved, and the test would otherwise stop running without anyone noticing.
    A whole-tree scope selects none, since `--thorough` is the spelling for every thorough test.
    """
    if scope is None:
        return {}
    paths = dev.touched_paths(ctx.root, scope)
    tracked = dev.tracked_files(ctx.root)
    targets = {t.name: t for t in ctx.discover(preset)}
    launcher = jsr.LazyLauncher(None, None)
    chosen: dict[str, list[str]] = {}
    dead: list[str] = []
    for name in candidates:
        listing = query_listing(preset, targets[name], test_name=None, extra_args=[], root=ctx.root, launcher=launcher)
        if listing is None:
            continue
        for test in listing.tests:
            globs = list(test.get("rerun_on") or [])
            if not globs:
                continue
            watch = Watch.of(globs)
            dead += [f"  {name}: {test['name']!r}: {g!r}" for g in watch.dead_patterns(tracked)] if tracked else []
            if watch.touched_by(paths):
                chosen.setdefault(name, []).append(str(test["name"]))
    if dead:
        ctx.die("check: these rerun_on globs match no tracked file, so the rule is stale:\n" + "\n".join(dead))
    return chosen


def _select_test_binaries(ctx: Context, preset: dev.Preset, scope: dev.ChangeScope | None, candidates: list[str]) -> list[str]:
    """The test binaries the scope's change can affect, from the preset's target graph; all of them for a whole-tree scope.

    Reports what it skipped and, when it runs everything, which file made it.
    """
    if scope is None:
        return candidates
    paths = dev.touched_paths(ctx.root, scope)
    graph = dev.TargetGraph.load(preset.build_dir, preset.build_type, root=ctx.root)
    selection = dev.select_test_binaries_for_change(graph, paths, candidates, build_inputs=_BUILD_INPUTS, prose=_PROSE)
    if selection.everything:
        shown = "; ".join(selection.reasons[:3]) + (" ..." if len(selection.reasons) > 3 else "")
        dev.ui.write_line(console.dim(f"test: every binary runs: {shown}"))
    else:
        dev.ui.write_line(console.dim(
            f"test: {len(paths)} changed path(s) select {len(selection.binaries)} of {len(candidates)} test binaries"))
        for name in selection.skipped:
            dev.ui.write_line(console.dim(f"--- skipped {name} (the change cannot reach it) ---"))
    return selection.binaries


def _memo(ctx: Context, *, read: bool) -> dev.CheckMemo:
    return dev.CheckMemo(ctx.root, ctx.root / "build" / "check-memo.json", read=read)


def _run_gates(args: argparse.Namespace, ctx: Context, selected: list[dev.Check], scope: dev.ChangeScope | None,
               *, named: bool = False, memo: dev.CheckMemo | None = None) -> bool:
    """Select from `selected` by what `scope` touched, unless `named`, then run them and return the verdict."""
    ctx.warn_known_issues(ctx.resolve_presets(None))
    skipped: list[dev.Check] = []
    if not named:
        try:
            chosen, paths = dev.select_checks(selected, root=ctx.root, scope=scope)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))
        skipped = [c for c in selected if c not in chosen]
        selected = chosen
        if paths is not None:
            dev.ui.write_line(console.dim(
                f"check: {len(paths)} changed path(s) select {len(selected)} of {len(selected) + len(skipped)} gates"))

    return dev.run_checks(
        selected, fix=args.fix, scope=scope,
        mirror=args.mirror_output, verbose=args.verbose, no_test=args.no_test,
        build=_build_for_gates(ctx, mirror=args.mirror_output, verbose=args.verbose),
        skipped=skipped, memo=memo,
    )


def _build_for_gates(ctx: Context, *, mirror: bool, verbose: bool):
    """The one build the runner makes ahead of the gates, on the default preset.

    A target the preset does not configure (SC_BUILD_TOOLS off, say) is left out rather than failing the build; the gate that wanted it reports its own skip.
    """

    def build(targets: list[str]) -> bool:
        presets = ctx.resolve_presets([ctx.default_preset_name()])
        known = {t.name for t in ctx.discover(presets[0])}
        wanted = [t for t in targets if t in known]
        if not wanted:
            return True
        results = dev.build(presets, wanted, root=ctx.root, auto_configure=True, mirror=mirror, verbose=verbose)
        if not all(r.ok for r in results):
            dev.report.print_build_failure(results, presets, ctx.root)
            return False
        return True

    return build


def run(args: argparse.Namespace, ctx: Context) -> None:
    # A deliberate full run (--all) and a gate named on the command line bypass the memo's reads; both still record.
    memo = _memo(ctx, read=not (args.all or bool(args.names)))
    try:
        checks = _build_checks(ctx, memo=memo)
        dev.validate_checks(checks, root=ctx.root)
    except dev.CheckRegistryError as e:
        ctx.die(str(e))
    if args.list:
        dev.list_checks(checks)
        sys.exit(0)

    by_name = {c.name: c for c in checks}
    if args.names:
        for name in args.names:
            if name not in by_name:
                ctx.die(f"unknown check {name!r}. Available: {', '.join(by_name)}")
        # Registry order, not the order the names were typed.
        # The sequence is a correctness property, so `check format lint --fix` must not silently undo it.
        wanted = set(args.names)
        selected = [c for c in checks if c.name in wanted]
    else:
        selected = list(checks)

    # Default: everything changed against the default branch's merge base, working tree included.
    # There is no working-tree fallback: a gate scoped to dirty files empties as you commit, so the run right before
    # pushing would inspect nothing and report green.
    # Without a base the default has no meaning, and an explicit scope flag is required instead.
    explicit = args.since is not None or args.commit is not None or args.all or args.dirty_only
    base = None if explicit else _branch_base(ctx)
    if not explicit and base is None:
        ctx.die(f"check found no merge base with origin/{ctx.policy.default_branch} (or {ctx.policy.default_branch}, "
                "off it) to scope its gates to; "
                "fetch origin, or pass --since <rev>, --commit <rev>, --dirty-only or --all")
    scope = a.scope_from_args(args, default_since=base)
    if scope is not None and scope.revision is not None:
        # Fail on a bad revision here, rather than once a gate is already running.
        try:
            dev.resolve_range(ctx.root, scope.revision)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))
    if scope is not None and scope.since is not None:
        # Same, for the branch-base kind: changed_files raises on an unresolvable base, and a gate is a bad place to
        # discover that.
        try:
            dev.changed_files(ctx.root, scope)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))

    # A gate named on the command line runs whatever the change touched; the registry walk is what selection narrows.
    ok = _run_gates(args, ctx, selected, scope, named=bool(args.names), memo=memo)
    sys.exit(0 if ok else 1)
