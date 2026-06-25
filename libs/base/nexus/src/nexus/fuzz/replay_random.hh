#pragma once

#include <clean-core/math/random.hh>

namespace nx::fuzz
{
/// Helper used by emitted regression code to reproduce operations that consume a cc::random&.
/// Each call hands the operation a fresh generator seeded with the recorded per-step seed, which
/// reproduces exactly the draws the failing run made.
///
///   nx::fuzz::replay_random random;
///   test->eval_op("gen", random.seeded(3737));
struct replay_random
{
    [[nodiscard]] cc::random seeded(cc::u64 seed) const { return cc::random(seed); }
};
} // namespace nx::fuzz
