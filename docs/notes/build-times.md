# Build times — where a `dev.py check` actually goes

Breadcrumbs from the August 2026 measurement pass, so the next person to revisit this starts from data rather than from intuition.
Numbers are from one machine (24 cores, Windows 11, clang-cl 22.1.7, Ninja) and are meant as ratios, not absolutes.

## How to measure

```bash
uv run dev.py check --fix --profile .tmp/dev-profile/check.json --profile-type chrome-tracing
uv run dev.py compile-time headers "libs/base/clean-core/src/clean-core/container/*.hh"
uv run dev.py compile-time tu      "libs/**/tests/**/*.cc"
```

`--profile` answers which *phase* is slow; `compile-time` answers which headers and TUs, once the answer is "compilation".
[guides/compile-times.md](../guides/compile-times.md) is that tool's workflow and the traps it exists to avoid.

The profile loads directly into <https://ui.perfetto.dev>, or into the `repo_tools` `analyze_trace` tool.
The step banners only time whole steps; per-TU compiles, the MSVC env capture and the per-binary test probes are invisible without `--profile`.
The summary block dev.py prints at the end is usually enough on its own: it gives sum, span and parallelism per job type.

**`sum` versus `span` is the whole game.**
A job type whose sum equals its span ran strictly serially, and its parallelism column reads `1.0x`.
That is where orchestration wins live.
Where parallelism is already at core count, only doing less work helps.

## The three scenarios

They behave differently enough that "the build is slow" is not one question.

| scenario | what changed | frequency |
|---|---|---|
| **cold** | empty `build/`, extern rebuilt too | rare — a fresh clone, or a deliberate wipe |
| **semi-cold** | every source under `libs/` and `tools/` is newer than its object; `extern/` untouched | **the day-to-day one** — at least once per feature branch, after merging `main` |
| warm | a handful of files | the inner loop |

Semi-cold is the one to optimize.
It is what a `git pull` on a branch produces: our own libraries move constantly, extern does not.
Reproduce it by touching first-party sources only:

```python
# from the repo root
import os, time
now = time.time()
for root in ("libs", "tools"):
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d != "extern"]
        for f in fns:
            if os.path.splitext(f)[1] in {".cc", ".hh", ".c", ".h", ".cpp"}:
                os.utime(os.path.join(dp, f), (now, now))
```

## What the August 2026 baseline looked like

`check` builds and tests four presets: relwithdebinfo, debug, release, singlethreaded.

**Cold — 400 s**

| phase | wall | CPU | parallelism |
|---|---|---|---|
| compile | 243.8 s | 4528.9 s | 18.6x |
| configure | 84.7 s | 84.7 s | 1.0x |
| test | 41.8 s | 41.8 s | 1.0x |
| env capture | 23.5 s | 23.5 s | 1.0x |
| link | 6.9 s | 14.7 s | 2.1x |

**Semi-cold — 230 s**

| phase | wall | CPU | parallelism |
|---|---|---|---|
| compile | 164.9 s | 3938.2 s | 23.9x |
| test | 46.9 s | 46.9 s | 1.0x |
| env capture | 13.3 s | 13.3 s | 1.0x |
| link | 4.8 s | 15.3 s | 3.2x |
| configure | 0 | — | the fingerprint is current, so it does not run at all |

