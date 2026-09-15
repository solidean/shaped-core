#pragma once

#include <clean-core/math/random.hh>
#include <nexus/fwd.hh>

namespace nx
{
/// This test's seed, derived from the run seed and the test's name, or the value `nx::config::seed(n)` pins.
///
/// A dispatched invocable derives its own from its driver's seed, its invocation name and its own name, so it gets
/// the same seed whether it runs in a full sweep or is addressed alone.
/// Printed as the run seed at the start of every run: `--seed N` reproduces every test's seed at once.
/// Zero outside a test.
[[nodiscard]] u64 test_seed();

/// A generator seeded from test_seed(), for a test that randomizes anything it wants reproduced.
[[nodiscard]] cc::random test_random();
} // namespace nx
