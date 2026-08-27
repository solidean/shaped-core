# Profile-Guided Optimization (PGO)

`uv run dev.py pgo` builds a profile-guided-optimized variant of the libraries.
It builds an **instrumented** binary, runs a representative workload to collect a profile, then rebuilds using that profile so the optimizer lays out hot paths better.
The training workload and the before/after metrics both come from **nexus PGO benchmarks** — see [perf-results.md](perf-results.md) — so there is no separate, hand-maintained training manifest.
This is the full reference; the loop is four stages wrapped by one `run`.
Back to [guides](_index.md).

Clang-only, exactly like [coverage.md](coverage.md).
IR-based PGO (`-fprofile-generate` / `-fprofile-use` → `llvm-profdata`) works across `clang-cl`, `clang` and AppleClang; GCC's `.gcda` and MSVC's `.pgd` models are out of scope.

```bash
uv run dev.py pgo run              # instrument -> train -> optimize -> measure (the all-in-one)
uv run dev.py pgo run --no-measure # stop after the optimized build
uv run dev.py pgo instrument       # just the instrumented build
uv run dev.py pgo train            # run PGO benchmarks on it, merge the profile
uv run dev.py pgo optimize         # build the optimized preset from the profile
uv run dev.py pgo measure          # baseline (release) vs PGO speedup table
```

## The four phases

```
instrument:  build the *-pgo-generate preset      (SC_PGO_GENERATE -> -fprofile-generate)
train:       run PGO benchmarks on it           (LLVM_PROFILE_FILE -> *.profraw)
             llvm-profdata merge -sparse           ->  build/pgo/pgo.profdata
optimize:    build the *-pgo-use preset            (SC_PGO_USE consumes build/pgo/pgo.profdata)
measure:     run PGO benchmarks on release + pgo-use, diff the recorded metrics
```

The merged profile always lands at the stable, source-relative path **`build/pgo/pgo.profdata`**.
So the `*-pgo-use` configure preset reads it through the `SC_PGO_USE` CMake option without dev.py injecting any `-D` flags, and the configure path stays identical to every other preset.

## How it works

PGO instrumentation is a build-time concern, gated by two CMake options in the root [CMakeLists.txt](../../CMakeLists.txt) from module [tools/cmake/PGO.cmake](../../tools/cmake/PGO.cmake).
`SC_PGO_GENERATE` adds `-fprofile-generate`; `SC_PGO_USE` adds `-fprofile-use=build/pgo/pgo.profdata`.
Both are off for normal builds, and the dedicated **`*-pgo-generate` / `*-pgo-use` presets** turn them on.
They are **Release** rather than RelWithDebInfo: PGO targets the shipping configuration, and the measured run wants `CC_ASSERT` off.

| Platform | Generate preset                | Use preset                  | Baseline                   |
|----------|--------------------------------|-----------------------------|----------------------------|
| Windows  | `pgo-generate-clang`           | `pgo-use-clang`             | `release-clang`            |
| Linux    | `pgo-generate-linux-clang`     | `pgo-use-linux-clang`       | `release-linux-clang`      |
| macOS    | `pgo-generate-macos-arm-llvm`  | `pgo-use-macos-arm-llvm`    | `macos-arm-llvm-release`   |

`pgo run` with no `--preset` uses the platform defaults above.
Training and measurement run **every `*-test` binary** with `--pgo-benchmarks`.
A binary that contains no PGO benchmarks exits 0 and contributes nothing, so no per-library configuration is needed.
During training each binary writes its counters to a distinct `LLVM_PROFILE_FILE` under `build/<gen>/pgo/profraw/`, which `llvm-profdata merge -sparse` folds into the single profile.

The **baseline for the speedup delta is the clean `release-clang` build**, a fair "shipping vs shipping+PGO" comparison, rather than the instrumented build.

## Output

Like every dev.py step, each tool invocation is captured to
`build/<preset>/run-logs/`. Sidecars:

| File                                    | What it is                                                        |
|-----------------------------------------|-------------------------------------------------------------------|
| `build/<gen>/pgo.json`                  | train metadata: the merge step and how many `*.profraw` were folded |
| `build/<use>/pgo-measure.json`          | the baseline→PGO metric diff (per `(binary, test, name)`, % change) |
| `build/<preset>/pgo/.../*.pgo.json`    | the raw per-binary pgo-benchmark metrics (the perf-results contract) |

`measure` prints a speedup table: per metric, `baseline -> pgo unit`, and the oriented `±%`.
Positive means faster, respecting each metric's higher- or lower-is-better, and the green/red carries the direction.

```
PGO speedup [release-clang -> pgo-use-clang]:
  bench-hash (…) | xxh64@64KiB      27.92 ->      33.10 GB/s     +18.5%
  bench-alloc (…) | mimalloc@64B   182.07 ->     190.79 M ops/s  +4.8%
```

## Phases independently

Each phase works standalone and fails with a clear message when its prerequisite is missing.
`pgo optimize` before `pgo train` errors with "PGO profile not found at build/pgo/pgo.profdata — run: uv run dev.py pgo train", rather than a configure-time fatal.
`measure` incrementally builds both the baseline and the pgo-use preset first, so it is safe to run on its own once a profile exists.

## Tooling

`llvm-profdata` must be present and version-matched to the clang that built the binaries, resolved exactly as [coverage.md](coverage.md#tooling) describes.
`uv run dev.py doctor` reports it with its version.

## Related

- [perf-results.md](perf-results.md) — the `nx::pgo` API and `.pgo.json` schema that drive training and the speedup report.
- [building-and-testing.md](building-and-testing.md) — the build/test driver this builds on.
- [coverage.md](coverage.md) — the sibling LLVM pipeline this mirrors.
- [CMakePresets.json](../../CMakePresets.json) — the `*-pgo-*` presets; [tools/cmake/PGO.cmake](../../tools/cmake/PGO.cmake) — the `SC_PGO_*` options.
