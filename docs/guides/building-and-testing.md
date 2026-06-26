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
| `check`        | Run pre-commit checks (format, crossrefs, test) and report one green/red verdict. |
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
build type (MSVC / Clang / GCC across Windows / Linux / macOS / Android NDK / Emscripten). The
default is chosen by platform:

| Platform | Default preset                       | Assertions (`CC_ASSERT`) |
|----------|--------------------------------------|--------------------------|
| Windows  | `relwithdebinfo-clang`               | on                       |
| Linux    | `relwithdebinfo-linux-clang`         | on                       |
| macOS    | `macos-arm-llvm-relwithdebinfo`      | on                       |

Override with `--preset`. **It is a per-subcommand flag — it goes *after* the subcommand**
(`uv run dev.py test --preset release-clang`). `--emsdk-path` (for WASM) is likewise
per-subcommand. Only `--verbose`, `--mirror-output`, and `--colored` / `--plain` are global (before the subcommand).
Presets accept comma-lists, repeated flags, and shell-style wildcards, and dev.py operates on
all that match:

```bash
uv run dev.py build --preset debug-clang,release-clang
uv run dev.py test  --preset "x64-linux-*"
```

`relwithdebinfo-*` presets have `CC_ASSERT` **on**; `release-*` presets have them **off**. When
you touch assertion-gated code, build a `release-*` preset too — the default build only exercises
the assertions-on branch.

### WebAssembly (Emscripten)

