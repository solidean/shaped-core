# Continuous integration

GitHub Actions gates the Tier-1 platforms on every push to `main` and every pull request, plus manual `workflow_dispatch`.
This doc is about what CI actually does; for the support model — what *should* work, and the Tier-2/3 platforms — see [platforms.md](../platforms.md).
Back to [_index.md](_index.md).

## Philosophy

Two ideas drive the layout:

- **Lean on `dev.py`.** CI drives `dev.py` for everything it can — build, test, doctor — rather than calling `cmake` / `ctest` directly or hand-rolling shell.
  Explicit flags are fine, and preferred over implicit defaults.
  A step like `uv run dev.py test --preset relwithdebinfo-linux-clang --merged-xml-report …` is one a developer pastes verbatim and reproduces locally.
  The less CI does that *isn't* just a `dev.py` invocation, the smaller the gap between "green on CI" and "green on my machine".
  So workflows stay thin: provision the toolchain, then drive `dev.py`.

- **Wide but shallow, with one deep punch.** Most workflows cover a *platform* at a single config, RelWithDebInfo — broad surface area, cheap.
  One workflow, **Linux Clang**, runs the deep matrix (debug / relwithdebinfo / release) to exercise the define interactions a single config cannot.
  Chiefly that is `CC_ASSERT` on versus off, plus optimization-level differences.
  Linux clang carries the deep matrix because it is the fastest, lowest-setup runner.

## Setup

One workflow per platform/compiler, so each gets its own status badge in the
[README](../../README.md):

| Workflow                                                              | Runner           | Preset(s)                                                  |
|-----------------------------------------------------------------------|------------------|------------------------------------------------------------|
| [ci-linux-clang.yml](../../.github/workflows/ci-linux-clang.yml)      | `ubuntu-26.04`   | `debug-linux-clang`, `relwithdebinfo-linux-clang`, `release-linux-clang`, `nopch-linux-clang` (deep matrix), `--toolset 21` |
| [ci-linux-gcc.yml](../../.github/workflows/ci-linux-gcc.yml)          | `ubuntu-26.04`   | `relwithdebinfo-linux-gcc`, `--toolset 14`                  |
| [ci-windows-clang.yml](../../.github/workflows/ci-windows-clang.yml)  | `windows-latest` | `relwithdebinfo-clang`, `--toolset 20` (assert clang-cl 20) |
| [ci-windows-msvc.yml](../../.github/workflows/ci-windows-msvc.yml)    | `windows-2025`   | `relwithdebinfo-msvc`, `--toolset 14.44` (VS 2022)         |
| [ci-windows-msvc-vs2026.yml](../../.github/workflows/ci-windows-msvc-vs2026.yml) | `windows-2025-vs2026` | `relwithdebinfo-msvc`, `--toolset 14.51` (VS 2026) |
| [ci-windows-arm-msvc.yml](../../.github/workflows/ci-windows-arm-msvc.yml) | `windows-11-arm` | `relwithdebinfo-arm64-windows-msvc`, `--toolset 14.44` (VS 2022, native arm64) |
| [ci-linux-arm-clang.yml](../../.github/workflows/ci-linux-arm-clang.yml) | `ubuntu-26.04-arm` | `relwithdebinfo-arm64-linux-clang`, `--toolset 21` (native arm64) |
| [ci-macos-clang.yml](../../.github/workflows/ci-macos-clang.yml)      | `macos-latest`   | `macos-arm-llvm-relwithdebinfo`, `--toolset 22` (assert clang 22) |
| [ci-wasm-emscripten.yml](../../.github/workflows/ci-wasm-emscripten.yml) | `ubuntu-24.04`   | `emscripten-relwithdebinfo`                                 |
| [ci-ios-clang.yml](../../.github/workflows/ci-ios-clang.yml) | `macos-latest` | `ios-arm64-relwithdebinfo` (**build-only**) |
| [ci-android-ndk.yml](../../.github/workflows/ci-android-ndk.yml) | `ubuntu-26.04` | `android-ndk-arm64-relwithdebinfo` (**build-only**) |

`nopch-linux-clang` is the odd cell: RelWithDebInfo with precompiled headers off.
A PCH reaches every TU through `/FI`, so a source that dropped an include it still uses compiles everywhere else and fails only here — see [precompiled-headers.md](precompiled-headers.md).

Every workflow shares the same shape: provision the toolchain, then `doctor` → `build` → `test` through `dev.py`, always with an **explicit `--preset`**, since CI never relies on the platform default.
Each also uploads a **diagnostics artifact**, described below.
The two **build-only** jobs, iOS and Android, cross-compile targets the runner cannot execute, so they stop after `build`.
They have no `test` step, and their artifact carries only `ci-diag.zip` and `ci-logs.zip` — no merged test report.
[platforms.md](../platforms.md) has the build-only Tier-2 status.

### Diagnostics artifacts