**After the fixes below: cold 331 s, semi-cold 198 s.**
Attribute those carefully rather than taking the deltas at face value — see [Runs vary by ~8 %](#runs-vary-by-8) at the end.
Cold's −70 s is almost all real, because configure and env are mechanically verified spans: configure 84.7 s → 44.7 s, env 23.5 s → 2.7 s.
Semi-cold's −32 s is not: only ~11 s of it is attributable, and the rest was a faster machine that day.

Two things follow immediately, and they point in opposite directions.

**Configure only costs when cold.**
It was 21 % of a cold run and 0 % of a semi-cold one.
21 s per preset, and essentially all of it is SDL3's ~150 `try_compile` feature probes, re-run per preset from scratch.

**Compile is already saturated in semi-cold.**
23.9x on 24 cores means no scheduling trick will help.
The only lever left there is compiling less, or compiling the same thing more cheaply.

## Fixed

- **`<immintrin.h>` is gone from clean-core.**
  `spin.hh` included it to reach `_mm_pause`, and `spin.hh` sits under `mutex.hh`, so most of the repo paid for the AVX-512 headers.
  `clean-core/platform/intrinsics.hh` now *declares* the handful of intrinsics we use instead of including anything — the counterpart to `win32_sanitized.hh`, which includes behind guards.
  `spin.hh` went from **45 files entered to 1**, and `wide_arith.hh` to 12.
  Note this only removes the *direct* path: a TU that includes `<mutex>` or `<memory>` still gets them through `<xutility>`, which is the finding below.
- **`<chrono>` is behind `cc::current_time_steady_secs` / `cc::current_cycles`** in `clean-core/common/time.hh`.
  It was the most expensive header in the standard library at 1.16 s and 148 files, and `nexus/bench/impl/baseline.hh` had it in a *header*.
  Only `time.cc` may include it now, enforced by clean-core's `.shaped-lint.yml`.

- **The MSVC environment was captured 9 times per cold run**, ~2.6 s each — twice per preset, because configure and build each asked independently.
  It is now cached process-wide on `(toolset, arch)`, so every preset sharing a toolchain shares one capture: 23.5 s → 2.7 s.
  The cache holds its lock across the capture, not just around the dict.
  The concurrent-configure path below asks from four threads at once, and a dict-only lock lets all four miss and shell out anyway.
- **The four configures ran serially, each behind the previous preset's build.**
  They are independent — separate build dirs, separate `CMakeFiles/CMakeTmp` — so `ensure_configured_all` now fans them out.
  Prerequisite fetches stay serial (they share `extern/`), and `build/compile_commands.json` is published once from the primary preset, since order is no longer defined.
  84.7 s → 44.7 s, and the floor is not 23 s because the `lint` gate configures the default preset alone before anything else — clang-tidy needs its `compile_commands.json`.
  So a cold run pays one serial 21 s configure, then 23 s for the other three together.
- **`node-allocation-design-benchmark.cc` is compiled out** behind `CC_BENCH_NODE_ALLOCATION_DESIGN`, default 0.
  At 8.9 s it was the worst first-party TU in the repo by 2.2x, and it is a settled design study rather than a regression gate.
  That TU now costs 0.3 s, so it is −35 s of compile CPU per run — but only ~1.5 s of wall clock, because 1948 TUs at 24x parallelism absorb one long pole easily.
  See the tradeoff section below.

## Not done, and why

**Unity builds — measured, deferred.**
The suspicion was that merging two 4 s TUs might cost 8 s rather than 5 s.
It does not, and by a wide margin.
Merging real files with their real compile commands, run solo:

| group | separate | merged | speedup |
|---|---|---|---|
| 4 × dx12 `*-test.cc` | 6.87 s | 2.45 s | 2.80x |
| 3 × viewer `*-test.cc` | 5.66 s | 2.12 s | 2.67x |
| 4 × dxc `end-to-end-*.cc` | 7.36 s | 2.55 s | 2.89x |
| 4 × clean-core container `*-test.cc` | 3.12 s | **failed to compile** | — |

The marginal cost of each additional file in a merged TU is ~0.27 s against ~1.8 s standalone, so roughly **85 % of a test TU is header parsing**.
`dev.py compile-time tu` confirms that independently and end to end: a dx12 test measures **84.6 % includes**, against **29 %** for a clean-core container test.
So the two families want opposite fixes, and unity builds pay far more in the graphics tree than in clean-core.
Verify any repeat of this by checking object sizes.
The first attempt string-replaced paths that differ in separator between `compile_commands.json`'s `output` field and the command line.
That silently no-opped, and "measured" a merged TU that was really just the first file recompiled.

The container group is the honest counterweight: `imkey` is defined in both `map-test.cc` and `set-test.cc`, and merging them is a redefinition error.
One collision in four sampled groups is the migration cost, and it is mechanical.

Deferred rather than rejected, for reasons that are not about the speedup:

- It must be **preset-scoped** (`CMAKE_UNITY_BUILD` on a check-only preset), never a target property.
  `compile_commands.json` would otherwise describe unity TUs, which breaks the clang-tidy gates, `dev.py assembly`, `build_diag`'s per-TU grouping and coverage mapping.
- It trades parallelism for redundancy, and semi-cold compile is already at 23.9x.
  Batch size has to keep the job count comfortably above core count.
- Touch one file, rebuild a batch — bad for the inner loop, which is another argument for scoping it to the gate.

**A prebuilt / shared extern tree — open question.**
Extern is 773 s of the cold run's 4529 s compile CPU, dominated by `implot_items.cpp` (192 s across four presets, up to 62.6 s in one) and `sqlite3.c` (100 s).
`singlethreaded` is `RelWithDebInfo` + `SC_THREADS=OFF`, and no extern target sees `SC_THREADS`, so those objects are rebuilt identically for no reason.
But sharing them properly is a superbuild — a separate configure, an install prefix, imported targets — and **it buys nothing in semi-cold**, where extern is already untouched.
Worth doing for cold, not worth confusing with the day-to-day cost.

**System headers, not ours, dominate the parse.**
On a dx12 test TU the exclusive self time splits 0.32 s ours against 0.98 s system — MSVC's `<ranges>`, `<variant>` and `<xutility>`, plus `d3d12.h`, `winnt.h` and `immintrin.h`.
That caps what include hygiene on our own headers can win at roughly a quarter of the parse cost, and it is the strongest argument for unity builds, which amortize exactly the system-header share.

**`<xutility>` is the root of the system-header cost on MSVC, and it pulls `<immintrin.h>`.**
That is why `xutility`, `xstring` and the AVX-512 intrinsic headers dominate every attribution table — they are not included by anything of ours directly.
Anything that reaches `<xutility>` inherits ~43 intrinsic headers, and the split between the STL headers is sharp:

| header | cost | files | intrinsics |
|---|--:|--:|--:|
| `<type_traits>` | 0.079 s | 17 | 0 |
| `<utility>` | 0.106 s | 29 | 0 |
| `<atomic>` | 0.121 s | 38 | 1 |
| `<string_view>` | 0.413 s | 91 | 43 |
| `<memory>` | 0.485 s | 103 | 43 |
| `<mutex>` | 0.586 s | 116 | 44 |
| `<chrono>` | **1.160 s** | **148** | 45 |

So the actionable rule is not "include less" but **"keep `<memory>`, `<string>`, `<string_view>`, `<mutex>`, `<system_error>`, `<ranges>` and `<chrono>` out of widely-included headers"**.
`<type_traits>`, `<utility>` and `<atomic>` are free by comparison and need no such care.
The clean-core headers that still cross that line carry a COST NOTE pointing back here.

**A header's cost is its transitive closure, and two of clean-core's containers are outliers.**
`key_value_cache.hh` costs 0.56 s to include and pulls in 134 files; `pinned_data.hh` costs 0.47 s across 117.
Every other container header is 0.03–0.14 s across 12–46 files.

## Gotchas worth keeping

**A TU costs about 2.2x more under load than measured solo.**
The dx12 test TUs are ~1.8 s each when compiled alone and ~4 s each during a 24-way parallel build.
So a solo `clang-cl` timing is a lower bound, not the number that appears in a build, and speedup *ratios* transfer better than absolute times.

**Tests run strictly serially, by choice.**
46.9 s, 20 % of a semi-cold run, at 1.0x.
They are not parallelized because the binaries thread internally, and because benchmarks and GPU-touching tests share resources that will not tolerate concurrent runs.
Making nexus parallel is the follow-up that unlocks this, and it needs care around exactly those two cases.

**`shaped-graphics-test` dominates the test phase**, at 4.5–10.5 s per preset against a typical 20–900 ms.

### Runs vary by ~8 %

Two semi-cold runs of the same tree differed by 348 s of compile CPU, which looked like a spectacular win until it was broken down per target.
Every target had moved, by a median ratio of 0.920 and a range of 0.85–0.98, on identical TU counts.
That is the machine — thermals, background load — not the change.

So **attribute a delta per target before believing it**.
A real change shows up as one target moving while the others hold; a uniform shift across all of them is the machine.
The check that settles it:

```python
# per-target after/before ratio; a tight cluster away from 1.0 is machine variance
r = [after[t] / before[t] for t in before if before[t] > 30]
print(statistics.median(r), min(r), max(r))
```

Wall-clock spans of *serial* phases — configure, env, test — are far more trustworthy than compile CPU, because they are not competing for cores.
That is why the cold configure and env numbers above are quoted as results and the compile ones are not.

## On compiling experiments out

`node-allocation-design-benchmark.cc` is the pattern.
An experiment that settled a design question keeps its full source in the tree, behind a local `#if FLAG` that is 0, with the flag defined in the file itself.

The accepted trade is that guarded code rots.
That is fine and is close to the point: if the surrounding code has drifted far enough to break the experiment, the conclusions it reached are due a rerun anyway.

Two things must come with the guard, or flipping it back on is a bad afternoon:

- Any doc describing the experiment says it is compiled out and names the flag.
- Any script that drives it fails with a message naming the flag, rather than on an empty parse.

Candidates are experiments only.
`GUIDE_BENCHMARK` benchmarks are tracked perf gates feeding [perf-results](../guides/perf-results.md) and the PGO report — those stay compiled in.
Of clean-core's benchmark TUs, only the design sweep lacked a `GUIDE_BENCHMARK`, which is a decent smell test.

## What is left on the table

In rough order of value:

- **Parallel tests.** 40 s, 20 % of a semi-cold run, at 1.0x — the largest remaining orchestration lever, and blocked on making nexus parallel.
- **Unity builds**, as measured above: ~85 % of a test TU is header parsing, and semi-cold compile is 76 % of the run.
- **Overlapping the three parallel configures with the first preset's build.** Worth ~23 s, cold only.
  They currently block at the start of the test gate while the machine sits idle, and the default preset is already configured by then, so its build could start immediately.
- **A shared extern tree**, worth ~580 s of cold compile CPU and nothing semi-cold.

## If you revisit this

Take a fresh cold and semi-cold profile before changing anything — the ratios above will have moved.
Re-check whether compile parallelism is still near core count in semi-cold, because that single number decides whether the next win is orchestration or less work.
