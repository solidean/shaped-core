#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <limits>
#include <type_traits>

// =========================================================================================================
// Deterministic pseudo-random number generation
// =========================================================================================================
//
// cc::random is a small, fast, header-only PRNG based on PCG32 (O'Neill, Apache-2.0 reference).
// A given seed produces the same sequence on every platform and compiler, which makes it suitable
// for reproducible tests, fuzzing, and any algorithm that must replay exactly from a recorded seed.
//
// It is move-only with an explicit clone(): a generator is a position in a stream, and silently
// copying it (two callers drawing the "same" numbers) is almost always a bug. Duplicate the stream
// on purpose with clone() when you really want two independent-but-identical sequences.
//
//   cc::random rng(seed);                    - seeded generator
//   rng.next_u32() / next_u64()              - raw uniform bits
//   rng.uniform(a, b)                        - unbiased integer in [a, b], or float in [a, b)
//   rng.uniform_bool()                       - fair coin flip
//   rng.uniform_in(range) / shuffle(range)   - pick / permute over an indexable range
//   rng.clone()                              - explicit duplicate of the current stream position
//

namespace cc
{
/// Deterministic PCG32 generator. Reproducible across platforms for a fixed seed.
/// Move-only by design; use clone() to intentionally duplicate the stream position.
struct random
{
    /// Default-constructed generators share a fixed seed, so they are reproducible but identical.
    /// Prefer the seeded constructor whenever distinct streams matter.
    random() { seed(_default_seed); }

    explicit random(u64 s) { seed(s); }

    random(random const&) = delete;
    random& operator=(random const&) = delete;
    random(random&&) = default;
    random& operator=(random&&) = default;

    /// Returns an independent generator at the same stream position. Both produce identical sequences.
    [[nodiscard]] random clone() const
    {
        random r;
        r._state = _state;
        r._inc = _inc;
        return r;
    }

    /// Resets to a fresh stream fully determined by the seed (fixed stream selector).
    void seed(u64 s)
    {
        _state = 0;
        _inc = _default_stream;
        (void)next_u32();
        _state += s;
        (void)next_u32();
    }

    /// Reconstructs a generator directly from a raw state, bypassing the seeding scramble.
    /// The blessed roundtrip: from_state(r.state()) reproduces r's subsequent draws exactly.
    /// Any u64 is a valid state (the fixed odd increment makes the LCG full-period).
    [[nodiscard]] static random from_state(u64 state)
    {
        random r;
        r._state = state;
        r._inc = _default_stream;
        return r;
    }

    /// Raw LCG state. The replay partner of from_state(); also useful for inspection/debugging.
    [[nodiscard]] u64 state() const { return _state; }

    /// One PCG32 step: 32 uniform bits.
    [[nodiscard]] u32 next_u32()
    {
        u64 const old = _state;
        _state = old * 6364136223846793005ull + _inc;
        u32 const xorshifted = u32(((old >> 18u) ^ old) >> 27u);
        u32 const rot = u32(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    /// 64 uniform bits from two steps. The draw order is fixed and part of the reproducibility contract.
    [[nodiscard]] u64 next_u64()
    {
        u64 const hi = next_u32();
        u64 const lo = next_u32();
        return (hi << 32) | lo;
    }

    /// Unbiased uniform integer in [a, b] inclusive. Rejection-samples to avoid modulo bias.
    template <class T>
    [[nodiscard]] T uniform(T a, T b_inclusive)
    {
        static_assert(std::is_integral_v<T>, "integral overload expects an integral type");
        CC_ASSERT(a <= b_inclusive, "lower bound must not exceed upper bound");

        u64 const n = u64(b_inclusive) - u64(a) + 1;
        if (n == 0) // full 64-bit range requested: every draw is already valid
            return T(u64(a) + next_u64());

        // reject the top partial bucket so the remaining range divides evenly
        u64 const limit = std::numeric_limits<u64>::max() - (std::numeric_limits<u64>::max() % n);
        u64 x;
        do
        {
            x = next_u64();
        } while (x >= limit);
        return T(u64(a) + (x % n));
    }

    /// Fair coin flip.
    [[nodiscard]] bool uniform_bool() { return (next_u32() & 1u) != 0; }

    /// Uniform float in [a, b). Uses 24 mantissa bits.
    [[nodiscard]] f32 uniform(f32 a, f32 b)
    {
        f32 const unit = f32(next_u32() >> 8) * (1.0f / 16777216.0f); // [0, 1)
        return a + (b - a) * unit;
    }

    /// Uniform double in [a, b). Uses 53 mantissa bits.
    [[nodiscard]] f64 uniform(f64 a, f64 b)
    {
        f64 const unit = f64(next_u64() >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
        return a + (b - a) * unit;
    }

    /// Uniformly picks one element of an indexable, non-empty range; returns a reference into it.
    template <class Range>
    [[nodiscard]] decltype(auto) uniform_in(Range&& r)
    {
        auto const n = isize(r.size());
        CC_ASSERT(n > 0, "cannot pick from an empty range");
        return r[uniform(isize(0), n - 1)];
    }

    /// In-place uniform shuffle (forward Fisher-Yates). The loop order is part of the reproducibility contract.
    template <class Range>
    void shuffle(Range& r)
    {
        auto const n = isize(r.size());
        for (isize i = 1; i < n; ++i)
        {
            isize const j = uniform(isize(0), i);
            auto tmp = cc::move(r[i]);
            r[i] = cc::move(r[j]);
            r[j] = cc::move(tmp);
        }
    }

private:
    static constexpr u64 _default_seed = 0x853c49e6748fea9bull;
    static constexpr u64 _default_stream = 0xda3e39cb94b95bdbull; // odd increment selects the stream

    u64 _state = 0;
    u64 _inc = _default_stream;
};
} // namespace cc