The `emscripten-{debug,relwithdebinfo,release}` presets cross-compile to WASM (single-threaded).
They need the [emsdk](https://github.com/emscripten-core/emsdk); point dev.py at it with the
`--emsdk-path` flag (or `SC_EMSDK_PATH` / an activated `EMSDK`) — dev.py applies the emsdk
environment itself, so no permanent activation is required:

```bash
uv run dev.py test --preset emscripten-relwithdebinfo --emsdk-path /path/to/emsdk
```

The test binaries are `.wasm` + a `.js` loader; dev.py runs them under emsdk's Node and parses the
same JUnit report as native runs. `uv run dev.py doctor` validates the toolchain. See
[requirements.md](../requirements.md#emscripten--wasm) for the full setup and the feature knobs.

**Browser test runner.** Under Emscripten each `*-test` target also builds a MODULARIZE wasm module
(`*-test-web.js` + `.wasm`), and CMake generates HTML pages at the build root: one per library
(`<library>-web.html`) plus an aggregate **`tests-web.html`** that loads every library's module and
shows them grouped with a grand total. Each page runs its tests **one per animation frame**, rendering
a live table of per-test timing, assertion counts, and pass/fail. Open them with:

```bash
uv run dev.py test-web                 # combined page: all libraries
uv run dev.py test-web clean-core      # just one library
```

`test-web` builds the module(s) and serves the page with emsdk's **emrun** (a dev-only static server +
browser launcher; Ctrl-C to stop). It defaults to the `emscripten-relwithdebinfo` preset and takes the
same `--emsdk-path` as the other commands. The pages are plain static files — to host them, serve the
build root over HTTP from any static server (not `file://`); emrun is not needed in deployment.

The pages are built by the normal `dev.py build` too (they're regular targets) and are not run by
`dev.py test` (only `*-test`, not `*-test-web`, is a node test). The implementation is in nexus:
[web_runner.cc](../../libs/base/nexus/src/nexus/web/web_runner.cc) (the `nx_web_*` C ABI), the shared
renderer [nexus-web-driver.js](../../libs/base/nexus/web/nexus-web-driver.js), the page template
[nexus-web-page.html.in](../../libs/base/nexus/web/nexus-web-page.html.in), and the build glue
[NexusWebRunner.cmake](../../libs/base/nexus/cmake/NexusWebRunner.cmake).

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
`build/run-logs/`. Before committing, prefer `uv run dev.py check --fix`, which
runs this format check (and the others) in one shot — see
[Pre-commit checks](#pre-commit-checks).

## Pre-commit checks

`uv run dev.py check` runs the project's pre-commit gates and reports a single
green/red verdict — the "everything green" gate. **`uv run dev.py check --fix`
is the recommended move before committing**: it auto-applies every unambiguous
fix it can (currently clang-format), runs the rest, and tells you what's left.

```bash
uv run dev.py check            # run every check -> one verdict
uv run dev.py check --fix      # apply fixable checks (clang-format -i), then report
uv run dev.py check --no-test  # static checks only — skip the build+test tail (docs-only re-check)
uv run dev.py check --all      # widen the format check from dirty-only to the whole tree
uv run dev.py check crossrefs  # run just one (or several) checks by name
uv run dev.py check --list     # list the registered checks
```

Registered checks:

| Check       | What it does                                                                   | `--fix`? |
|-------------|--------------------------------------------------------------------------------|----------|
| `format`    | clang-format `libs/` sources. Dirty-only by default; `--all` for the whole tree. | yes (rewrites in place) |
| `crossrefs` | Validate doc↔code cross-references repo-wide (always full-repo).                 | no (report only) |
| `test`      | Build + run the full suite on the debug, default, release **and** (Linux/macOS) sanitizer presets. | no (report only) |

`crossrefs` scans markdown links (`[text](path#L42)`, including line/heading
anchors) and `//`-comment doc references (`docs/...md`) across `libs/`, `docs/`,
`.claude/`, and the root meta docs, and flags any that no longer resolve. These
rot silently — a moved file breaks links in *other*, untouched files — so the
scan is always full-repo, not dirty-only, and `--all` does not affect it. It
reports each offender as `file:line: reason`.

`test` is the slow tail and runs **only after the static checks pass** (no point
building a tree that already fails a cheap lint), and `--no-test` skips it. It
builds and runs the suite across build variants: **debug** (`-O0` plus mimalloc's
`MI_DEBUG` heap, `CC_ASSERT` on), the platform default **relwithdebinfo**
(`CC_ASSERT` on), the **release** sibling (`CC_ASSERT` off), and — on **Linux and
macOS** — a **sanitizer** preset (ASan+UBSan). So assertion modes, the debug
allocator, and undefined behavior are all covered every commit. Builds are warm at
commit time — code you didn't compile, you didn't test — so the real cost is the
test run (well under a second); expect a one-time cold build of the extra presets.
On failure it prints the `test_diag` selector, same as `dev.py test`. See
[Sanitizers](#sanitizers) for why Windows is excluded.

New gates plug into the check registry in [dev.py](../../dev.py) without changing
the command surface.

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

## Sanitizers

The `sanitize-*` presets are Debug builds with AddressSanitizer + UndefinedBehaviorSanitizer
(`SANITIZE=address,undefined`, wired in the root [CMakeLists.txt](../../CMakeLists.txt)):

```bash
uv run dev.py test --preset sanitize-linux-clang   # Linux
uv run dev.py test --preset sanitize-macos-arm-llvm # macOS
uv run dev.py test --preset sanitize-clang          # Windows (see caveat)
```

On **Linux and macOS** the clang driver links the sanitizer runtime itself, and these presets
are part of the `check` test gate. On **Windows (clang-cl)** the build is wired to link the
dynamic ASan runtime manually (CMake drives `lld-link` directly, bypassing the driver) and dev.py
puts that runtime on `PATH` when launching the binaries — but **clang-cl's ASan is broken with
C++ exceptions**: any `throw`/`catch` faults during exception dispatch (a toolchain bug,
reproducible with a two-line program). Since nexus catches test exceptions, the suite can't be
green under ASan on Windows, so `sanitize-clang` is **excluded from the `check` gate** there. It
remains available for manually ASan-checking exception-free code paths.

## Useful flags

- `--mirror-output` / `--verbose` — global (before the subcommand); stream child output / be chatty.
- `--colored` / `--plain` — global; force or disable colored output. The default auto-detects:
  colored when stdout and stderr are both a terminal, plain when either is piped (e.g. run by an
  agent). In auto mode the `NO_COLOR` / `FORCE_COLOR` environment conventions are also honored.
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