`dev.py` is quiet by default, capturing compiler and test output to files under `build/<preset>/` rather than the console.
So a red CI job shows only a terse "build failed" line, not the actual error.
Each job therefore uploads a `<platform>-diagnostics` artifact — always, even on failure — with three things:

- **`ci-diag.zip`** — every `.diag.json` sidecar, one per compile or link, written by the diag launcher wired in as the CMake compiler and linker launcher.
  That is `diag-launcher.exe` on Windows, and `diag_launcher.sh` → `diag_launcher.py` on Linux and macOS.
  This is the build-step analogue of the JUnit report, produced by `dev.py build --diag-archive` even when the build fails.
  CI builds also pass `--keep-going` so one failing run surfaces *every* independent error rather than just the first; without it, fixing portability becomes a slow one-at-a-time loop.
- **`ci-test-results.xml`** — the merged JUnit test report, from `dev.py test --merged-xml-report`.
- **`ci-logs.zip`** — the raw captured run logs and step sidecars, the last resort when the structured sidecars don't explain it.
  Produced by the global `dev.py --collect-logs`, which fires on exit regardless of pass or fail.
  It is set on *both* the build and test steps so either failure mode is covered.
  The logs under `build/<preset>/run-logs/` accumulate across configure → build → test, so the test step's archive is a strict superset of the build step's.
  It also carries the `*.ccrec` recordings nexus writes for a failing `nx::config::recorded` test — see [nexus/docs/recording.md](../../libs/base/nexus/docs/recording.md).
  That is how a failure reproducible only on a remote runner gets diagnosed rather than guessed at: the test's own event stream comes back with the run.
  Load one with `cc::rec::load_recording`; the events a test logged also print through the console listener, so `run-logs/` alone often answers the question.
  And when the build fails the test step is skipped, leaving the build step's archive — the one with the failure logs — as the uploaded artifact.

To use them locally, download the artifact into `build/.tmp/<name>/` and point `build_diag` / `test_diag` **straight at the archive**.
Both tools read inside a `.zip`, decoded in memory, with no extraction step:

```bash
gh run download <run-id> --name linux-gcc-diagnostics --dir build/.tmp/linux-gcc
# then, via the repo_tools MCP:
#   build_diag base_path="build/.tmp/linux-gcc/ci-diag.zip" show_tags=["error"]
#   test_diag  base_path="build/.tmp/linux-gcc/ci-logs.zip" errors_only=true
```

### System packages

Only the Linux jobs install anything beyond `uv`, and only for SDL3.

SDL refuses to configure on a unix that has neither X11 nor wayland **development headers**, so the three
Linux workflows install them with `apt-get`.
Headers only — sr's window and input tests are headless, running on SDL's dummy video driver, so no display
and no runtime windowing library is involved.

The list is not just `libx11-dev`: with X11 enabled, SDL also requires every X extension it uses, and fails configure naming the first one missing.
Those are XCURSOR, XDBE, XFIXES, XINPUT, XRANDR, XSCRNSAVER, XSHAPE, XSYNC and XTEST.
In Ubuntu terms that is

    libx11-dev libxext-dev libxcursor-dev libxfixes-dev libxi-dev libxrandr-dev
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols

If SDL ever demands another, the error names the package group — the authoritative list is
`SDL_missing_dependency` in the fetched tree's `cmake/sdlchecks.cmake`.

Without them nothing breaks: [extern/sdl3](../../extern/sdl3/CMakeLists.txt) checks first and bows out, and
`SC_HAS_SDL3` reports OFF so the rest of sr still builds.
That is the point of installing them — otherwise the window and input tests would silently vanish from the
Linux runs rather than fail, which is the worst of both.

The same applies to a developer machine: a Linux checkout without those packages configures and builds fine,
it just has no `sr::window`.

### Toolchains

Most jobs use the **runner's preinstalled toolchain**, since clang/gcc, CMake and Ninja all ship on the GitHub images.
So the only provisioning is installing `uv`, plus SDL3's windowing headers on Linux (see [System packages](#system-packages)).
Where a runner carries several versions of a compiler, the job **pins the one we expect with `dev.py`'s `--toolset`** rather than shimming `PATH` with symlinks.
A missing toolset is then a hard, loud error instead of a silent fall-through to whatever the default is.
`--toolset` also redirects the build directory to `build/<preset>-<toolset>`, so a pinned job never shares a CMake cache with a default-toolset build.
Per-platform specifics:

- **Linux** (`ubuntu-26.04`) ships clang 20/21/22 and gcc 13/14/15 side by side.
  The jobs pass `--toolset 21` (clang, the LLVM 21 target in [requirements.md](../requirements.md)) and `--toolset 14` (gcc, at or above the GCC 13 floor).
  dev.py resolves `clang++-21` / `g++-14` on `PATH` and errors if they are absent, so no symlinking is involved.
