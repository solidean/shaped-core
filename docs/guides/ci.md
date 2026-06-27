# Continuous integration

GitHub Actions gates the Tier-1 native toolchains on every push to `main` and
every pull request (and on manual `workflow_dispatch`). (Back to
[_index.md](_index.md).)

## Philosophy

CI leans on `dev.py` for everything it can — build, test, doctor — rather than
calling `cmake`/`ctest` directly or hand-rolling shell. Explicit flags are fine
(and preferred over implicit defaults): a workflow step like
`uv run dev.py test --preset relwithdebinfo-linux-clang --merged-xml-report …`
is something a developer can paste verbatim and reproduce locally. The less CI
does that *isn't* just a `dev.py` invocation, the smaller the gap between "green
on CI" and "green on my machine". So the workflows stay thin: provision the
toolchain, then drive `dev.py`.

## Setup

One workflow per platform, so each gets its own status badge in the
[README](../../README.md):

| Workflow                                                         | Runner           | Preset                       |
|------------------------------------------------------------------|------------------|------------------------------|
| [ci-windows-clang.yml](../../.github/workflows/ci-windows-clang.yml) | `windows-latest` | `relwithdebinfo-clang`       |
| [ci-linux-clang.yml](../../.github/workflows/ci-linux-clang.yml)     | `ubuntu-26.04`   | `relwithdebinfo-linux-clang` |

Both jobs use the **runner's preinstalled toolchain** — clang, CMake, and Ninja
all ship on the GitHub images — so neither installs any compiler or build tool.
The only provisioning step is installing `uv` (the `dev.py` runner). This keeps
the workflows fast and free of third-party install actions. They follow the same
shape and do all real work through `dev.py`:

1. Toolchain (from the runner image, no install):
   - **Linux** (`ubuntu-26.04`) ships clang 20/21/22 side by side. The
     `relwithdebinfo-linux-clang` preset calls bare `clang`/`clang++`, so the
     only setup is symlinking those names to the **21.x** binaries (our
     [requirements.md](../requirements.md) LLVM 21 target).
   - **Windows** (`windows-latest`) ships **LLVM 20** plus CMake and Ninja, with
     `clang-cl` already on `PATH`. No MSVC-setup step is needed: `dev.py` locates
     the MSVC environment via `vswhere` and injects it for `clang-cl`. This is a
     touch behind the LLVM 21 target — acceptable because the clang-format/tidy
     gate runs on Linux (clang 21), so Windows only needs to *compile* clean. If
     a future C++ feature needs clang 21 on Windows, add a pinned install step
     then.
2. Install `uv`.
3. `uv run dev.py doctor` — **informational, non-gating** (`continue-on-error`).
   Doctor also probes clangd's compile database and `llvm-cov`/`llvm-profdata`,
   which aren't needed for a build+test gate and would otherwise fail a fresh
   runner. It runs first purely so the toolchain state is visible in the log.
4. `uv run dev.py build --preset <preset>` — the gating build.
5. `uv run dev.py test --preset <preset> --merged-xml-report build/ci-test-results.xml`
   — the gating test run; the merged JUnit XML is uploaded as a build artifact.

Every build/test invocation passes `--preset` **explicitly** — CI never relies
on the platform-default preset.

## Diagnosing CI failures with `gh`

The [GitHub CLI](https://cli.github.com/) reads runs without leaving the
terminal. Scope by workflow file or branch:

```bash
gh run list --workflow ci-linux-clang.yml      # recent runs of one workflow
gh run list --branch u/pt/github-ci            # all runs on a branch

gh run view <run-id>                           # job/step summary
gh run view <run-id> --log-failed              # only the failing step's log
gh run watch <run-id>                          # follow an in-progress run live

gh run download <run-id>                       # pull the JUnit XML artifact
gh run rerun <run-id> --failed                 # retry just the failed jobs
```

`gh run view --log-failed` is usually the fastest first look; download the
`*-test-results` artifact when you need the per-test JUnit detail rather than
console output.

## Extending

Kept deliberately small to start. Natural next steps, each its own workflow or
matrix entry once these two are green: MSVC, Linux GCC, macOS, an arm64 Linux
job (`runs-on: ubuntu-26.04-arm`), the sanitizer/debug/release presets, and
build caching.

**Prefer Linux for additional checks.** Linux runners spin up faster and cost
less than Windows/macOS, and (like Windows here) the `ubuntu-26.04` job needs no
toolchain-install steps — so it has the lowest end-to-end latency. New gates
(extra presets, lint passes, coverage) are cheapest to add there unless they
specifically require another platform.
