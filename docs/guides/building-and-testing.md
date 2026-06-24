# Building and Testing

[dev.py](../../dev.py) is the unified build & test driver for shaped-core. It wraps CMake +
Ninja, discovers targets and tests, captures output, and emits machine-readable results. This is
the full reference; the `/building-and-testing` skill
([.claude/skills/building-and-testing/SKILL.md](../../.claude/skills/building-and-testing/SKILL.md))
is the short version you run during a session.

Run everything from the repo root via `uv` — it resolves the Python deps inline (PEP 723), no
venv setup:

```bash
uv run dev.py <command> [options]
```

## Commands

| Command        | What it does                                                                 |
|----------------|------------------------------------------------------------------------------|
| `configure`    | Configure the CMake project for the selected preset(s).                       |
| `build`        | Build (auto-configures first if inputs changed). `-t/--target` to scope.      |
| `test`         | Build, then run test binaries (`*-test`). Optional name/binary filter.        |
| `format`       | clang-format `libs/` sources in place (see below).                            |
| `clean`        | Remove a preset's build directory (`--all` for every preset, `--dry-run`).    |
| `diagnose clangd FILE` | Show clangd's diagnostics for a source file (see below).              |
| `doctor`       | Read-only toolchain sanity check (cmake, ninja, compiler, presets, clangd).   |
| `list-presets` | List available build presets.                                                |
| `list-targets` | List discovered CMake targets for a preset.                                   |

`build` and `test` **auto-configure** when CMake inputs or the source listing change
(fingerprinted); pass `--no-configure` to skip. `test` also builds first; pass `--no-build` to
skip.

### Common invocations

```bash
uv run dev.py test                  # build + run the full suite
uv run dev.py test "<pattern>"      # run just tests whose name matches (or a whole *-test binary)
uv run dev.py test -t clean-core-test
uv run dev.py build                 # build everything
uv run dev.py build -t nexus        # build one target
uv run dev.py doctor
```

The positional argument to `test` is smart: if it names a test binary, that whole binary runs;
otherwise it is treated as a **test-name substring filter** applied across every `*-test` binary
(binaries with no match are skipped, not failed).

## Presets

Presets live in [CMakePresets.json](../../CMakePresets.json), one per platform × compiler ×
build type (MSVC / Clang / GCC across Windows / Linux / macOS / Android NDK). The default is
chosen by platform:

| Platform | Default preset                       | Assertions (`CC_ASSERT`) |
|----------|--------------------------------------|--------------------------|
| Windows  | `relwithdebinfo-clang`               | on                       |
| Linux    | `relwithdebinfo-linux-clang`         | on                       |
| macOS    | `macos-arm-llvm-relwithdebinfo`      | on                       |

Override with `--preset`. **It is a per-subcommand flag — it goes *after* the subcommand**
(`uv run dev.py test --preset release-clang`). Only `--verbose` and `--mirror-output` are global
(before the subcommand). Presets accept comma-lists, repeated flags, and shell-style wildcards,
and dev.py operates on all that match:

```bash
uv run dev.py build --preset debug-clang,release-clang
uv run dev.py test  --preset "x64-linux-*"
```

`relwithdebinfo-*` presets have `CC_ASSERT` **on**; `release-*` presets have them **off**. When
you touch assertion-gated code, build a `release-*` preset too — the default build only exercises
the assertions-on branch.

## Quiet by default, and how to diagnose

dev.py does **not** stream child output. For each step it:

- captures stdout/stderr to `build/<preset>/run-logs/run-log-<name>.{stdout,stderr}.txt`,
- writes a JSON sidecar in the build dir (`configure.json` / `build.json` / `test.json`),
- writes a per-binary JUnit `*.results.xml` next to each test binary,

then prints a one-line trace per step plus a pass/fail summary. On failure it prints the exact
diagnostic selector to run.

So the loop is **dev.py, then diagnose with the `repo_tools` MCP tools**:

| After a... | Tool         | Typical call                                                       |
|------------|--------------|-------------------------------------------------------------------|
| build      | `build_diag` | `build_diag base_path="build/<preset>"`                           |
| test run   | `test_diag`  | `test_diag path="build/<preset>/**/*.results.xml" errors_only=true` |

These read the artifacts dev.py already emitted — far better than scrolling raw logs. Pass
`--mirror-output` only when you want the live stream (e.g. to watch a crashing binary).

