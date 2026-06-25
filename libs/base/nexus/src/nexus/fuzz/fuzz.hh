#pragma once

// Umbrella header for the nx::fuzz API-sequence fuzzer.
//
// Build the setup once in a TEST, then use SECTIONs: one to fuzz, one per pinned regression/behavior.
//
//   #include <nexus/fuzz/fuzz.hh>
//
//   TEST("add1 never reaches 7")
//   {
//       auto test = nx::fuzz::test::create();
//       test->add_value("3", 3);
//       test->add_op("add1", [](int a) { return a + 1; });
//       test->add_invariant("is-not-7", [](int i) { return i != 7; });
//
//       SECTION("fuzz") { CHECK(test->execute_fuzz_test()); }
//
//       SECTION("regression: reaches 7")
//       {
//           auto i0 = test->eval_op("3");
//           // ... paste the emitted reproducer here ...
//       }
//   }
//
// See libs/base/nexus/docs/fuzz-testing.md for the model, determinism, and regression workflow.

#include <nexus/fuzz/regression_dialect.hh>
#include <nexus/fuzz/replay_random.hh>
#include <nexus/fuzz/run.hh>
#include <nexus/fuzz/test.hh>
#include <nexus/fuzz/value.hh>
#include <nexus/test.hh>
