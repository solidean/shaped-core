# Test Coverage

`uv run dev.py coverage` collects **LLVM source-based coverage** for the test
suite: it builds an instrumented variant, runs the tests to produce raw counter
data, merges it, and post-processes it into a machine-readable JSON report plus a
short terminal summary. This is the full reference; the loop itself is three
subcommands — `run`, `merge`, `report`.

Why LLVM source-based coverage: every default preset is Clang-based (`clang-cl`
on Windows, `clang` on Linux, Homebrew `llvm` clang on macOS), so one toolchain —
`-fprofile-instr-generate -fcoverage-mapping` → `llvm-profdata` → `llvm-cov` —
works everywhere, and `llvm-cov export` emits JSON natively.

```bash
uv run dev.py coverage run            # build instrumented, run tests, merge, report
uv run dev.py coverage run --html     # also write a browsable HTML report
uv run dev.py coverage run "vector"   # scope to matching tests (same filter as `dev.py test`)
uv run dev.py coverage report         # re-post-process the last run (no test re-run)
uv run dev.py coverage merge --preset coverage-clang --preset coverage-linux-clang
```

## How it works

Coverage instrumentation is a build-time concern, gated by the `SC_COVERAGE`
CMake option (in the root [CMakeLists.txt](../../CMakeLists.txt)). When ON it
adds the LLVM coverage flags to **every** target repo-wide; it's off for normal
builds. You don't set it by hand — the dedicated **`*-coverage` presets** turn it
on. They are RelWithDebInfo (matching the default tested preset, so the suite is
green) and configure into their own `build/<preset>/` dir, keeping instrumented
artifacts out of your normal builds.

| Platform | Coverage build preset       | Configures into                            |
|----------|-----------------------------|--------------------------------------------|
| Windows  | `coverage-clang`            | `build/x64-windows-clang-ninja-coverage/`  |
| Linux    | `coverage-linux-clang`      | `build/x64-linux-clang-ninja-coverage/`    |
| macOS    | `coverage-macos-arm-llvm`   | `build/macos-arm-llvm-coverage/`           |

`coverage run` (with no `--preset`) uses the platform default above. The pipeline
per preset:

1. **build** the instrumented preset (skip with `--no-build` / `--no-configure`);
2. **run** each `*-test` binary with a distinct `LLVM_PROFILE_FILE`, dropping raw
   `coverage/profraw/<name>-<pid>.profraw` (this reuses the normal test runner,
   so JUnit XML and `test.json` are produced exactly as for `dev.py test`);
3. **merge** the raw files with `llvm-profdata merge -sparse` into
   `coverage/coverage.profdata`;
4. **export** with `llvm-cov export` into the JSON sidecar (below), and — with
   `--html` — an `llvm-cov show` report under `coverage/html/`.

By default the report ignores test and vendored code
(`-ignore-filename-regex` matching `extern/` and `tests/`), so the numbers
describe the **library sources** under `libs/`, not the tests exercising them.

## Output: sidecars and the `coverage_diag` contract

Like every dev.py step, each tool invocation is captured to
`build/<preset>/run-logs/`. Two sidecars land in the build dir:

| File                    | What it is                                                                 |
|-------------------------|---------------------------------------------------------------------------|
| `coverage.llvm-cov.json`| The raw `llvm-cov export` JSON — full per-file region/line/function detail. |
| `coverage.json`         | dev.py metadata: the steps that ran, the `ignore_regex`, and distilled overall + per-library totals. |

The **`.llvm-cov.json` suffix is deliberate**: it's the discoverable artifact a
future `coverage_diag` MCP tool reads, exactly as `build_diag`/`test_diag` read
`build.json`/`test.json` today. Build tooling on top of it rather than parsing
terminal output.

The terminal summary prints overall line/function/region percentages and a
per-library line-coverage table:

```
Coverage [coverage-clang]: lines 89.6% (3555/3968), functions 92.1%, regions 67.5%
  libs/base/clean-core             90.9%  (2679/2948 lines)
  libs/base/nexus                  85.9%  (876/1020 lines)
  JSON: build/x64-windows-clang-ninja-coverage/coverage.llvm-cov.json
```

## Subcommands

| Subcommand | What it does                                                                          |
|------------|--------------------------------------------------------------------------------------|
| `run`      | Build + run instrumented tests, merge, and report. The all-in-one. Optional name/binary filter; `--target`, `--no-build`, `--no-configure`, `--html`, `--timeout`. |
| `report`   | Re-export JSON/HTML/summary from the existing `coverage.profdata` — no test re-run. Cheap iteration on the report (e.g. add `--html`). |
| `merge`    | Combine several presets' `coverage.profdata` into one report under `--output` (default `build/coverage-merged/`). Each preset must already have a `run`. |

`merge` is for combining coverage from multiple build configurations or accumulated
runs — e.g. union the Windows and Linux numbers in CI before applying a gate.

## Tooling

`llvm-profdata` and `llvm-cov` must be present and **version-matched to the clang
that built the binaries** (a mismatch silently corrupts the mapping). dev.py
resolves them via PATH / the `LLVM_PROFDATA` / `LLVM_COV` env overrides, then
falls back to the directory of the configured compiler (where clang-cl ships them
on Windows). `uv run dev.py doctor` reports both with their versions.

## Related

- [building-and-testing.md](building-and-testing.md) — the build/test driver this builds on.
- [CMakePresets.json](../../CMakePresets.json) — the `*-coverage` presets and `SC_COVERAGE`.