**Don't pipe dev.py into `tail`/`head`/`grep`.** The output is already terse, and
`… 2>&1 | tail` reports the pipe's exit code (0) — masking a real failure as success.

## Formatting

`uv run dev.py format` runs clang-format over the project's `.cc`/`.hh` sources
(scoped to `libs/` for now), with [.clang-format](../../.clang-format) as the
authoritative style:

```bash
uv run dev.py format                         # format every libs/ source in place
uv run dev.py format --dirty-only            # only git-dirty/untracked files — the pre-commit move
uv run dev.py format --dirty-only --check-only   # verify without rewriting (exit 1 if any differ)
```

`--check-only` rewrites nothing; it lists the non-conforming files and exits
non-zero, so it works as a CI / pre-commit gate. `--dirty-only` restricts the
set to what's part of your next commit (modified, staged, or untracked) — pair
the two before committing.

clang-format output is not stable across major versions, so the command pins to
the major version declared by `.clang-format`'s `Requires: clang-format >= N`
header and **errors** if the installed clang-format's major differs. Pass
`--allow-different-version` to downgrade that to a warning and proceed anyway.
Like every other step, the clang-format run is captured under
`build/run-logs/`.

## clangd / IDE code intelligence

The presets set `CMAKE_EXPORT_COMPILE_COMMANDS`, so every configure emits a
`compile_commands.json` into the per-preset build dir (`build/<preset>/`). `configure`
**publishes** the active preset's database to `build/compile_commands.json` after a successful run,
and [.clangd](../../.clangd) points clangd's `CompilationDatabase` at the `build` **directory** so
it picks that file up. Two things matter here and are easy to get wrong:

- `CompilationDatabase` is a **directory**, not a file path. Pointing it at
  `build/compile_commands.json` makes clangd silently fall back to bare flags (no includes,
  wrong language standard) and flag every line.
- The published file is what makes clangd work regardless of whether you configured through
  `dev.py` or the VSCode CMake Tools extension. With multiple presets, the last one configured
  wins that single published database.

If clangd reports errors in the editor that don't match reality, reproduce them from the CLI:

```bash
uv run dev.py diagnose clangd libs/base/nexus/src/nexus/run.cc
uv run dev.py diagnose clangd <file> --preset debug-clang   # force a specific preset's database
```

This runs `clangd --check` and prints every diagnostic as `file:line: severity: message [code]`
(exit 1 if any are errors). By **default it uses clangd's own discovery** (`.clangd` + upward
search) — i.e. exactly what the editor does — so a misconfigured `.clangd` shows up here too rather
than being masked; it warns loudly when clangd falls back to generic flags. Pass `--preset` to
force that preset's per-preset database instead. Note that `clangd --check` also self-tests
refactor tweaks and inflates its own "N errors" summary with those failures — `diagnose clangd`
ignores that noise and reports only real code diagnostics. `doctor` runs the same discovery-based
check on a sample file to confirm clangd is wired up end-to-end.

A common first move when the editor looks wrong: reload the clangd language server (the published
database, or a just-edited `.clangd`, can be stale relative to the running server).

## Tests (nexus)

Test executables follow the convention `<lib>-test` and are built on the **nexus** framework,
which is Catch2 v3 CLI–compatible (see
[nexus' catch2-runner-compat](../../libs/base/nexus/docs/catch2-runner-compat.md)). dev.py
discovers them via the CMake File API, runs each with the optional name filter, and synthesizes a
JUnit result from the process — so a binary that **crashes before printing anything** is still
recorded as a failure (non-zero exit). Re-run that binary with `--mirror-output` to see what
happened.

Never run a test binary directly — always go through `dev.py test`, so discovery, capture, and
result recording stay consistent.

## Useful flags

- `--mirror-output` / `--verbose` — global (before the subcommand); stream child output / be chatty.
- `--no-configure`, `--no-build` — skip the automatic steps.
- `--timeout SECS` (on `test`) — per-binary timeout (default 60; `0` disables). A binary that
  exceeds it is killed and reported as failed.
- `--merged-xml-report FILE` / `--no-xml-reports` (on `test`) — merge per-binary XML into one
  file / skip XML entirely. Per-binary XML is on by default and is what `test_diag` reads, so you
  usually need neither.

## Related

- [.claude/skills/building-and-testing/SKILL.md](../../.claude/skills/building-and-testing/SKILL.md)
  — the skill this doc backs.
- [docs/coding-guidelines.md](../coding-guidelines.md) — how to write the code you're building.
