#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""
Build & test CLI for shaped-core.

Usage:
    uv run dev.py configure                 Configure the CMake project
    uv run dev.py build [--target T]        Build (optionally specific targets)
    uv run dev.py test [--target T] [NAME]  Run tests (optionally a binary / test name)
    uv run dev.py test-web [LIBRARY]        Open the browser test runner (Emscripten; all libs, or one)
    uv run dev.py format [--dirty-only]     Format libs/ sources with clang-format
    uv run dev.py check [NAME...] [--fix]   Run pre-commit checks (format, crossrefs, test)
    uv run dev.py coverage run [NAME]       Collect LLVM test coverage (run/merge/report)
    uv run dev.py pgo run                    Profile-guided optimization (instrument/train/optimize/measure)
    uv run dev.py clean [--all]             Remove build artifacts
    uv run dev.py info build-flags TARGET   Show resolved compile/link flags (or compile-command FILE)
    uv run dev.py diagnose clangd FILE      Show clangd diagnostics for a source file
    uv run dev.py doctor                    Sanity-check the toolchain
    uv run dev.py list-presets              List available build presets
    uv run dev.py list-targets              List discovered targets

Presets and targets accept comma-lists, repeated flags, and shell-style
wildcards, and operate on as many as you select:
    --preset debug-clang,release-clang
    --preset "x64-linux-*" --preset debug-clang
    --target "*-test"

The default preset is chosen by platform but can be overridden with --preset.
'build' and 'test' auto-configure when cmake inputs or the source listing change
(fingerprinted); pass --no-configure to skip.

dev.py is quiet by default: child stdout/stderr is captured to
build/<preset>/run-logs/run-log-<name>.{stdout,stderr}.txt rather than
mirrored to the terminal — pass --mirror-output to stream it live. Each command
also writes a machine-readable sidecar (configure.json / build.json / test.json)
next to the build directory.

