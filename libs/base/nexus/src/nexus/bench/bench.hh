#pragma once

// Umbrella header for the nx::bench benchmarking helpers.
//
//   #include <nexus/bench/bench.hh>
//
//   // Keep measured work from being deleted (barriers.hh):
//   auto const h = nx::bench::keep(hash(key));   // wraps an expression, costs nothing
//   nx::bench::sink(result);                     // the statement form
//   nx::bench::evict_data_caches();              // run-time, costs milliseconds, for a cold measurement
//
//   // Measure a workload once (hardware_counters.hh; loop inside the body yourself if you want repetition):
//   auto const m = nx::bench::measure_hw_counters([&] {
//       for (auto i = 0; i < 1'000'000; ++i)
//           sink += work(i);
//   });
//   if (auto ins = m.value_of(nx::bench::hw_counter::instructions_retired))
//       cc::println("{:'} instructions", ins.value());
//
//   nx::bench::print_hw_counters(); // see what this machine can measure
//
// barriers.hh says why the read guards and the memory barrier are different things.
// hardware_counters.hh has the counter API contract, and docs/guides/profiling.md what each platform can measure.

#include <nexus/bench/barriers.hh>
#include <nexus/bench/hardware_counters.hh>
