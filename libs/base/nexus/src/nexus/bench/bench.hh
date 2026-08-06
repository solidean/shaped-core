#pragma once

// Umbrella header for the nx::bench benchmarking helpers.
//
//   #include <nexus/bench/bench.hh>
//
//   // Measure a workload once (loop inside the body yourself if you want repetition):
//   auto const m = nx::bench::measure_hw_counters([&] {
//       for (auto i = 0; i < 1'000'000; ++i)
//           sink += work(i);
//   });
//   if (auto ins = m.value_of(nx::bench::hw_counter::instructions_retired))
//       cc::println("{:'} instructions", ins.value());
//
//   nx::bench::print_hw_counters(); // see what this machine can measure
//
// See hardware_counters.hh for the API contract, and docs/guides/profiling.md for what each platform can measure.

#include <nexus/bench/hardware_counters.hh>