Output is colored (green/orange/red) when stdout and stderr are both a terminal,
and plain when either is piped (e.g. run by an agent). Force it either way with
--colored / --plain; in auto mode NO_COLOR / FORCE_COLOR are also honored.
"""

from __future__ import annotations

import argparse
import fnmatch
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from tools import dev  # noqa: E402
from tools.dev import console  # noqa: E402

# ---------------------------------------------------------------------------
# Project policy
# ---------------------------------------------------------------------------

# Default build preset per platform. Override with --preset.
DEFAULT_BUILD_PRESETS: dict[str, str] = {
    "Windows": "relwithdebinfo-clang",
    "Linux": "relwithdebinfo-linux-clang",
    "Darwin": "macos-arm-llvm-relwithdebinfo",
}

# Debug sibling of each default preset. Run by the `test` check alongside the
# others: -O0 plus mimalloc's debug heap (MI_DEBUG) catch bugs the optimized
# presets miss.
DEFAULT_DEBUG_PRESETS: dict[str, str] = {
    "Windows": "debug-clang",
    "Linux": "debug-linux-clang",
    "Darwin": "macos-arm-llvm-debug",
}

# Release sibling of each default preset, run alongside it by the `test` check so
# precommit exercises both CC_ASSERT on (debug/relwithdebinfo) and off (release).
DEFAULT_RELEASE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Sanitizer (ASan+UBSan) preset per platform, run by the `test` check as an extra
# defensive pass. Windows is intentionally absent: clang-cl's ASan is broken with
# C++ exceptions (any throw/catch faults during EH dispatch — a toolchain bug, not
# ours), and nexus catches test exceptions, so it can never be green there. The
# sanitize-clang preset still exists for manual non-throwing runs; see
# docs/guides/building-and-testing.md.
DEFAULT_SANITIZE_PRESETS: dict[str, str] = {
    "Linux": "sanitize-linux-clang",
    "Darwin": "sanitize-macos-arm-llvm",
}

# Default coverage preset per platform (instrumented Debug build; SC_COVERAGE ON).
# `coverage` uses these instead of DEFAULT_BUILD_PRESETS when no --preset is given.
COVERAGE_BUILD_PRESETS: dict[str, str] = {
    "Windows": "coverage-clang",
    "Linux": "coverage-linux-clang",
    "Darwin": "coverage-macos-arm-llvm",
}

# PGO presets per platform: the instrumented (-fprofile-generate) and optimized
# (-fprofile-use) Release builds, plus the clean Release baseline the speedup is
# measured against. `pgo` uses these when no --preset is given.
PGO_GENERATE_PRESETS: dict[str, str] = {
    "Windows": "pgo-generate-clang",
    "Linux": "pgo-generate-linux-clang",
    "Darwin": "pgo-generate-macos-arm-llvm",
}
PGO_USE_PRESETS: dict[str, str] = {
    "Windows": "pgo-use-clang",
    "Linux": "pgo-use-linux-clang",
    "Darwin": "pgo-use-macos-arm-llvm",
}
PGO_BASELINE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Default preset for `test-web` (the browser runner is Emscripten-only, regardless of host platform).
DEFAULT_WEB_PRESET = "emscripten-relwithdebinfo"


def _is_test_target(target: dev.Target) -> bool:
    """Project convention: test executables are named '*-test'."""
    return target.kind == "EXECUTABLE" and target.name.endswith("-test")


def die(msg: str) -> None:
    print(console.red(f"ERROR: {msg}"), file=sys.stderr)
    sys.exit(1)


def _rel(p: Path) -> str:
    """Path relative to the repo root (posix style) for compact hints."""
    return dev.report.rel(p, ROOT)


def _fail_build(results: list[dev.StepResult], presets: list[dev.Preset]) -> None:
    """Report a failed build phase and exit(1) (for the build/test commands)."""
    dev.report.print_build_failure(results, presets, ROOT)
    sys.exit(1)


def default_preset_name() -> str:
    name = DEFAULT_BUILD_PRESETS.get(platform.system())
    if name is None:
        die(f"No default preset for {platform.system()!r}. Use --preset.")
    return name


def resolve_presets(specs: list[str] | None) -> list[dev.Preset]:
    """Resolve --preset specs, falling back to the platform default."""
    try:
        presets = dev.resolve_presets(ROOT, specs or [])
        if not presets:
            presets = dev.resolve_presets(ROOT, [default_preset_name()])
        return presets
    except dev.PresetError as e:
        die(str(e))


def resolve_build_presets(args: argparse.Namespace) -> list[dev.Preset]:
    """Resolve --preset and apply the --toolset / --build-suffix / --build-dir overrides.

    Used by the configure/build/test commands; validates a pinned toolset eagerly so an
    unresolvable one fails fast with a clean message instead of mid-build.
    """
    presets = resolve_presets(args.preset)
    try:
        return dev.apply_overrides(
            presets, root=ROOT,
            toolset=args.toolset, build_suffix=args.build_suffix, build_dir=args.build_dir,
        )
    except dev.ToolsetError as e:
        die(str(e))


def _discover(preset: dev.Preset, emsdk_path: str | None = None) -> list[dev.Target]:
    """Discover targets for a preset, auto-configuring if needed."""
    try:
        return dev.discover_targets(preset.build_dir, preset.build_type)
    except dev.NotConfiguredError:
        result = dev.ensure_configured(preset, root=ROOT, emsdk_path=emsdk_path)
        if result is not None and not result.ok:
            die(f"Configure failed for {preset.name!r}")
        return dev.discover_targets(preset.build_dir, preset.build_type)


def resolve_target_names(
    preset: dev.Preset, specs: list[str] | None, emsdk_path: str | None = None
) -> list[str] | None:
    """Expand --target specs into concrete target names against a preset.

    Returns None when no specs were given (meaning 'build everything').
    """
    patterns: list[str] = []
    for spec in specs or []:
        patterns.extend(s.strip() for s in spec.split(",") if s.strip())
    if not patterns:
        return None

    available = _discover(preset, emsdk_path)
    names = [t.name for t in available]
    selected: list[str] = []
    seen: set[str] = set()
    for pat in patterns:
        matches = [pat] if pat in names else [n for n in names if fnmatch.fnmatch(n, pat)]
        if not matches:
            die(f"No target matches {pat!r}. Available: {', '.join(sorted(names))}")
        for n in matches:
            if n not in seen:
                seen.add(n)
                selected.append(n)
    return selected


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_configure(args: argparse.Namespace) -> None:
    presets = resolve_build_presets(args)
    results = dev.configure(
        presets, root=ROOT, force=True, mirror=args.mirror_output, verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )
    sys.exit(0 if all(r.ok for r in results) else 1)


def cmd_build(args: argparse.Namespace) -> None:
    presets = resolve_build_presets(args)
    # Targets are resolved against the first preset (target sets match across presets).
    target_names = (
        resolve_target_names(presets[0], args.target, args.emsdk_path)
        if not args.no_configure else None
    )
    if target_names is None and args.target:
        # --no-configure: can't discover, pass the literal names through to cmake.
        target_names = [s.strip() for spec in args.target for s in spec.split(",") if s.strip()]

    results = dev.build(
        presets,
        target_names,
        root=ROOT,
        auto_configure=not args.no_configure,
        mirror=args.mirror_output,
        verbose=args.verbose,
        emsdk_path=args.emsdk_path,
        keep_going=args.keep_going,
    )
    # Bundle the diag sidecars before the pass/fail gate: a failed build is
    # exactly when its per-invocation compiler errors are worth capturing.
    if args.diag_archive:
        n = dev.archive_diag([p.build_dir for p in presets], Path(args.diag_archive), ROOT)
        print(f"Diagnostics archive written to {args.diag_archive} ({n} sidecar(s))", file=sys.stderr)
    if not all(r.ok for r in results):
        _fail_build(results, presets)
    build_steps = [r for r in results if r.step_type == "build"]
    files = sum(dev.ninja_built_count(r.stdout_log) for r in build_steps)
    dev.report.summarize_build(build_steps, files, presets)
    sys.exit(0)


def cmd_test(args: argparse.Namespace) -> None:
    presets = resolve_build_presets(args)
    primary = presets[0]

    # Optionally build first (incremental — fast when nothing changed).
    if not args.no_build:
        target_names = (
            resolve_target_names(primary, args.target, args.emsdk_path)
            if not args.no_configure else None
        )
        results = dev.build(
            presets, target_names, root=ROOT,
            auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
            emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = _discover(primary, args.emsdk_path)
    wanted = resolve_target_names(primary, args.target, args.emsdk_path) if args.target else None
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets, is_test=_is_test_target,
        wanted_names=wanted, name_arg=args.test_name, target_label=args.target,
    )
    if err:
        die(err)

    records = dev.test(
        presets,
        binary_names,
        root=ROOT,
        test_name=test_name,
        timeout=args.timeout if args.timeout else None,
        write_xml=not args.no_xml_reports,
        mirror=args.mirror_output,
        verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    sys.exit(0 if dev.report.summarize_tests(records, presets, ROOT) else 1)


def cmd_test_web(args: argparse.Namespace) -> None:
    # The browser test runner is Emscripten-only. No library arg -> the aggregate page (all libraries);
    # a library arg -> just that library's page. Either way we build the module(s), then serve+open the
    # page with emrun (the page loads its wasm test module(s) and runs them one per animation frame).
    presets = resolve_presets(args.preset or [DEFAULT_WEB_PRESET])
    preset = presets[0]
    if not preset.is_emscripten:
        die(f"test-web needs an Emscripten preset (got {preset.name!r}); e.g. --preset {DEFAULT_WEB_PRESET}")

    if args.library:
        # Accept "clean-core", "clean-core-test", or "clean-core-test-web" — all name the same runner.
        lib = args.library.removesuffix("-web").removesuffix("-test")
        target: str | None = f"{lib}-test-web"
        page = f"{lib}-web.html"
    else:
        target = None  # build everything so all modules + the aggregate page are present
        page = "tests-web.html"

    if not args.no_build:
        results = dev.build(
            presets, [target] if target else None, root=ROOT,
            mirror=args.mirror_output, verbose=args.verbose, emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    page_path = preset.build_dir / page
    if not page_path.is_file():
        die(f"no page at {_rel(page_path)} - "
            + (f"library {args.library!r} has no web runner?" if args.library else "build may have failed"))

    env = dev.emsdk_env(args.emsdk_path)
    if env is None:
        die("emsdk not found - pass --emsdk-path or activate emsdk (see: uv run dev.py doctor)")
    emrun = shutil.which("emrun", path=env.get("PATH"))
    if emrun is None:
        die("emrun not found in the emsdk environment")

    # emrun serves the page's directory (the build root) so the page can reach its libs/ modules, and
    # opens the default browser. It runs in the foreground until you stop it (Ctrl-C). .bat needs cmd.
    launch = ["cmd", "/c", emrun] if emrun.lower().endswith((".bat", ".cmd")) else [emrun]
    print(console.dim(f"serving {_rel(page_path)} via emrun (Ctrl-C to stop)..."), file=sys.stderr)
    result = subprocess.run([*launch, str(page_path)], env=env, cwd=str(preset.build_dir))
    sys.exit(result.returncode)


def cmd_clean(args: argparse.Namespace) -> None:
    if args.all:
        print(console.dim("clean --all: removing all build/ artifacts"), file=sys.stderr)
        presets = dev.load_presets(ROOT)
    else:
        presets = resolve_presets(args.preset)
        print(console.dim(f"clean: preset(s) {', '.join(p.name for p in presets)}"), file=sys.stderr)

    removed_any = False
    for preset in presets:
        removed_any |= dev.remove_build_dir(preset.build_dir, dry_run=args.dry_run)
    if not removed_any:
        print(console.dim("  nothing to remove (already clean)"), file=sys.stderr)


def cmd_format(args: argparse.Namespace) -> None:
    try:
        result = dev.run_format(
            ROOT,
            check=args.check_only,
            dirty_only=args.dirty_only,
            allow_different_version=args.allow_different_version,
            mirror=args.mirror_output,
            verbose=args.verbose,
        )
    except dev.FormatSetupError as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_format(result, ROOT) else 1)


# ---------------------------------------------------------------------------
# Pre-commit checks
#
# Each registered Check is a named gate `dev.py check` can run. `dev.py check`
# with no name runs them all (the pre-commit aggregator); `dev.py check <name>`
# runs a subset. A check's `run` prints its own banner/summary and returns
# ok: bool. Checks that support `--fix` apply unambiguous fixes (clang-format);
# others ignore it and only report. A `requires_green` check (the test suite)
# is the slow tail: it runs only after every static check passed — there's no
# point building and testing a tree that already fails a cheap lint — and
# `--no-test` skips it outright (handy for a docs-only re-check). The registry
# is the growth point — new gates plug in here; the generic runner lives in
# tools/dev/checks.py.
# ---------------------------------------------------------------------------

def _check_format(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Pre-commit default is dirty-only (just the next commit's files); --all
    # widens to the whole tree. --fix rewrites in place, else check-only.
    try:
        result = dev.run_format(
            ROOT,
            check=not fix,
            dirty_only=not all_scope,
            allow_different_version=False,
            mirror=mirror,
            verbose=verbose,
        )
    except dev.FormatSetupError as e:
        die(str(e))
    return dev.report.summarize_format(result, ROOT)


def _check_crossrefs(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Always full-repo: a moved file breaks links in other, untouched files, so a
    # dirty-only scan would miss exactly the breakage this guards against. Not
    # fixable, so fix/all_scope are ignored.
    return dev.report.summarize_crossrefs(dev.check_crossrefs(ROOT), ROOT)


def _check_tests(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Build + run the full suite across build variants: debug (-O0 + mimalloc's
    # MI_DEBUG heap) and relwithdebinfo exercise CC_ASSERT on, release exercises
    # it off, and — where supported — a sanitizer (ASan+UBSan) preset adds a
    # memory/UB pass. Together they catch allocator misuse, assert regressions,
    # and undefined behavior the optimized presets miss. Warm builds are the norm
    # at commit time — uncompiled code couldn't have been tested — so the real
    # cost is the test run. Not fixable; fix/all_scope ignored.
    system = platform.system()
    specs = [default_preset_name()]
    for sibling in (
        DEFAULT_DEBUG_PRESETS.get(system),
        DEFAULT_RELEASE_PRESETS.get(system),
        DEFAULT_SANITIZE_PRESETS.get(system),
    ):
        if sibling:
            specs.append(sibling)
    presets = resolve_presets(specs)

    results = dev.build(presets, None, root=ROOT, auto_configure=True, mirror=mirror, verbose=verbose)
    if not all(r.ok for r in results):
        dev.report.print_build_failure(results, presets, ROOT)
        return False

    test_targets = [t for t in _discover(presets[0]) if _is_test_target(t)]
    if not test_targets:
        print(console.red("No test binaries found (expected '*-test' executables)"), file=sys.stderr)
        return False

    records = dev.test(
        presets, [t.name for t in test_targets], root=ROOT,
        test_name=None, timeout=60.0, write_xml=True, mirror=mirror, verbose=verbose,
    )
    return dev.report.summarize_tests(records, presets, ROOT)


# The pre-commit registry: project policy for which gates exist (the generic
# Check type and runner live in tools/dev/checks.py).
CHECKS: list[dev.Check] = [
    dev.Check("format", "clang-format libs/ sources (dirty-only; --all for the whole tree)",
              True, _check_format),
    dev.Check("crossrefs", "validate doc<->code cross-references repo-wide", False, _check_crossrefs),
    dev.Check("test", "build + run the full suite on the debug, default, release (and where supported, sanitizer) presets",
              False, _check_tests, requires_green=True),
]


def cmd_check(args: argparse.Namespace) -> None:
    if args.list:
        dev.list_checks(CHECKS)
        sys.exit(0)

    by_name = {c.name: c for c in CHECKS}
    if args.names:
        selected: list[dev.Check] = []
        seen: set[str] = set()
        for name in args.names:
            if name not in by_name:
                die(f"unknown check {name!r}. Available: {', '.join(by_name)}")
            if name not in seen:
                seen.add(name)
                selected.append(by_name[name])
    else:
        selected = list(CHECKS)

    ok = dev.run_checks(
        selected, fix=args.fix, all_scope=args.all,
        mirror=args.mirror_output, verbose=args.verbose, no_test=args.no_test,
    )
    sys.exit(0 if ok else 1)


def cmd_diagnose(args: argparse.Namespace) -> None:
    if args.diagnose_target == "clangd":
        cmd_diagnose_clangd(args)
    else:  # argparse 'required=True' should prevent this.
        die(f"unknown diagnose target {args.diagnose_target!r}")


def cmd_diagnose_clangd(args: argparse.Namespace) -> None:
    file = Path(args.file)
    if not file.is_absolute():
        file = Path.cwd() / file
    file = file.resolve()
    if not file.is_file():
        die(f"no such file: {args.file}")

    clangd_bin = dev.clangd.find_clangd()
    if clangd_bin is None:
        die("clangd not found on PATH. Install LLVM/clangd or add it to PATH.")

    # Default: reproduce the editor exactly — let clangd discover the database the
    # same way it does in the IDE (via .clangd's CompilationDatabase and its own
    # upward search). That way a misconfigured .clangd shows up here too, instead
    # of being masked. With --preset, force that preset's per-preset database.
    if args.preset:
        preset = resolve_presets(args.preset)[0]
        cc_dir = preset.build_dir
        label = f"{preset.name} ({_rel(cc_dir)})"
        if not (cc_dir / "compile_commands.json").is_file():
            die(f"no compile_commands.json at {_rel(cc_dir)} - run: uv run dev.py configure --preset {preset.name}")
    else:
        cc_dir = None
        label = "clangd auto-discovery (.clangd)"

    print(console.dim(f"clangd: checking {_rel(file)} against {label}"), file=sys.stderr)
    try:
        result = dev.clangd.check_file(clangd_bin, file, compile_commands_dir=cc_dir, timeout=120)
    except subprocess.TimeoutExpired:
        die("clangd --check timed out")

    if not result.found_database:
        print(
            console.yellow(
                f"WARNING: clangd found no compilation database for {_rel(file)} and used generic "
                f"fallback flags (no project includes), so diagnostics below are unreliable. This is "
                f"the same failure the editor hits. Check .clangd's CompilationDatabase points at the "
                f"build/ directory, and run: uv run dev.py configure"
            ),
            file=sys.stderr,
        )

    if args.verbose:
        print(result.log, file=sys.stderr)

    rel = _rel(file)
    for d in result.diagnostics:
        line = f"{rel}:{d.line}: {d.severity}: {d.message} [{d.code}]"
        if d.severity == "error":
            line = console.red(line)
        elif d.severity == "warning":
            line = console.yellow(line)
        print(line)
    sys.stdout.flush()  # keep diagnostics (stdout) ahead of the summary (stderr)

    errors = len(result.errors)
    warnings = len(result.warnings)
    if not result.diagnostics:
        print(console.green("\nNo diagnostics."), file=sys.stderr)
    else:
        summary = f"\n{errors} error(s), {warnings} warning(s)"
        print(console.red(summary) if errors else console.yellow(summary), file=sys.stderr)
    sys.exit(1 if errors else 0)


def cmd_doctor(args: argparse.Namespace) -> None:
    checks = dev.doctor(
        ROOT, default_preset=DEFAULT_BUILD_PRESETS.get(platform.system()), emsdk_path=args.emsdk_path
    )
    all_ok = True
    for label, ok, detail in checks:
        # ok is True (pass), False (fail), or None for an advisory (neither).
        if ok is None:
            mark = console.yellow("SKIP")
        elif ok:
            mark = console.green("OK  ")
        else:
            mark = console.red("FAIL")
            all_ok = False
        print(f"  [{mark}] {label}: {detail}")
    sys.exit(0 if all_ok else 1)


def cmd_list_presets(args: argparse.Namespace) -> None:
    for p in dev.load_presets(ROOT):
        print(f"{p.name}  ({p.build_type or 'no build type'} -> {p.configure_preset})")


def cmd_list_targets(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    for t in _discover(presets[0], args.emsdk_path):
        artifact = f"  -> {t.artifact}" if t.artifact else ""
        print(f"{t.name}  [{t.kind}]{artifact}")


# ---------------------------------------------------------------------------
# Info
#
# Read-only inspection of what the build configuration actually passes to the
# tools. `build-flags`/`link-flags` print the per-target settings from the CMake
# File API (TU-flag-set aware: one block per distinct compile group);
# `compile-command` prints the exact per-file invocation from
# compile_commands.json — the ground truth the compiler sees.
# ---------------------------------------------------------------------------

def cmd_info(args: argparse.Namespace) -> None:
    match args.info_cmd:
        case "build-flags":
            cmd_info_flags(args, compile_=True, link=False)
        case "link-flags":
            cmd_info_flags(args, compile_=False, link=True)
        case "compile-command":
            cmd_info_compile_command(args)
        case _:  # argparse 'required=True' should prevent this.
            die(f"unknown info subcommand {args.info_cmd!r}")


def _print_compile_group(group: dev.CompileGroup, *, sole: bool, index: int) -> None:
    std = f"C++{group.std}" if group.std and group.language == "CXX" else (group.std or "?")
    if sole:
        print(f"  compile flags  ({group.language}, {std}, {len(group.sources)} sources)")
    else:
        print(f"  flag set #{index}  ({group.language}, {std}, {len(group.sources)} sources)")
    if group.defines:
        print(f"    defines:  {', '.join(group.defines)}")
    if group.includes:
        rendered = [(f"[sys] {_rel(Path(p))}" if sys_ else _rel(Path(p))) for p, sys_ in group.includes]
        print(f"    includes: {', '.join(rendered)}")
    for frag in group.flags:
        print(f"    flags:    {frag}")
    if not sole:
        for src in group.sources:
            print(console.dim(f"      - {_rel(Path(src))}"))


def cmd_info_flags(args: argparse.Namespace, *, compile_: bool, link: bool) -> None:
    preset = resolve_presets(args.preset)[0]
    names = resolve_target_names(preset, args.target, args.emsdk_path) or []
    models = dev.load_target_models(preset.build_dir, preset.build_type)
    for i, name in enumerate(names):
        if i:
            print()
        tf = dev.extract_flags(models[name])
        print(console.bold(f"{tf.name}  [{tf.kind}]") + console.dim(f"  preset={preset.name}"))
        if compile_:
            if not tf.compile_groups:
                print("  (no compile step - not a compiled target)")
            for j, group in enumerate(tf.compile_groups, start=1):
                _print_compile_group(group, sole=len(tf.compile_groups) == 1, index=j)
        if link:
            if not tf.link_flags and not tf.link_libraries:
                print("  (no link step - static library or non-linked target)")
            else:
                if tf.link_flags:
                    print(f"  link flags:     {' '.join(tf.link_flags)}")
                for libline in tf.link_libraries:
                    print(f"  link library:   {libline}")


def cmd_info_compile_command(args: argparse.Namespace) -> None:
    preset = resolve_presets(args.preset)[0]
    # compile_commands.json is produced by configure; make sure it exists.
    _discover(preset, args.emsdk_path)
    try:
        entries = dev.load_entries(preset.build_dir)
    except FileNotFoundError as e:
        die(str(e))
    requested = Path(args.file)
    entry = dev.find_entry(entries, requested, ROOT)
    if entry is None:
        hint = dev.suggest_files(entries, requested)
        msg = f"No compile command for {args.file!r} in {preset.name}."
        if hint:
            msg += " Did you mean:\n  " + "\n  ".join(_rel(Path(h)) for h in hint)
        die(msg)
    print(console.bold(_rel(Path(entry["file"]))) + console.dim(f"  preset={preset.name}"))
    command = entry.get("command", "")
    if args.raw:
        print(command)
    else:
        for arg in shlex.split(command, posix=False):
            print(f"  {arg}")


# ---------------------------------------------------------------------------
# Coverage
#
# `coverage` collects LLVM source-based coverage from the instrumented
# *-coverage presets. Three phases map to subcommands: `run` (build + run tests
# + merge + report), `merge` (combine several presets' merged data), and
# `report` (re-post-process existing data without re-running). The raw
# `llvm-cov export` JSON lands as a `.llvm-cov.json` sidecar in the build dir for
# future tooling (a `coverage_diag` analog of build_diag/test_diag).
# ---------------------------------------------------------------------------

def default_coverage_preset_name() -> str:
    name = COVERAGE_BUILD_PRESETS.get(platform.system())
    if name is None:
        die(f"No default coverage preset for {platform.system()!r}. Use --preset.")
    return name


def _resolve_coverage_presets(specs: list[str] | None) -> list[dev.Preset]:
    """Resolve --preset for coverage, defaulting to the platform coverage preset."""
    return resolve_presets(specs or [default_coverage_preset_name()])


def cmd_coverage(args: argparse.Namespace) -> None:
    match args.coverage_cmd:
        case "run":
            cmd_coverage_run(args)
        case "merge":
            cmd_coverage_merge(args)
        case "report":
            cmd_coverage_report(args)
        case _:  # argparse 'required=True' should prevent this.
            die(f"unknown coverage subcommand {args.coverage_cmd!r}")


def cmd_coverage_run(args: argparse.Namespace) -> None:
    presets = _resolve_coverage_presets(args.preset)
    primary = presets[0]

    if not args.no_build:
        results = dev.build(
            presets, None, root=ROOT, auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    all_targets = _discover(primary)
    wanted = resolve_target_names(primary, args.target) if args.target else None
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets, is_test=_is_test_target,
        wanted_names=wanted, name_arg=args.pattern, target_label=args.target,
    )
    if err:
        die(err)
    try:
        cov_results = dev.coverage_run(
            presets, binary_names, root=ROOT, test_name=test_name, html=args.html,
            timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.CoverageToolError as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_coverage(cov_results, ROOT) else 1)


def cmd_coverage_report(args: argparse.Namespace) -> None:
    presets = _resolve_coverage_presets(args.preset)
    binary_names = [t.name for t in _discover(presets[0]) if _is_test_target(t)]
    try:
        results = dev.coverage_report(
            presets, binary_names, root=ROOT, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_coverage(results, ROOT) else 1)


def cmd_coverage_merge(args: argparse.Namespace) -> None:
    if not args.preset:
        die("coverage merge needs at least one --preset (which presets' data to combine)")
    presets = resolve_presets(args.preset)
    names_by_preset = {p.name: [t.name for t in _discover(p) if _is_test_target(t)] for p in presets}
    output_dir = Path(args.output) if args.output else (ROOT / "build" / "coverage-merged")
    try:
        result = dev.coverage_merge(
            presets, names_by_preset, root=ROOT, output_dir=output_dir, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_coverage([result], ROOT) else 1)


# ---------------------------------------------------------------------------
# PGO (profile-guided optimization)
# ---------------------------------------------------------------------------

def _platform_preset(table: dict[str, str], what: str) -> dev.Preset:
    name = table.get(platform.system())
    if name is None:
        die(f"No default {what} preset for {platform.system()!r}. Use --preset.")
    return resolve_presets([name])[0]


def _pgo_presets(args: argparse.Namespace) -> tuple[dev.Preset, dev.Preset, dev.Preset]:
    """Resolve (generate, use, baseline) presets. --preset overrides the generate/use pair."""
    if args.preset:
        gen = resolve_presets(args.preset)[0]
        # Derive the matching use preset from the generate name (…-generate… → …-use…).
        use = resolve_presets([gen.name.replace("generate", "use")])[0]
    else:
        gen = _platform_preset(PGO_GENERATE_PRESETS, "pgo-generate")
        use = _platform_preset(PGO_USE_PRESETS, "pgo-use")
    baseline = _platform_preset(PGO_BASELINE_PRESETS, "pgo baseline")
    return gen, use, baseline


def _pgo_binary_names(preset: dev.Preset) -> list[str]:
    """Test binaries to drive (every *-test); guide-benchmark-less ones simply no-op."""
    return [t.name for t in _discover(preset) if _is_test_target(t)]


def cmd_pgo(args: argparse.Namespace) -> None:
    match args.pgo_cmd:
        case "run":
            cmd_pgo_run(args)
        case "instrument":
            cmd_pgo_instrument(args)
        case "train":
            cmd_pgo_train(args)
        case "optimize":
            cmd_pgo_optimize(args)
        case "measure":
            cmd_pgo_measure(args)
        case _:  # argparse 'required=True' should prevent this.
            die(f"unknown pgo subcommand {args.pgo_cmd!r}")


def cmd_pgo_instrument(args: argparse.Namespace) -> None:
    gen, _use, _base = _pgo_presets(args)
    results = dev.pgo_instrument(gen, root=ROOT, mirror=args.mirror_output, verbose=args.verbose)
    if not all(r.ok for r in results):
        _fail_build(results, [gen])
    print(console.green(f"Instrumented build ready: {gen.name}"), file=sys.stderr)
    sys.exit(0)


def cmd_pgo_train(args: argparse.Namespace) -> None:
    gen, _use, _base = _pgo_presets(args)
    binaries = _pgo_binary_names(gen)
    try:
        result = dev.pgo_train(
            gen, binaries, root=ROOT, timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_pgo({"ok": result["ok"], "train": result, "measure": None}, ROOT) else 1)


def cmd_pgo_optimize(args: argparse.Namespace) -> None:
    _gen, use, _base = _pgo_presets(args)
    try:
        results = dev.pgo_optimize(use, root=ROOT, mirror=args.mirror_output, verbose=args.verbose)
    except dev.PgoError as e:
        die(str(e))
    if not all(r.ok for r in results):
        _fail_build(results, [use])
    print(console.green(f"Optimized (PGO) build ready: {use.name}"), file=sys.stderr)
    sys.exit(0)


def cmd_pgo_measure(args: argparse.Namespace) -> None:
    _gen, use, baseline = _pgo_presets(args)
    binaries = _pgo_binary_names(use)
    try:
        result = dev.pgo_measure(
            baseline, use, binaries, root=ROOT, timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_pgo({"ok": True, "train": None, "measure": result}, ROOT) else 1)


def cmd_pgo_run(args: argparse.Namespace) -> None:
    gen, use, baseline = _pgo_presets(args)
    binaries = _pgo_binary_names(gen)
    try:
        result = dev.pgo_run(
            gen, use, baseline, binaries, root=ROOT, measure=not args.no_measure,
            timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        die(str(e))
    sys.exit(0 if dev.report.summarize_pgo(result, ROOT) else 1)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _add_preset_arg(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--preset", action="append",
        help="Build preset(s): comma-list, repeatable, and shell-style wildcards "
             "(default: auto-detected by platform)",
    )


def _add_build_override_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--toolset", metavar="VERSION", default=None,
        help="Pin the compiler version within the preset's family: a bare version "
             "(clang/gcc -> clang++-N/g++-N on PATH; msvc -> vcvars_ver, e.g. 14.51) or an "
             "explicit compiler path. Not found = hard error. Auto-redirects the build dir so "
             "toolsets don't share a CMake cache.",
    )
    p.add_argument(
        "--build-suffix", metavar="TAG", default=None,
        help="Append '-TAG' to the build folder (build/<preset>-TAG). The go-to for a "
             "toolset matrix: one folder per toolset, side by side.",
    )
    p.add_argument(
        "--build-dir", metavar="PATH", default=None,
        help="Use this build directory instead of build/<preset> (relative to the repo root, "
             "or absolute). For a fully custom layout; single preset only.",
    )


def _add_emsdk_arg(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--emsdk-path", metavar="DIR", default=None,
        help="Path to an emsdk install for the WASM (Emscripten) presets; dev.py applies its "
             "environment itself, so no permanent/--system activation is needed. Falls back to "
             "SC_EMSDK_PATH / EMSDK / emcc-on-PATH.",
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="shaped-core build & test CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--mirror-output", action="store_true",
                        help="Stream child stdout/stderr live instead of only capturing it")
    parser.add_argument("--collect-logs", metavar="FILE", default=None,
                        help="On exit (pass or fail), bundle all captured run logs and step sidecars "
                             "under build/ into a zip at FILE — last-resort raw diagnostics for CI.")
    color_group = parser.add_mutually_exclusive_group()
    color_group.add_argument("--colored", action="store_true",
                             help="Force colored output (default: auto-detect by terminal)")
    color_group.add_argument("--plain", action="store_true",
                             help="Force plain, uncolored output")

    sub = parser.add_subparsers(dest="command", required=True)

    cfg_p = sub.add_parser("configure", help="Configure the CMake project")
    _add_preset_arg(cfg_p)
    _add_build_override_args(cfg_p)
    _add_emsdk_arg(cfg_p)

    build_p = sub.add_parser("build", help="Build the project")
    _add_preset_arg(build_p)
    _add_build_override_args(build_p)
    _add_emsdk_arg(build_p)
    build_p.add_argument("--target", "-t", action="append",
                         help="Target(s) to build: comma-list, repeatable, wildcards")
    build_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    build_p.add_argument("--keep-going", "-k", action="store_true",
                         help="Keep building after the first error (ninja -k 0) so one run surfaces every "
                              "independent failure instead of stopping at the first — pairs with --diag-archive.")
    build_p.add_argument("--diag-archive", metavar="FILE",
                         help="After building, bundle every .diag.json sidecar (one per compile/link, "
                              "written by diag-launcher) into a zip at FILE — the build-step analogue of "
                              "--merged-xml-report. Produced even when the build fails; extract at the repo "
                              "root and inspect with build_diag.")

    test_p = sub.add_parser("test", help="Run tests")
    _add_preset_arg(test_p)
    _add_build_override_args(test_p)
    _add_emsdk_arg(test_p)
    test_p.add_argument("--target", "-t", action="append",
                        help="Test binary target(s): comma-list, repeatable, wildcards")
    test_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    test_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    test_p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                        help="Per-binary timeout in seconds (default: 60; 0 disables). The binary is "
                             "killed and reported as failed if it exceeds it.")
    xml_group = test_p.add_mutually_exclusive_group()
    xml_group.add_argument("--merged-xml-report", metavar="FILE",
                           help="Also merge the per-binary JUnit XML into a single report at FILE")
    xml_group.add_argument("--no-xml-reports", action="store_true",
                           help="Do not write any JUnit XML result files (per-binary XML is on by default)")
    test_p.add_argument("test_name", nargs="?",
                        help="Specific test name or binary to run (auto-discovers the binary)")

    web_p = sub.add_parser("test-web", help="Open the browser test runner (Emscripten); serves + opens via emrun")
    _add_preset_arg(web_p)
    _add_emsdk_arg(web_p)
    web_p.add_argument("library", nargs="?",
                       help="Library to show alone (e.g. clean-core); omit for the combined page of all libraries")
    web_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")

    fmt_p = sub.add_parser("format", help="Format C++ sources with clang-format")
    fmt_p.add_argument("--check-only", action="store_true",
                       help="Report non-conforming files and exit non-zero; do not rewrite")
    fmt_p.add_argument("--dirty-only", action="store_true",
                       help="Only format git-dirty/untracked libs/ sources (good pre-commit check)")
    fmt_p.add_argument("--allow-different-version", action="store_true",
                       help="Downgrade a clang-format version mismatch from error to warning")

    check_p = sub.add_parser("check", help="Run pre-commit checks (format, crossrefs, ...)")
    check_p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    check_p.add_argument("--fix", action="store_true",
                         help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    check_p.add_argument("--all", action="store_true",
                         help="Widen the format check from dirty-only to the whole tree")
    check_p.add_argument("--no-test", action="store_true",
                         help="Skip the test suite (build + run); just the static checks")
    check_p.add_argument("--list", action="store_true", help="List registered checks and exit")

    cov_p = sub.add_parser("coverage", help="Collect LLVM source-based test coverage")
    cov_sub = cov_p.add_subparsers(dest="coverage_cmd", required=True)

    cov_run_p = cov_sub.add_parser("run", help="Build + run instrumented tests, then merge & report")
    _add_preset_arg(cov_run_p)
    cov_run_p.add_argument("--target", "-t", action="append",
                           help="Test binary target(s): comma-list, repeatable, wildcards")
    cov_run_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    cov_run_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    cov_run_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")
    cov_run_p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                           help="Per-binary timeout in seconds (default: 60; 0 disables)")
    cov_run_p.add_argument("pattern", nargs="?",
                           help="Specific test name or binary to run (auto-discovers the binary)")

    cov_merge_p = cov_sub.add_parser("merge", help="Combine several presets' coverage into one report")
    _add_preset_arg(cov_merge_p)
    cov_merge_p.add_argument("--output", "-o", metavar="DIR",
                             help="Output directory (default: build/coverage-merged)")
    cov_merge_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")

    cov_report_p = cov_sub.add_parser("report", help="Re-post-process existing coverage (no test run)")
    _add_preset_arg(cov_report_p)
    cov_report_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")

    pgo_p = sub.add_parser("pgo", help="Profile-guided optimization (instrument/train/optimize/measure)")
    pgo_sub = pgo_p.add_subparsers(dest="pgo_cmd", required=True)

    def _add_pgo_timeout(p: argparse.ArgumentParser) -> None:
        p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                       help="Per-binary timeout in seconds (default: 0 = disabled; benchmarks run long)")

    pgo_run_p = pgo_sub.add_parser("run", help="Full pipeline: instrument -> train -> optimize -> measure")
    _add_preset_arg(pgo_run_p)
    _add_pgo_timeout(pgo_run_p)
    pgo_run_p.add_argument("--no-measure", action="store_true",
                           help="Stop after the optimized build; skip the baseline-vs-PGO measurement")

    pgo_inst_p = pgo_sub.add_parser("instrument", help="Build the instrumented (-fprofile-generate) preset")
    _add_preset_arg(pgo_inst_p)

    pgo_train_p = pgo_sub.add_parser("train", help="Run guide benchmarks on the instrumented build, merge profile")
    _add_preset_arg(pgo_train_p)
    _add_pgo_timeout(pgo_train_p)

    pgo_opt_p = pgo_sub.add_parser("optimize", help="Build the optimized (-fprofile-use) preset from the profile")
    _add_preset_arg(pgo_opt_p)

    pgo_meas_p = pgo_sub.add_parser("measure", help="Run guide benchmarks on baseline + PGO and diff metrics")
    _add_preset_arg(pgo_meas_p)
    _add_pgo_timeout(pgo_meas_p)

    clean_p = sub.add_parser("clean", help="Remove build artifacts")
    _add_preset_arg(clean_p)
    clean_p.add_argument("--all", action="store_true", help="Remove every preset's build directory")
    clean_p.add_argument("--dry-run", action="store_true", help="Print what would be removed")

    diag_p = sub.add_parser("diagnose", help="Diagnose tooling (e.g. clangd) for a file")
    diag_sub = diag_p.add_subparsers(dest="diagnose_target", required=True)
    clangd_p = diag_sub.add_parser(
        "clangd", help="Show clangd diagnostics for a file (uses build/compile_commands.json)"
    )
    _add_preset_arg(clangd_p)
    clangd_p.add_argument(
        "file", help="Source file to check (its compile flags come from the compilation database)"
    )

    info_p = sub.add_parser("info", help="Inspect resolved compile/link flags and per-file commands")
    info_sub = info_p.add_subparsers(dest="info_cmd", required=True)

    def _add_info_target(p: argparse.ArgumentParser) -> None:
        _add_preset_arg(p)
        _add_emsdk_arg(p)
        p.add_argument("target", nargs="+", help="Target(s): comma-list, repeatable, wildcards")

    info_bf_p = info_sub.add_parser("build-flags", help="Per-target compile flags (one block per distinct flag set)")
    _add_info_target(info_bf_p)
    info_lf_p = info_sub.add_parser("link-flags", help="Per-target linker flags and link libraries")
    _add_info_target(info_lf_p)

    info_cc_p = info_sub.add_parser(
        "compile-command", help="Exact compile command for one source file (from compile_commands.json)"
    )
    _add_preset_arg(info_cc_p)
    _add_emsdk_arg(info_cc_p)
    info_cc_p.add_argument("file", help="Source file (absolute, repo-relative, or a unique filename)")
    info_cc_p.add_argument("--raw", action="store_true",
                           help="Print the verbatim single-line command instead of one argument per line")

    doctor_p = sub.add_parser("doctor", help="Sanity-check the toolchain")
    _add_emsdk_arg(doctor_p)

    lp = sub.add_parser("list-presets", help="List available build presets")
    _add_preset_arg(lp)
    lt = sub.add_parser("list-targets", help="List discovered targets")
    _add_preset_arg(lt)
    _add_emsdk_arg(lt)

    args = parser.parse_args()
    console.configure("colored" if args.colored else "plain" if args.plain else "auto")

    # Capture logs on the way out regardless of how the command exits (including
    # sys.exit on a failed build/test) — atexit fires on SystemExit too.
    if args.collect_logs:
        import atexit

        def _emit_log_archive() -> None:
            try:
                n = dev.archive_logs(ROOT / "build", Path(args.collect_logs), ROOT)
                print(f"Log archive written to {args.collect_logs} ({n} file(s))", file=sys.stderr)
            except OSError as e:
                print(f"warning: failed to write log archive {args.collect_logs}: {e}", file=sys.stderr)

        atexit.register(_emit_log_archive)

    match args.command:
        case "configure":
            cmd_configure(args)
        case "build":
            cmd_build(args)
        case "test":
            cmd_test(args)
        case "test-web":
            cmd_test_web(args)
        case "format":
            cmd_format(args)
        case "check":
            cmd_check(args)
        case "coverage":
            cmd_coverage(args)
        case "pgo":
            cmd_pgo(args)
        case "clean":
            cmd_clean(args)
        case "diagnose":
            cmd_diagnose(args)
        case "info":
            cmd_info(args)
        case "doctor":
            cmd_doctor(args)
        case "list-presets":
            cmd_list_presets(args)
        case "list-targets":
            cmd_list_targets(args)


if __name__ == "__main__":
    main()
