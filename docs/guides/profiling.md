# Profiling

Measuring what code actually cost when it ran — not what it might do.
Back to [guides](_index.md).

This is the dynamic counterpart to [disassembly.md](disassembly.md): that guide reads the *static* codegen (what the optimizer emitted), while this one measures the *runtime* cost (what the CPU actually spent).
More profiling tooling will land here over time; today it covers hardware performance counters.

## Hardware performance counters

The CPU keeps counters for events the instruction stream hides: instructions retired, branch mispredictions, cache misses.
A `mov` that misses to DRAM and one that hits L1 read identically in the disassembly but differ by hundreds of cycles — only a counter shows that.
`nx::bench` reads these around a single call, miniperf-style.

### The API

`#include <nexus/bench/bench.hh>`, namespace `nx::bench`.

```cpp
// Measure the default counter set across ONE invocation of the body (it does not loop for you).
auto const m = nx::bench::measure_hw_counters([&] { work(); });

// Pull a counter out; nullopt if it was not requested or could not be read this run.
if (auto const ins = m.value_of(nx::bench::hw_counter::instructions_retired); ins.has_value())
    cc::println("retired {} instructions", ins.value());

// Or pass a config: override the counter set (order matters — see the PMC budget below), and/or ask for
// every counter to be measured even when they exceed the budget (measure_all re-runs the body over subsets).
auto const m2 = nx::bench::measure_hw_counters([&] { work(); }, {
    .counters = cc::vector<nx::bench::hw_counter>{
        nx::bench::hw_counter::instructions_retired,
        nx::bench::hw_counter::cache_llc_misses,
    },
    .measure_all = true, // body must be repeatable/deterministic; it runs once per pass
});
```

The counters (`hw_counter`): `elapsed_nanoseconds`, `reference_cycles`, `instructions_retired`, `branch_instructions`, `branch_misses`, `cache_l1d_misses`, `cache_llc_references`, `cache_llc_misses`.

The call is **best-effort and never fails as a whole**.
It always yields the baseline — elapsed time, and on x86 a reference-cycle count — with no privileges anywhere, including virtualized CI.
A PMU counter the machine cannot read this run comes back with `value_of() == nullopt` rather than erroring, so gate hard assertions on `has_value()`, not on a machine assumption.

### Discovering what a machine can measure

```bash
uv run dev.py profiling counters
```

Prints every counter with `[x]` (measurable right now) or `[ ]`, and — when the CPU has a PMU that this process cannot read — the one-time setup still needed.
`nx::bench::available_hw_counters()` is the same information in code; `nx::bench::print_hw_counters()` is the printer.
`available` means *actually readable now*, not merely "the CPU exposes it": on Windows it is probed by really starting the trace session.

### Platform support

- **Linux** — `perf_event_open(2)`, user space only.
  Gated by `/proc/sys/kernel/perf_event_paranoid`; commonly blocked inside containers/sandboxes.
- **Windows** — a real-time ETW context-switch session (details below).
  Needs a one-time setup to read counters without elevation.
- **macOS / ARM64** — baseline only for now (no PMU backend).

`NX_BENCH_HAS_HW_COUNTERS` (1/0) is a coarse compile-time hint that a real backend was compiled in; the runtime source of truth is always `available_hw_counters()`.

### Windows setup (non-elevated)

Reading PMU counters on Windows goes through an ETW SystemTraceProvider session, which a non-admin can start only after a one-time grant.
Run once, elevated:

```powershell
powershell.exe -ExecutionPolicy Bypass -File tools\setup-pmu-access.ps1
```

It grants three things together: Performance Log Users membership, `SeSystemProfilePrivilege`, and ETW DACLs on the session and System-Trace-Provider GUIDs.
Sign out and back in afterward (the group and privilege enter the token at logon).
`uv run dev.py doctor` reports whether this is in place.
Without it, `nx::bench` degrades to the baseline and warns once.

### Caveats

- **Limited PMC budget.**
  Only a few hardware counters can be programmed at once (a handful per core, minus whatever other ETW sessions currently hold).
  A single pass **degrades to as many as fit**, keeping the earliest-requested counters — so put the ones you care about first; a counter that lost the budget race comes back `nullopt`.
  Set `hw_measure_config::measure_all` to lift this: the body is re-run over budget-sized subsets until every requested counter is measured (it must be deterministic, and runs once per pass).
- **Windows granularity.**
  Counters are read at context switches, so the measured region is bracketed by forced switches and the count is quantum-granular with a small fixed overhead.
  Good for "what did this whole benchmark cost" and A/B comparisons; not for counting a 200-instruction region.
- **`reference_cycles` is not core cycles.**
  It is an rdtsc/thread-cycle baseline that tracks wall-time, so it does not reflect frequency scaling — but it is always available and enough to show a cost gap.

### Worked example: cache-friendly vs cache-hostile traversal

`libs/base/nexus/tests/bench-cache-traversal-test.cc` walks a 256 MiB `8192 x 8192` array row-major (contiguous inner stride) and column-major (striding a full row per step).
Same work, same instruction stream (vectorization disabled so both retire the same scalar loads) — only the memory access order differs.
A representative run:

| traversal | time | ref_cycles | instructions | llc_misses |
|-----------|------|-----------|--------------|------------|
| row-major | 18 ms | 67M | 537M | 4.2M |
| col-major | 301 ms | 1112M | 539M | 69M |

Near-identical instructions, but the column-major walk pays ~16× the cycles and ~16× the cache misses — the cost the instruction stream hides, made visible.
Run it yourself (it is `nx::config::manual`, so name it exactly):

```bash
uv run dev.py --mirror-test-output test "nexus bench - 2d traversal cache effect"
```

## See also

- [disassembly.md](disassembly.md) — the static side: read the emitted codegen, and with `dev.py assembly trace` see which path an invocation actually ran plus the data it touched (a cache footprint the counters here quantify but do not localize).
- [perf-results.md](perf-results.md) — recording benchmark *metrics* over time (`GUIDE_BENCHMARK` + `nx::guide`), distinct from the ad-hoc per-region counters here.
