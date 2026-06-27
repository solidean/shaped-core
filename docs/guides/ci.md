# Continuous integration

GitHub Actions gates the Tier-1 native toolchains on every push to `main` and
every pull request (and on manual `workflow_dispatch`). (Back to
[_index.md](_index.md).)

## Philosophy

Two ideas drive the layout:

- **Lean on `dev.py`.** CI drives `dev.py` for everything it can — build, test,
  doctor — rather than calling `cmake`/`ctest` directly or hand-rolling shell.
  Explicit flags are fine (and preferred over implicit defaults): a step like
  `uv run dev.py test --preset relwithdebinfo-linux-clang --merged-xml-report …`
  is something a developer can paste verbatim and reproduce locally. The less CI
  does that *isn't* just a `dev.py` invocation, the smaller the gap between
  "green on CI" and "green on my machine". So workflows stay thin: provision the
  toolchain, then drive `dev.py`.

- **Wide but shallow, with one deep punch.** Most workflows cover a *platform*
  at a single config (RelWithDebInfo) — broad surface area, cheap. One workflow,
  **Linux Clang**, runs the *deep* matrix (debug / relwithdebinfo / release) to
  exercise the define interactions a single config can't — chiefly `CC_ASSERT`
  on (debug, relwithdebinfo) vs off (release), plus optimization-level
  differences. Linux clang carries the deep matrix because it's the fastest,
  lowest-setup runner.

## Setup

One workflow per platform/compiler, so each gets its own status badge in the
[README](../../README.md):

| Workflow                                                              | Runner           | Preset(s)                                                  |
|-----------------------------------------------------------------------|------------------|------------------------------------------------------------|
| [ci-linux-clang.yml](../../.github/workflows/ci-linux-clang.yml)      | `ubuntu-26.04`   | `debug-linux-clang`, `relwithdebinfo-linux-clang`, `release-linux-clang` (deep matrix) |
| [ci-linux-gcc.yml](../../.github/workflows/ci-linux-gcc.yml)          | `ubuntu-26.04`   | `relwithdebinfo-linux-gcc`                                  |
| [ci-windows-clang.yml](../../.github/workflows/ci-windows-clang.yml)  | `windows-latest` | `relwithdebinfo-clang`                                      |
| [ci-windows-msvc.yml](../../.github/workflows/ci-windows-msvc.yml)    | `windows-latest` | `relwithdebinfo-msvc`                                       |
| [ci-macos-clang.yml](../../.github/workflows/ci-macos-clang.yml)      | `macos-latest`   | `macos-arm-llvm-relwithdebinfo`                            |
| [ci-wasm-emscripten.yml](../../.github/workflows/ci-wasm-emscripten.yml) | `ubuntu-24.04`   | `emscripten-relwithdebinfo`                                 |

Every workflow shares the same shape: provision the toolchain, then `doctor` →
`build` → `test` through `dev.py`, always with an **explicit `--preset`** (CI
never relies on the platform-default preset), and upload a **diagnostics
artifact** (see below).

### Diagnostics artifacts

`dev.py` is quiet by default — it captures compiler/test output to files under
`build/<preset>/` rather than the console — so a red CI job shows only a terse
"build failed" line, not the actual error. Each job therefore uploads a
`<platform>-diagnostics` artifact (always, even on failure) with three things:

- **`ci-diag.zip`** — every `.diag.json` sidecar, one per compile/link, written
  by the diag launcher (`diag-launcher.exe` on Windows, `diag_launcher.sh` →
  `diag_launcher.py` on Linux/macOS, wired in as the CMake compiler/linker
  launcher). This is the build-step analogue of the JUnit report. Produced by
  `dev.py build --diag-archive`, even when the build fails. CI builds also pass
  `--keep-going` (ninja `-k 0`) so a single failing run surfaces *every*
  independent error, not just the first — otherwise each red run reveals only one
  error and fixing portability becomes a slow one-at-a-time loop.
- **`ci-test-results.xml`** — the merged JUnit test report
  (`dev.py test --merged-xml-report`).
- **`ci-logs.zip`** — the raw captured run logs and step sidecars, the last
  resort when the structured sidecars don't explain it. Produced by the global
  `dev.py --collect-logs`, which fires on exit regardless of pass/fail. It is set
  on *both* the build and test steps so either failure mode is covered: the logs
  under `build/<preset>/run-logs/` accumulate across configure→build→test, so the
  test step's archive is a strict superset of the build step's, and when the
  build fails the test step is skipped — leaving the build step's archive (with
  the failure logs) as the uploaded one.

To use them locally: `gh run download <run-id>`, then **extract `ci-diag.zip` at
the repo root** (its entries are `build/<preset>/…`) and point `build_diag` at
that preset:

```bash
gh run download <run-id> --name linux-gcc-diagnostics
unzip -o ci-diag.zip            # recreates build/<preset>/**/*.diag.json
# then, via the repo_tools MCP: build_diag base_path="build/<preset>"
```

### Toolchains

Most jobs use the **runner's preinstalled toolchain** — clang/gcc, CMake, and
Ninja all ship on the GitHub images — so the only provisioning is installing
`uv` (the `dev.py` runner). Per-platform specifics:

- **Linux** (`ubuntu-26.04`) ships clang 20/21/22 and gcc 13/14/15 side by side.
  The presets call bare `clang`/`clang++` / `gcc`/`g++`, so the only setup is
  symlinking those names — clang to the **21.x** binaries (our
  [requirements.md](../requirements.md) LLVM 21 target), gcc to **14** (≥ the
  GCC 13 floor).
- **Windows** (`windows-latest`) ships **LLVM 20** plus a recent Visual Studio
  (currently VS 18 / MSVC 19.51) with CMake + Ninja; `clang-cl` / `cl` are
  already reachable. No MSVC-setup step is needed — `dev.py` locates the MSVC
  environment via `vswhere` and injects it. The clang job riding LLVM 20 (a touch
  behind the 21 target) is fine because the clang-format/tidy gate runs on Linux
  (clang 21), so Windows clang only needs to *compile* clean. The MSVC (`cl`) job
  builds and tests the `relwithdebinfo-msvc` preset on the same runner.
- **macOS** (`macos-latest`, arm64) needs Homebrew LLVM — the `macos-arm-llvm-*`
  presets point at `/opt/homebrew/opt/llvm` and link Homebrew `libc++` — so it
  `brew install llvm ninja` (CMake ships on the runner).
- **WASM** uses the official `emscripten-core/setup-emsdk` action, pinned to a
  fixed Emscripten version (`EMSCRIPTEN_VERSION`) with the emsdk cached across
  runs. `dev.py` gets `--emsdk-path "$EMSDK"` on doctor/build/test; tests run
  under Node.

`doctor` runs first on every job but is **informational, non-gating**
(`continue-on-error`): it also probes clangd's compile database and
`llvm-cov`/`llvm-profdata`, which aren't needed for a build+test gate and would
otherwise fail a fresh runner. It runs purely so the toolchain state is visible
in the log.

## Diagnosing CI failures

The [GitHub CLI](https://cli.github.com/) reads runs without leaving the
terminal. The orientation commands:

```bash
gh pr checks <pr>                              # pass/fail per workflow on a PR
gh run list --branch <branch>                  # recent runs on a branch
gh run view <run-id>                           # job/step summary
gh run watch <run-id> --exit-status            # follow an in-progress run live
gh run rerun <run-id> --failed                 # retry just the failed jobs
```

`gh run view <run-id> --log-failed` shows the failing step's console output —
but remember **`dev.py` is quiet by default**: a failed build prints only
`build failed - diagnose with: build_diag …`, *not* the compiler error. The
errors live in the uploaded **diagnostics artifact**, not the console. So the
real loop is download → extract → `build_diag`:

```bash
# 1. Grab the failing job's diagnostics (per workflow; matrix legs are suffixed
#    with the preset, e.g. linux-clang-debug-linux-clang-diagnostics).
gh run download <run-id> --name linux-gcc-diagnostics --dir build/.tmp

# 2. Extract ci-diag.zip at the repo root — its entries are build/<preset>/…,
#    so this recreates the sidecar tree exactly where build_diag expects it.
unzip -o build/.tmp/ci-diag.zip            # (or: python -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall('.')" build/.tmp/ci-diag.zip)

# 3. Read the real errors via the repo_tools MCP build_diag tool:
#    build_diag base_path="build/x64-linux-gcc-ninja-relwithdebinfo" show_tags=["error"]
```

`build_diag` groups the captured `.diag.json` sidecars into a per-translation-
unit error tree and surfaces unique first-errors, so a single call pinpoints the
problem — e.g. the Linux GCC job's archive immediately showed one `-fpermissive`
error in `hash-types-test.cc` (a `const` optional needing an initializer), and
the macOS archive showed a block-scope `extern "C"` in `assert.cc`. For **test**
failures (build green, tests red), the artifact also carries
`ci-test-results.xml` (the merged JUnit report) and `ci-logs.zip` (raw captured
stdout/stderr) as the fallback.

> Coming: once `build_diag` / `test_diag` can read a `.zip` directly, step 2 goes
> away — point the tool straight at the downloaded `ci-diag.zip` /
> `ci-test-results` artifact.

Remember to clean up afterward: `rm -rf build/.tmp build/<preset>` (the extracted
sidecar tree is gitignored under `build/`, but tidy it so a later local
`build_diag` doesn't read stale cloud sidecars).

## Extending

Natural next steps, each its own workflow or matrix entry: an **arm64 Linux**
job (`runs-on: ubuntu-26.04-arm`), the **sanitizer** presets (ASan/UBSan, Linux
clang), the remaining WASM tiers (threads, WebGPU, WASI), iOS/Android, and build
caching (ccache/sccache).

**Prefer Linux for additional checks.** Linux runners spin up faster and cost
less than Windows/macOS, and the `ubuntu-26.04` job needs no toolchain-install
steps — so it has the lowest end-to-end latency. New gates (extra presets, lint
passes, coverage) are cheapest to add there unless they specifically require
another platform — which is also why the deep config matrix lives on Linux
clang.