- **Windows MSVC** runs **two** jobs, one per toolset, each pinning it with
  `--toolset` so dev.py selects it via `vswhere` + `vcvars`: VS 2022's **14.44**
  on `windows-2025` ([ci-windows-msvc.yml](../../.github/workflows/ci-windows-msvc.yml)),
  and VS 2026's **14.51** on `windows-2025-vs2026`
  ([ci-windows-msvc-vs2026.yml](../../.github/workflows/ci-windows-msvc-vs2026.yml)).
  `--toolset` searches prerelease installs too, so a preview VS is reachable.
- **Windows clang** (`windows-latest`) uses the preinstalled **LLVM (clang 20)** `clang-cl`, with `dev.py` injecting the MSVC environment via `vswhere`.
  `clang-cl` is a single unversioned binary with no `clang++-N` to swap to, so `--toolset 20` here *asserts* it is clang 20 rather than selecting it — a hard error if the base image moves on.
  Riding clang 20, a touch behind the 21 target, is fine because the clang-format and clang-tidy gates run on Linux clang 21, so Windows clang only needs to *compile* clean.
- **macOS** (`macos-latest`, arm64) needs Homebrew LLVM: the `macos-arm-llvm-*` presets point `CMAKE_CXX_COMPILER` at `/opt/homebrew/opt/llvm/bin/clang++` and link Homebrew `libc++`.
  So the job runs `brew install llvm ninja` — CMake ships on the runner, and this is a distinct toolchain from the image's system Apple clang.
  Homebrew's `clang++` is unversioned, so `--toolset 22` *asserts* it is clang 22 rather than selecting it, which is loud if a Homebrew bump changes it.
- **WASM** uses the official `emscripten-core/setup-emsdk` action, pinned to a fixed Emscripten version (`EMSCRIPTEN_VERSION`) with the emsdk cached across runs.
  `dev.py` gets `--emsdk-path "$EMSDK"` on doctor, build and test; tests run under Node.
- **Windows ARM** (`windows-11-arm`, native arm64) uses VS 2022's MSVC pinned with `--toolset 14.44`, like the x64 job.
  The `arm64-windows-msvc-*` preset sets the CMake architecture and host toolset to arm64, and `dev.py` threads the preset's arch into the vcvars `-arch`, so `cl` targets arm64 rather than x64.
  The checked-in `diag-launcher.exe` is x64 and runs under the image's x64 emulation.
- **Linux ARM** (`ubuntu-26.04-arm`, native arm64) is the x64 Linux clang job's twin on arm: preinstalled clang, `--toolset 21`, no toolchain-install step.
- **iOS and Android** are **build-only** (Tier 2).
  iOS (`macos-latest`) cross-compiles with Apple Clang via the `ios-arm64-*` preset (`CMAKE_SYSTEM_NAME=iOS`, `iphoneos` sysroot), with Ninja from Homebrew.
  Android (`ubuntu-26.04`) cross-compiles via the NDK with the `android-ndk-arm64-*` preset, which reads the toolchain from `$ANDROID_NDK_ROOT`.
  The job points that at the image's **NDK r29 (Clang 21)** rather than the default r27, whose Clang 18 is too old for our C++23 — `std::atomic_ref`, for one.
  Both presets wire the POSIX `diag_launcher.sh`, since the iOS and Android hosts are macOS and Linux, so their `ci-diag.zip` carries real per-compile sidecars that `build_diag` reads.
  Neither runs tests: the runner cannot execute the produced binaries.

`doctor` runs first on every job but is **informational and non-gating** (`continue-on-error`).
It also probes clangd's compile database and `llvm-cov` / `llvm-profdata`, which a build-and-test gate does not need and which would otherwise fail a fresh runner.
It runs purely so the toolchain state is visible in the log.

## Diagnosing a red run

That is the `/debugging-ci` skill's job ([SKILL.md](../../.claude/skills/debugging-ci/SKILL.md)), and it drives the flow end to end.
Find the failing job, download its artifact, point the diag tools at the archive.
The one thing worth carrying over from this page is *why* there is a flow at all.
`gh run view <run-id> --log-failed` shows the failing step's console, and because `dev.py` is quiet that console holds only `build failed - diagnose with: build_diag …` — never the compiler error.
Everything real is in the artifact above.

## Extending

Natural next steps, each its own workflow or matrix entry: the **sanitizer**
presets (ASan/UBSan, Linux clang), the remaining WASM tiers (threads, WebGPU,
WASI), running the iOS/Android binaries on a simulator/emulator (today they are
build-only), and build caching (ccache/sccache).

**Prefer Linux for additional checks.**
Linux runners spin up faster and cost less than Windows and macOS, and the `ubuntu-26.04` job installs no toolchain beyond SDL3's headers, so it has the lowest end-to-end latency.
New gates — extra presets, lint passes, coverage — are cheapest to add there unless they specifically require another platform.
That is also why the deep config matrix lives on Linux clang.
