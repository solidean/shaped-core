# Building and Testing

[dev.py](../../dev.py) is the unified build & test driver for shaped-core.
It wraps CMake + Ninja, discovers targets and tests, captures output, and emits machine-readable results.
This is the full reference.
The `/building-and-testing` skill ([SKILL.md](../../.claude/skills/building-and-testing/SKILL.md)) is the short version you drive during a session.

Run everything from the repo root via `uv` — it resolves the Python deps inline (PEP 723), so there is no venv to set up:

```bash
uv run dev.py <command> [options]
```

## Commands

| Command        | What it does                                                                 |
|----------------|------------------------------------------------------------------------------|
| `configure`    | Configure the CMake project for the selected preset(s).                       |
| `build`        | Build (auto-configures first if inputs changed). `-t/--target` to scope.      |
| `test`         | Build, then run test binaries (`*-test`). Optional name/binary filter.        |
| `test-web`     | Build the Emscripten browser pages and serve them (see [WebAssembly](#webassembly-emscripten)). |
| `run`          | Build one **non-test** executable and run it, forwarding the rest of the command line. |
| `example`      | List the repo's examples, or build and run exactly one ([examples.md](examples.md)).  |
| `format`       | clang-format our C++ sources in place (see [Formatting](#formatting)).        |
| `lint`         | The linting front door: `clang-tidy`, `shaped`, `prose-apply`, `prose-stats`. |
| `check`        | Run pre-commit checks (lint, format, crossrefs, test) and report one green/red verdict. |
| `coverage`     | LLVM source-based test coverage ([coverage.md](coverage.md)).                 |
| `pgo`          | Profile-guided optimization ([pgo.md](pgo.md)).                               |
| `assembly`     | Disassemble a symbol, or trace what one invocation actually ran ([disassembly.md](disassembly.md)). |
| `profiling`    | Report the machine's hardware performance counters ([profiling.md](profiling.md)). |
| `clean`        | Remove a preset's build directory (`--all` for every preset, `--dry-run`).    |
| `diagnose clangd FILE` | Show clangd's diagnostics for a source file (see below).              |
| `info`         | Inspect resolved compile/link flags and per-file compile commands (see below). |
| `doctor`       | Read-only toolchain sanity check: cmake, ninja, compiler, presets, clangd, the LLVM tools coverage and PGO need, the networking environment, and the graphics environment (Vulkan, windowing, DXC). |
| `list-presets` / `list-targets` / `list-toolsets` | What is available: build presets, a preset's CMake targets, the installed compiler toolsets. |

`build` and `test` **auto-configure** when CMake inputs or the source listing change (fingerprinted); pass `--no-configure` to skip.
`test` also builds first; pass `--no-build` to skip.

### Common invocations

```bash
uv run dev.py test                  # build + run the full suite
uv run dev.py test "<pattern>"      # run just tests whose name matches (or a whole *-test binary)
uv run dev.py test vector-test.cc   # a pattern matching no name is retried against the tests' source files
uv run dev.py test -t clean-core-test
uv run dev.py build                 # build everything
uv run dev.py build -t nexus        # build one target
uv run dev.py run shaped-linter --fix libs/base/clean-core/src/clean-core/vector.hh
uv run dev.py run instruction-tracer -- --help   # `--` when a program flag collides with one of ours
uv run dev.py example               # list every example
uv run dev.py example clean-core/vector   # build, resolve and run exactly one
uv run dev.py coverage run          # LLVM test coverage (see guides/coverage.md)
uv run dev.py pgo run               # profile-guided optimization (see guides/pgo.md)
uv run dev.py doctor
```

The positional argument to `test` is smart: naming a test binary runs that whole binary, and anything else is a **test-name substring filter**.
Before running, dev.py asks each `*-test` binary which tests the filter actually selects (via nexus' `--list-tests-json`) and runs **only the binaries that contain a match**.
The others are skipped without ever emitting a "did not select any tests" error.
If the filter matches **nothing in any binary**, the run fails loudly with a diagnostic: the closest test names ("did you mean …"), or, when a name matched but was excluded, the fix.
That fix is to name a disabled test exactly, or to pass the bucket flag for a `manual` / `pgo_benchmark` / `benchmark` / `example` test.
A full sweep (no filter) is unchanged: every binary runs, and an empty one is a failure.

### Selecting by file

A pattern that matches no test *name* anywhere in a binary is retried as a **glob over the tests' source files**.
So you can select the file you are editing, instead of recalling what the tests in it are called.

```bash
uv run dev.py test function_ref-test.cc                        # a bare filename
uv run dev.py test "libs/base/clean-core/tests/memory/*"       # a globbed path — quote it
uv run dev.py test libs/base/clean-core/tests/memory           # a directory means its subtree
uv run dev.py test C:\Projects\shaped-core\libs\base\nexus\tests\test-check-render-test.cc
```

Separators are interchangeable (`\` and `/`, and git-bash's `/c/…` is the same drive as `C:\…`), and matching folds case.
A pattern that is not anchored also matches as a path suffix, which is what makes a bare filename work against the absolute path the compiler recorded.
`*` stays inside one path segment and `**` crosses them, as everywhere else in the repo.

A file match is a *filter*, not an override: it selects exactly the tests an equivalent list of names would, so the bucket and disabled gates still hold — only an **exact test name** opens those.
So `dev.py test hash-benchmark.cc` reports that the file's benchmarks are in the `pgo_benchmark` and `benchmark` buckets.
`dev.py test hash-benchmark.cc --pgo-benchmarks` runs the first of those, and `dev.py benchmark` is how the others are reached.

`--match-files` and `--match-names` (forwarded straight to the runner) pin the reading when the fallback guesses wrong — `--match-files vector` never tries names.

Under bash a pattern like `tests/memory/*` is expanded by the shell into many arguments; those arrive as several filters, which are OR'd, so the selection comes out the same.
Quoting it keeps the glob for nexus to interpret, which is what you want for `**`.

## Chasing a flake: `--repeat N`

```bash
uv run dev.py test shaped-graphics-dx12-test --repeat 100
```

Runs the same selection up to N times and **stops at the first failing iteration**, naming it.
Build and target discovery happen once, so an iteration costs only the binaries — far cheaper than a shell loop around `dev.py`.

Stopping on failure is the point, not a convenience.
Run logs and JUnit XML are written per target and **overwritten every run**, so a loop that keeps going destroys the evidence of the failure it just found.
Halting leaves that iteration's `run-logs/` and `*.results.xml` exactly as the failing run left them, which is what `test_diag` and `--mirror-test-output` then read.

A rate is a different question from a cause, and this answers the cause.
When you do want the rate, loop it in the shell and count — but expect to lose the logs of all but the last failure.

## Presets

Presets live in [CMakePresets.json](../../CMakePresets.json), one per platform × compiler × build type (MSVC / Clang / GCC across Windows / Linux / macOS / Android NDK / Emscripten).
The default is chosen by platform:

| Platform | Default preset                       | Assertions (`CC_ASSERT`) |
|----------|--------------------------------------|--------------------------|
| Windows  | `relwithdebinfo-clang`               | on                       |
| Linux    | `relwithdebinfo-linux-clang`         | on                       |
| macOS    | `macos-arm-llvm-relwithdebinfo`      | on                       |

Override with `--preset`. **It is a per-subcommand flag — it goes *after* the subcommand** (`uv run dev.py test --preset release-clang`).
`--emsdk-path` (for WASM) is likewise per-subcommand.
Only `--verbose`, `--mirror-output`, `--mirror-test-output`, `--collect-logs`, and `--colored` / `--plain` are global, before the subcommand.
The `--profile*` flags of [Profiling a run](#profiling-a-run) are the one exception to the split: they are declared both globally and per-subcommand, so they bind on either side.
Presets accept comma-lists, repeated flags, and shell-style wildcards, and dev.py operates on all that match:

```bash
uv run dev.py build --preset debug-clang,release-clang
uv run dev.py test  --preset "x64-linux-*"
```

**When you touch assertion-gated code, build a `release-*` preset too.**
The default preset only exercises the assertions-on branch the table above names.

### No-PCH presets

Every preset above builds against per-target precompiled headers.
The `nopch-*` and `debug-nopch-*` presets set `CMAKE_DISABLE_PRECOMPILE_HEADERS=ON` instead, which is what catches a source relying on a header only the PCH's `/FI` supplied.
`check` runs `debug-nopch-*` as its debug leg and CI runs `nopch-linux-clang`, so you rarely reach for these by hand.
`uv run dev.py build --preset nopch-clang` reproduces a CI canary failure locally.
[precompiled-headers.md](precompiled-headers.md) is the tiers, how to pick one, and the ordering rule the SDK gates impose.

### Pinning toolset versions

A preset names a compiler *family* (clang / gcc / msvc); the concrete version is otherwise whatever the environment defaults to.
`--toolset` pins a specific one, and is a per-subcommand flag on `configure`, `build`, `test`, `run`, `doctor` and `profiling counters`.
So the same dev.py can drive clang 19 / 20 / 21, or two Visual Studio toolsets, on one machine:

```bash
uv run dev.py test --preset relwithdebinfo-linux-clang --toolset 21    # clang++-21 / clang-21 on PATH
uv run dev.py test --preset relwithdebinfo-linux-gcc   --toolset 14    # g++-14 / gcc-14
uv run dev.py test --preset relwithdebinfo-msvc        --toolset 14.51 # MSVC toolset 14.51 via vcvars
uv run dev.py test --preset relwithdebinfo-linux-clang --toolset /opt/llvm-20/bin/clang++  # explicit path
```

How the value resolves depends on the family.
clang and gcc swap `CMAKE_C/CXX_COMPILER`: a bare version finds `clang++-N` / `g++-N` on `PATH`, and a value with a slash is an explicit compiler path.
msvc selects the Visual Studio instance whose `VC/Tools/MSVC` has that toolset — **including prerelease and preview installs** — and pins it with `-vcvars_ver`.
A toolset that cannot be found is a **hard error**.
Where no versioned `clang++-N` / `g++-N` exists — `clang-cl` on Windows, Homebrew's unversioned `clang++` on macOS — a bare `--toolset N` does something else.
It **asserts** that the preset's own compiler is major version N.
That is a hard error on mismatch, so a base-image compiler bump is loud rather than silent.

`uv run dev.py list-toolsets` prints what is installed per family.
Each Visual Studio instance comes with its MSVC toolsets and the exact `--toolset` value for each, plus the clang and gcc drivers on `PATH`.

A pinned toolset must not share a CMake cache with the default one, so the build directory is **auto-redirected** to `build/<preset>-<toolset>`.
Two flags override where it lands:

- **`--build-suffix <tag>`** → `build/<preset>-<tag>`.
  The go-to for a toolset matrix: one folder per toolset, side by side, nothing clobbered.
- **`--build-dir <path>`** → an arbitrary directory, relative to the repo root or absolute, for a fully custom layout.
  Single preset only.

```bash
# clang version matrix — the auto-redirect already gives each its own tree
# (build/<preset>-19, -20, -21); no --build-suffix needed:
for v in 19 20 21; do
  uv run dev.py test --preset relwithdebinfo-linux-clang --toolset $v
done
```

Reach for `--build-suffix` only when you want a name the auto-redirect would not pick — two builds of the *same* toolset that differ some other way.

### WebAssembly (Emscripten)

The `emscripten-{debug,relwithdebinfo,release}` presets cross-compile to WASM, single-threaded.
They need the [emsdk](https://github.com/emscripten-core/emsdk); point dev.py at it with `--emsdk-path`, `SC_EMSDK_PATH`, or an activated `EMSDK`.
dev.py applies the emsdk environment itself, so no permanent activation is required:

```bash
uv run dev.py test --preset emscripten-relwithdebinfo --emsdk-path /path/to/emsdk
```

The test binaries are `.wasm` plus a `.js` loader; dev.py runs them under emsdk's Node and parses the same JUnit report as native runs.
`uv run dev.py doctor` validates the toolchain, and the full setup and feature knobs are [requirements.md](../requirements.md#emscripten--wasm)'s.

**Browser test runner.** Under Emscripten each `*-test` target also builds a MODULARIZE wasm module (`*-test-web.js` + `.wasm`).
CMake generates HTML pages at the build root: one per library (`<library>-web.html`), plus an aggregate **`tests-web.html`**.
The aggregate loads every library's module and shows them grouped, with a grand total.
Each page runs its tests **one per animation frame**, rendering a live table of per-test timing, assertion counts, and pass/fail.
Open them with:

```bash
uv run dev.py test-web                 # combined page: all libraries
uv run dev.py test-web clean-core      # just one library
```

`test-web` builds the module(s) and serves the page with emsdk's **emrun**, a dev-only static server and browser launcher; Ctrl-C stops it.
It defaults to the `emscripten-relwithdebinfo` preset and takes the same `--emsdk-path` as the other commands.
The pages are plain static files, so hosting them means serving the build root over HTTP from any static server rather than `file://` — emrun is not needed in deployment.

The pages are built by the normal `dev.py build` too, since they are regular targets, and they are not run by `dev.py test`: only `*-test`, not `*-test-web`, is a node test.
The implementation is nexus'.
[web_runner.cc](../../libs/base/nexus/src/nexus/web/web_runner.cc) exposes the `nx_web_*` C ABI and [NexusWebRunner.cmake](../../libs/base/nexus/cmake/NexusWebRunner.cmake) is the build glue.
The page itself is the shared renderer [nexus-web-driver.js](../../libs/base/nexus/web/nexus-web-driver.js).
Its template is [nexus-web-page.html.in](../../libs/base/nexus/web/nexus-web-page.html.in).

## Quiet by default, and how to diagnose

dev.py does **not** stream child output.
For each step it:

- captures stdout/stderr to `build/<preset>/run-logs/run-log-<name>.{stdout,stderr}.txt`,
- writes a JSON sidecar in the build dir (`configure.json` / `build.json` / `test.json`),
- writes a per-binary JUnit `*.results.xml` next to each test binary,
- and, with `--profile`, adds that step to a run-wide job profile ([Profiling a run](#profiling-a-run)),

then prints a one-line trace per step plus a pass/fail summary.
On failure it prints the exact diagnostic selector to run.

So the loop is **dev.py, then diagnose with the `repo_tools` MCP tools**:

| After a... | Tool         | Typical call                                                       |
|------------|--------------|-------------------------------------------------------------------|
| build      | `build_diag` | `build_diag base_path="build/<preset>"`                           |
| test run   | `test_diag`  | `test_diag path="build/<preset>/**/*.results.xml" errors_only=true` |

These read the artifacts dev.py already emitted, which beats scrolling raw logs.
Mirroring is additive to capture, so the logs read the same either way; to watch something live as well, reach for the mirror flags under [Useful flags](#useful-flags).
**Don't pipe dev.py into `tail`/`head`/`grep`.**
The output is already terse, and `… 2>&1 | tail` reports the pipe's exit code (0) — masking a real failure as success.

## Formatting

`uv run dev.py format` runs clang-format over our first-party `.cc`/`.hh` sources, with [.clang-format](../../.clang-format) as the authoritative style.
The scope is a whitelist — `libs/`, `tools/instruction-tracer/` and `tools/shaped-linter/` — so vendored code under `extern/` is never reformatted:

```bash
uv run dev.py format                         # format every libs/ source in place
uv run dev.py format --dirty-only            # only git-dirty/untracked files — the pre-commit move
uv run dev.py format --dirty-only --check-only   # verify without rewriting (exit 1 if any differ)
uv run dev.py format --commit <rev>          # only the sources that commit or range touched
```

`--check-only` rewrites nothing; it lists the non-conforming files and exits non-zero, so it works as a CI or pre-commit gate.
`--dirty-only` restricts the set to what is part of your next commit — modified, staged, or untracked — and the two pair up before committing.
`--commit` asks the same question of an already-committed change set instead — see [Re-checking a commit or a range](#re-checking-a-commit-or-a-range).

clang-format output is not stable across major versions, so the command pins to the major version declared by `.clang-format`'s `Requires: clang-format >= N` header.
When the clang-format it finds is missing or a different major, it fetches the pinned build into `tools/bin/` and uses that.
That is a ~1.5 MB download, once per machine, owned by [tools/bin/fetch-clang-format.py](../../tools/bin/fetch-clang-format.py).
The fetched binary is gitignored, and it outranks PATH precisely because its version is the one we chose.
It **errors** only when that fails too, and `--allow-different-version` downgrades the surviving mismatch to a warning; `SC_SKIP_CLANG_FORMAT_FETCH=1` opts out of the fetch entirely.
Like every other step, the clang-format run is captured under `build/run-logs/`.
Before committing, prefer `uv run dev.py check --fix`, which runs this check and the others in one shot — see [Pre-commit checks](#pre-commit-checks).

## Pre-commit checks

`uv run dev.py check` runs the project's pre-commit gates and reports a single green/red verdict — the "everything green" gate.
**`uv run dev.py check --fix` is the recommended move before committing.**
It auto-applies every unambiguous fix it can (clang-tidy, shaped-linter, then clang-format over what they rewrote), runs the rest, and tells you what is left.

```bash
uv run dev.py check            # run every check -> one verdict
uv run dev.py check --fix      # apply fixable checks (clang-format -i), then report
uv run dev.py check --no-test  # static checks only — skip the build+test tail (docs-only re-check)
uv run dev.py check --all      # widen lint, shaped-lint and format from dirty-only to the whole tree
uv run dev.py check --commit <rev>   # check a commit or range instead of the working tree
uv run dev.py check crossrefs  # run just one (or several) checks by name
uv run dev.py check --list     # list the registered checks
```

Registered checks, **in the order they run**:

| Check        | What it does                                                                   | `--fix`? |
|--------------|--------------------------------------------------------------------------------|----------|
| `lint`       | clang-tidy whitelist gates on `.cc` sources. Dirty-only by default; `--commit` or `--all` to rescope.  | yes (applies clang-tidy fixes) |
| `shaped-lint`| shaped-linter's own rules on `.cc`/`.hh`/`.md`/`.py`. Dirty-only by default; `--commit` or `--all` to rescope. | yes (applies its suggested fixes) |
| `format`     | clang-format our C++ sources. Dirty-only by default; `--commit` or `--all` to rescope. | yes (rewrites in place) |
| `crossrefs`  | Validate doc↔code cross-references repo-wide (always full-repo).                 | no (report only) |
| `test`       | Build + run the full suite on the debug, default, release, single-threaded **and** (Linux/macOS) sanitizer presets. | no (report only) |

**That order is a correctness property, not a listing convention.**
A lint fix is a byte-range edit — dropping a `cc::` qualifier shortens a line and strands the continuation lines aligned under where it used to end.
A clang-tidy fix can also land in a header the commit had not otherwise touched, which `format` only sees as dirty once that write has happened.
Running `format` after both fixers is what makes `check --fix` one finished pass instead of a pass plus "now go run format".
Naming checks explicitly does not change it: `check format lint` runs `lint` first, same as the full gate.

`lint` runs the clang-tidy gates via [tools/lint/clang-tidy.py](../../tools/lint/clang-tidy.py).
The gates are a strict, must-be-zero whitelist in [tools/lint/clang-tidy-gates.yml](../../tools/lint/clang-tidy-gates.yml).
That whitelist is deliberately distinct from the root `.clang-tidy` clangd reads, which stays the broader IDE incubator; a check graduates into the gate once the tree is clean under it.
Only `.cc` translation units are linted — a bare header has no compile-database entry, so its diagnostics surface through the `.cc` that includes it.
Dirty-only by default so gates adopt incrementally; `--all` widens to the whole tree, `--commit` retargets it at a commit, and `--fix` lets clang-tidy rewrite.
`shaped-lint` is the same shape over shaped-linter's own rules, which cover `.cc` / `.hh` / `.md` / `.py` — code *and* prose.
`dev.py lint` is the front door for both, plus `prose-apply` and `prose-stats`; the prose half is [prose.md](prose.md)'s.

`crossrefs` scans markdown links — `[text](path#L42)`, including line and heading anchors — and the references inside comments, then flags any that no longer resolve.
Comments count in every language we write prose in: `//` in `.cc`/`.hh`, `#` and docstrings in `.py`, `#` in `CMakeLists.txt` / `*.cmake`.
A comment reference is either a bare path ending in `.md` or a markdown link, whose target may have any extension.
Its scope is `libs/`, `docs/`, `tools/`, `.claude/` and the root meta files.
These rot silently, because a moved file breaks links in *other*, untouched files, so the scan is always full-repo and `--all` does not affect it.
It reports each offender as `file:line: reason`.

`test` is the slow tail and runs **only after the static checks pass** — no point building a tree that already fails a cheap lint — and `--no-test` skips it.
It builds and runs the suite across five build variants:

- **debug** — `-O0` plus mimalloc's `MI_DEBUG` heap, `CC_ASSERT` on, and **precompiled headers off**
  (`debug-nopch-*`, the cheapest of the four to give a PCH up on, so a source that dropped an include the PCH's `/FI` was supplying fails here — see [precompiled-headers.md](precompiled-headers.md))
- the platform default **relwithdebinfo** — `CC_ASSERT` on
- the **release** sibling — `CC_ASSERT` off
- a **single-threaded** sibling — `SC_THREADS=OFF`, otherwise reachable only through a wasm build
- on **Linux and macOS**, a **sanitizer** preset — ASan + UBSan

So assertion modes, the debug allocator, the threading fallback and undefined behavior are all covered every commit.
Builds are warm at commit time — code you didn't compile, you didn't test — so the real cost is the test run, well under a second; expect a one-time cold build of the extra presets.
On failure it prints the `test_diag` selector, same as `dev.py test`.
Windows has no sanitizer leg — see [Sanitizers](#sanitizers) for why.

New gates plug into the check registry in [cmd/check.py](../../tools/dev/cmd/check.py) without changing the command surface.

### Re-checking a commit or a range

Dirty-only is exactly right *before* a commit and useless *after* one.
Once the work is committed the working tree is clean, so a re-check finds nothing to check and reports green without having looked at anything.
`--commit` is the answer: it takes the changed files, and the changed lines, from a commit instead of from the working tree.

```bash
uv run dev.py check --commit c24d3581        # everything that merge brought in
uv run dev.py check --commit main..HEAD      # a branch's worth of work
uv run dev.py lint shaped --commit HEAD      # one gate, same scope
```

**A single commit means its first-parent diff**, so pointing it at a merge gives you everything the merge brought in rather than only the conflict resolutions.
That is the case it was built for: a merge lands on `main` and you want the gates re-run over the whole merged-in change set.
A range is handed to git as given, so `A..B` and `A...B` both behave as they do everywhere else.

Two things to know before trusting a result:

- **A bad revision is a hard error, not an empty run.** A typo'd hash would otherwise scope every gate to zero files and report green, which is the one failure a re-check must never produce.
- **Line ranges come from the commit, but the linters read files from disk.**
  If the tree has moved on since, prose findings are filtered against stale line numbers, and the run warns, naming the drifted files.

`--all` and `--commit` are mutually exclusive, as are `--dirty-only` and `--commit` on `format` and `lint`; each names a different change set and only one can win.
`crossrefs` ignores the scope entirely and stays repo-wide, because a moved file breaks links in other, untouched files.

## clangd / IDE code intelligence

The presets set `CMAKE_EXPORT_COMPILE_COMMANDS`, so every configure emits a `compile_commands.json` into the per-preset build dir (`build/<preset>/`).
`configure` **publishes** the active preset's database to `build/compile_commands.json` after a successful run.
[.clangd](../../.clangd) points clangd's `CompilationDatabase` at the `build` **directory**, so it picks that file up.
Two things matter here and are easy to get wrong:

- `CompilationDatabase` is a **directory**, not a file path.
  Pointing it at `build/compile_commands.json` makes clangd silently fall back to bare flags — no includes, wrong language standard — and flag every line.
- The published file is what makes clangd work whether you configured through `dev.py` or the VSCode CMake Tools extension.
  With multiple presets, the last one configured wins that single published database.

If clangd reports errors in the editor that don't match reality, reproduce them from the CLI:

```bash
uv run dev.py diagnose clangd libs/base/nexus/src/nexus/run.cc
uv run dev.py diagnose clangd <file> --preset debug-clang   # force a specific preset's database
```

This runs `clangd --check` and prints every diagnostic as `file:line: severity: message [code]`, exiting 1 if any are errors.
By **default it uses clangd's own discovery** (`.clangd` plus upward search), which is exactly what the editor does — so a misconfigured `.clangd` shows up here rather than being masked.
It warns loudly when clangd falls back to generic flags.
Pass `--preset` to force that preset's per-preset database instead.
`clangd --check` also self-tests refactor tweaks and inflates its own "N errors" summary with those failures; `diagnose clangd` ignores that noise and reports only real code diagnostics.
`doctor` runs the same discovery-based check on a sample file, to confirm clangd is wired up end to end.

A common first move when the editor looks wrong: reload the clangd language server, since the published database or a just-edited `.clangd` can be stale relative to the running server.

## Inspecting build/link flags

When a build misbehaves, the first question is usually *what flags is CMake actually passing?*
`info` answers that read-only, straight from the configured preset — it auto-configures if needed, and no build is required.
There are two layers:

```bash
uv run dev.py info build-flags clean-core-test    # per-target compile settings
uv run dev.py info link-flags  clean-core-test    # per-target linker flags + link libraries
uv run dev.py info compile-command libs/base/clean-core/src/clean-core/common/assert.cc
```

- **`build-flags` / `link-flags`** read the CMake **File API**, the same source target discovery uses.
  `build-flags` prints defines, includes (`[sys]` marks system includes), the language standard, and the raw flag fragments.
  Crucially, flags can **differ per translation unit** — a vendored source might be built with `/W0`.
  CMake groups a target's sources by flag-set, so where they diverge `build-flags` prints one numbered *flag set* block per group, with the sources each covers.
  Averaging them into one line would be misleading.
  `link-flags` prints the linker flags and the resolved link libraries; a static library, which is archived rather than linked, says so.
- **`compile-command FILE`** prints the **exact** command from `compile_commands.json` — literally what the compiler is invoked with for that one file, the ground truth after the File API.
  By default it lists one argument per line for readability; `--raw` prints the verbatim single line.
  The file argument matches an absolute path, a repo-relative path, or a unique bare filename.

The target argument to `build-flags` / `link-flags` accepts comma-lists, repeated values, and shell-style wildcards (`"clean-core*"`), like `--target` elsewhere.
Use `--preset` to inspect a specific configuration.

## Profiling a run

`--profile FILE` records **where a dev.py invocation's wall-clock time went** and writes it out on exit, pass or fail.
It answers the question the per-step trace cannot.
A `build` line saying `7534 ms` does not say which translation units those 7.5 seconds went into, or how much of the run never reached the compiler at all.

```bash
uv run dev.py check --fix --profile .tmp/dev-profile/check.json --profile-type chrome-tracing
uv run dev.py build --profile .tmp/dev-profile/build.json
```

Unlike every other flag here, `--profile` / `--profile-type` / `--profile-lanes` bind **on either side of the subcommand**.
Profiling is cross-cutting, and demanding a position for it is friction with no upside.

Every profiled run prints its own summary, so the common question is answered without opening anything:

```
Profile written to .tmp/dev-profile/check.json (1918 job(s))
  leaf jobs       count        sum       span    par
  compile          1751   2560.5 s     85.5 s  30.0x
  test               52     33.9 s     33.9 s   1.0x
  link               63     11.2 s      4.4 s   2.5x
  env                 8      8.8 s      8.8 s   1.0x
  configure           3      3.2 s      3.2 s   1.0x
  all leaves       1900   2620.1 s    130.4 s  20.1x

  containers      count        sum       span   (time already counted above)
  invocation          1    135.4 s    135.4 s
  check-gate          5    135.4 s    135.4 s
  build               5     86.9 s     86.9 s
```

**Two tables, because only one of them adds up.**
A **leaf** is work; the rows are disjoint, so `all leaves` is a real total.
A **container** is a span that encloses other jobs: `invocation` is the whole run, `check-gate` is one gate, `build` is the step the compiles happened inside.
Its time is its children's, so adding it to the leaves would double-count.
Mixing them is what makes `invocation` look like the expensive part when it is only the outermost one.

Only dev.py's own spans can be containers, and that restriction is deliberate.
A six-second compile encloses a fast one that ran on another core, which is two cores rather than structure.
Reading it as structure would file most of a build's fan-out under `containers`, leaving the leaf table reporting a fraction of the real work.

**`sum` and `span` answer different questions, and `span` is the one to act on.**
`sum` adds every job up, so 1751 compiles read as 2560 s of work.
`span` counts overlap once, so the same 1751 read as the 85.5 s of wall clock they are actually responsible for.
`par` is the ratio — 1.0 is serial, and above that is how many ran at once on average.
A type with a large `sum` and a small `span` is already parallel and not the thing to fix; a type whose `span` approaches the run's is.

`env` is the worked example, and it is what the summary above was captured before.
8.8 s of a 135 s run at `1.0x`, all of it re-deriving the same MSVC environment, because configure and build each asked for it independently once per preset.
It is cached on `(toolset, arch)` now and shows up as a single capture, so a run like that one reads `env 1 2.6 s`.
[notes/build-times.md](../notes/build-times.md) carries that measurement and the others it came with — read it before optimizing anything here, since it says which scenario each cost belongs to.

### What becomes a job

A **job** is one thing with a wall-clock start and an end.
Five sources feed the profile, and none of them needs the build reconfigured:

| type | one job per | where it comes from |
|------|-------------|---------------------|
| `configure` / `build` / `test` / `lint` / `format` / `run` | captured subprocess | `run_step`, the single choke point every child goes through |
| `compile` / `link` | translation unit, and linked binary | the `<output>.diag.json` sidecars `diag-launcher` already writes next to every artifact |
| `clang-tidy` | linted `.cc` | the gate runner's own thread pool, reported back through a fragment file |
| `check-gate` | pre-commit gate | the `check` registry |
| `env` / `git` / `discover` / `fingerprint` / `probe` / `prereq` / `crossrefs` | in-process phase | spans around the work that spends real time without spawning a step |

That last row is the reason to reach for this at all.
MSVC environment capture, `git status`, repo fingerprinting and the per-binary `--list-tests-json` probes are charged to nothing in the step trace.
On a multi-preset `check` they add up.

Compile and link jobs are harvested by diffing the sidecars against a mark taken before each build step, so an up-to-date build contributes none and only what actually rebuilt is timed.
Emscripten presets have no launcher and fall back to the tail of `.ninja_log`, whose edge times are relative to the ninja invocation and are anchored against the step's own end.
That is slightly less exact, and the only path for WASM.

### Two timelines, laid out differently

A trace has **two processes**, because the two halves of a run are shaped differently and squashing them together reads badly.

* **`dev.py`** — everything the driver timed on its own call stack, drawn as a proper nested flame chart with one row per depth.
  This nesting is exact rather than reconstructed: dev.py is single-threaded, so a span really does contain what ran inside it.
  The top row is the invocation, with the gates, steps and phases nested underneath.
* **`jobs`** — everything that fanned out under a step: compile edges, link steps, the per-file lint.
  These have no thread to attribute them to and overlap arbitrarily, so their rows are **reconstructed**: jobs are sorted by start and greedily packed into the fewest lanes their overlap requires.

The two are always allocated independently, so a compile edge is never pushed down a row by the step that spawned it, and either block can be collapsed in the viewer to look at the other.

Every track is **strictly ordered** in the emitted trace, which matters more than it sounds.
A viewer nests two slices on one track the moment they overlap by a single microsecond, drawing the second as a sub-row of the first.
One rounding artefact therefore turns a clean lane into a staircase that reads as a broken layout.
Timestamps come from a monotonic counter anchored once to the wall clock, rather than from the system clock.
Windows quantizes the latter to ~15 ms, which is coarse enough to order two sequential steps wrongly.
On top of that the exporter trims any overlap that remains, since a merged profile mixes clocks it cannot control.
Only the rendered slice is adjusted, never a recorded job, so the summary's numbers stay exact.

`--profile-lanes global` (the default) packs the fan-out into one pool.
`--profile-lanes per-type` gives each fanned-out job type its own pool and its own track — worth it once one kind of work is what you are chasing.
The `dev.py` process is unaffected either way, since depth is not a thing to allocate.

### Formats, and composing runs

`--profile-type jobs` (the default) writes the raw records: `name`, `type`, `start`, `end`, `extra`, plus `origin`, `leaf` and the allocated `lane` for convenience.
`--profile-type chrome-tracing` converts them to Chrome Trace Event Format, which <https://ui.perfetto.dev> loads directly and `analyze_trace` reads too.

Times are **absolute epoch seconds**, so profiles from separate invocations compose without renormalizing:

```bash
uv run dev.py profiling merge .tmp/dev-profile/*.json --out .tmp/dev-profile/all.json --type chrome-tracing
```

Lanes are re-allocated over the whole merged set, and a single input is therefore also the converter — record once as `jobs`, render it as a trace whenever.

The pure half of this — containment, lane allocation, the summary arithmetic and the trace export — has its own invariant test.
These are the parts that fail quietly: a lane that double-books two overlapping jobs makes parallel work read as serial, and a container misidentified drops real work out of the leaf table.

```bash
uv run tools/dev/profile-self-test.py
```

The change-set discovery behind `--dirty-only` and `--commit` has one for the same reason — a scope that resolves to nothing reports green without having looked at a line.

```bash
uv run tools/dev/changes-self-test.py
```

## Tests (nexus)

Test executables follow the convention `<lib>-test` and are built on **nexus**, which is Catch2 v3 CLI–compatible.
[nexus' catch2-runner-compat](../../libs/base/nexus/docs/catch2-runner-compat.md) is the reference for how they are discovered, filtered and bucketed.
dev.py discovers them via the CMake File API, runs each with the optional name filter, and synthesizes a JUnit result from the process.
So a binary that **crashes before printing anything** is still recorded as a failure, on its non-zero exit; `--mirror-test-output` shows whatever it printed before dying.

Never run a test binary directly — always go through `dev.py test`, so discovery, capture, and result recording stay consistent.

## Examples (`example`)

Example executables follow the convention `<lib>-<name>-example` and are built on nexus like the tests, but in the `example` bucket: every build compiles them and no sweep ever runs one.
[examples.md](examples.md) is the concept — what an example is for, how to declare one, and where it lives.

```bash
uv run dev.py example                     # list every example, grouped by binary
uv run dev.py example clean-core/vector   # build, resolve and run exactly one
```

Each example runs in its own source directory, so a relative path in one resolves next to the example rather than next to wherever `dev.py` was invoked.

Resolution is a **name** lookup across binaries, not a target lookup: every `*-example` target is built, then probed with nexus' `--list-tests-json - --examples`.
An exact name wins; otherwise a case-insensitive substring must select exactly one, and anything else is an error naming the candidates.
`--target` narrows which binaries are probed, and the match never does.
Listings are cached under `build/<preset>/example-listings.json` and revalidated by mtime and size, so only a rebuilt binary is re-probed.

There is deliberately no `--all`: running the whole corpus would open every window it has.

## Running a non-test executable (`run`)

`dev.py run <target> [args…]` is the counterpart for everything that is *not* a test: shaped-linter, instruction-tracer, a sample.
It builds the target first, resolves its artifact for the preset, and runs it with the program's output mirrored live.
**Its exit code is propagated verbatim**, so a tool's non-zero "I found something" reaches the caller intact.

```bash
uv run dev.py run shaped-linter --fix path/to/file.cc
uv run dev.py run shaped-linter --preset release-clang path/to/file.cc   # any preset
uv run dev.py run instruction-tracer -- --help                          # `--` for a colliding flag
```

Reach for it instead of hand-writing `build/<preset>/tools/…/foo.exe`: that path hard-codes one preset and happily runs a stale binary when the build is behind.

Everything dev.py does not recognize is forwarded to the program, so `--fix` lands on the tool while `--preset` still binds to dev.py.
A program flag that collides with one of dev.py's own needs the `--` separator.
`--quiet` captures the output to the step log instead of mirroring it.
`--timeout SECS` bounds a program that might hang; it defaults to `0`, meaning no limit — unlike `test --timeout`, which defaults to 60.

`run` **refuses `*-test` targets** and points at `dev.py test` — bypassing discovery and result recording is exactly what the rule above forbids.
It refuses `*-example` targets the same way, pointing at `dev.py example`, which resolves an example name across every example binary rather than making you find the one that holds it.

## External dependencies (`deps`)

`dev.py deps` reports what we pin under `extern/` and collects the licenses we ship.
[dependencies.md](dependencies.md) is the full guide, including the manifest schema and the bump workflow.

```bash
uv run dev.py deps list              # every dependency, its pin, and what upstream offers now
uv run dev.py deps list --offline    # manifests and installed pins only, no network
uv run dev.py deps licenses          # regenerate docs/licenses/
uv run dev.py deps licenses --check  # verify without writing; what `check` runs
```

Each dependency's pin lives in its own `extern/<dep>/dependency.yml`, which is what the vendor and fetch scripts read too.
`list` reaches the network by default and caches for a day, so it stays out of `check`; `licenses --check` reaches none, so it is a gate.

Worth knowing about `list`: its second block reports each fetched dependency as **current**, **stale** or **not fetched**.
A `.install/` at the wrong pin is otherwise invisible, and is the thing that makes a build quietly use a version nobody chose.

## Sanitizers

The `sanitize-*` presets are Debug builds with AddressSanitizer + UndefinedBehaviorSanitizer
(`SANITIZE=address,undefined`, wired in the root [CMakeLists.txt](../../CMakeLists.txt)):

```bash
uv run dev.py test --preset sanitize-linux-clang   # Linux
uv run dev.py test --preset sanitize-macos-arm-llvm # macOS
uv run dev.py test --preset sanitize-clang          # Windows (see caveat)
```

On **Linux and macOS** the clang driver links the sanitizer runtime itself, and these presets are part of the `check` test gate.
On **Windows (clang-cl)** the build links the dynamic ASan runtime manually, with CMake driving `lld-link` directly and bypassing the driver.
dev.py then puts that runtime on `PATH` when launching the binaries.
But **clang-cl's ASan is broken with C++ exceptions**: any `throw`/`catch` faults during exception dispatch, a toolchain bug reproducible with a two-line program.
Since nexus catches test exceptions, the suite can never be green under ASan on Windows, so `sanitize-clang` is **excluded from the `check` gate** there.
It stays available for manually ASan-checking exception-free code paths.

Two things these presets change beyond turning the sanitizers on, both worth knowing before you read a report.

**The allocator is different.** They set `SC_MIMALLOC=OFF`, so `cc::default_memory_resource` is `cc::system_memory_resource`.
Every allocation then goes through the global `operator new`, which is what these tools actually intercept.
Without that, LeakSanitizer never scans mimalloc memory and reports anything owned only by a `cc::` container as a direct leak.
The cost is that allocation behaviour is not what the other presets do — in-place resize always declines — so a bug that only reproduces here may be the allocator rather than the code.
[platforms.md](../platforms.md#default-allocator-sc_mimalloc) owns the knob, and it is independent of `SANITIZE`: sanitizing a mimalloc build stays available for when mimalloc itself is the suspect.

**Vendored code is excluded.** [tools/cmake/sanitizer-ignorelist.txt](../../tools/cmake/sanitizer-ignorelist.txt) suppresses findings attributed to `extern/`.
We cannot fix those without diverging from upstream, and a finding nobody will ever act on trains the reader to scroll past the next one.
Only attribution is suppressed: our own code stays fully instrumented, including the calls it makes into those libraries.
The flag is not wired for clang-cl, so the Windows sanitize preset still reports them.

## Useful flags

- `--mirror-output` / `--verbose` — global (before the subcommand); stream child output / be chatty.
- `--mirror-test-output` — global; stream only the test binaries live, staying quiet through configure and build.
  The usual choice when you want a binary's own output, such as a benchmark table, without the build wall.
- `--colored` / `--plain` — global; force or disable colored output.
  The default auto-detects: colored when stdout and stderr are both a terminal, plain when either is piped, such as a run driven by an agent.
  In auto mode the `NO_COLOR` / `FORCE_COLOR` environment conventions are also honored.
- `--no-configure`, `--no-build` — skip the automatic steps.
- `--keep-going` / `-k` (on `build`) — ninja `-k 0`: keep going after a failure, so one run surfaces every independent error instead of stopping at the first.
- `--diag-archive FILE` (on `build`) — zip the build's diagnostic sidecars, **even when the build failed**.
  `build_diag` reads the archive directly, so this pairs with `--keep-going` to turn one red build into one artifact — which is what CI does.
- `--timeout SECS` (on `test`) — per-binary timeout, default 60; `0` disables.
  A binary that exceeds it is killed and reported as failed — but not before it is asked where it was.
  dev.py provokes clean-core's crash handler first and gives it two seconds to write, so the step's **stderr log** holds the running test, plus a stack for every thread in the process.
  In a hang the stack you want is under `other threads`; the faulting one is dev.py's doing and says nothing.
  The log spells that out, because the report announces a fatal fault that never really happened.
- `--merged-xml-report FILE` / `--no-xml-reports` (on `test`) — merge per-binary XML into one file, or skip XML entirely.
  Per-binary XML is on by default and is what `test_diag` reads, so you usually need neither.
- `--profile FILE`, with `--profile-type` and `--profile-lanes` — record where the run's wall clock went; see [Profiling a run](#profiling-a-run).
  The only flags that bind on either side of the subcommand.

## Related

- [.claude/skills/building-and-testing/SKILL.md](../../.claude/skills/building-and-testing/SKILL.md) — the skill this doc backs.
- [examples.md](examples.md) — the `example` command's concept half: what an example is for, and how to declare one.
- [prose.md](prose.md) — `dev.py lint`'s prose half: `prose-apply`, `prose-stats`, and reworking a surface.
- [profiling.md](profiling.md) — the *other* profiling: what the CPU spent running our C++, rather than what the build spent producing it.
- [docs/coding-guidelines.md](../coding-guidelines.md) — how to write the code you're building.
