#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/memory/node_allocation.hh>
#include <typed-geometry/scalar/fwd.hh>

#if defined(__FMA__) || defined(__AVX2__)
#include <immintrin.h>
#else
#include <cmath>
#endif

// The kernels behind tg::float_expansion_sbo<K>: non-template, over component arrays, so every K shares one copy.
// An expansion is a sum of doubles, smallest magnitude first, strongly nonoverlapping, with no zero component (Shewchuk 1997).
// Round to nearest and no contraction are assumed: tools/cmake/FloatingPoint.cmake turns contraction off.
// Users include scalar/float_expansion/float_expansion.hh.

namespace tg::impl
{
/// The smallest product magnitude at which two_product is known exact: its roundoff stays clear of the subnormals.
inline constexpr f64 float_expansion_min_product = 0x1p-960;

/// a b - c, rounded once.
/// std::fma is a library call where math-errno is on, as on MSVC targets, so a build with FMA takes the instruction itself.
CC_FORCE_INLINE f64 float_expansion_fms(f64 a, f64 b, f64 c)
{
#if defined(__FMA__) || defined(__AVX2__)
    return _mm_cvtsd_f64(_mm_fmsub_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
#elif defined(__clang__) || defined(__GNUC__)
    return __builtin_fma(a, b, -c);
#else
    return std::fma(a, b, -c);
#endif
}

/// h = a + b as at most two components, zeros dropped; the count written.
CC_FORCE_INLINE i32 float_expansion_two_sum(f64 a, f64 b, f64* h)
{
    auto const x = a + b;
    auto const bv = x - a;
    auto const av = x - bv;
    auto const y = (a - av) + (b - bv);
    CC_ASSERT(x - x == 0.0, "a float_expansion is exact only while its sums stay finite");
    auto n = 0;
    if (y != 0.0)
        h[n++] = y;
    if (x != 0.0)
        h[n++] = x;
    return n;
}

/// h = a b as at most two components, a and b nonzero; the count written.
CC_FORCE_INLINE i32 float_expansion_two_product(f64 a, f64 b, f64* h)
{
    auto const x = a * b;
    auto const y = float_expansion_fms(a, b, x);
    CC_ASSERT(x - x == 0.0 && (x >= float_expansion_min_product || x <= -float_expansion_min_product),
              "a float_expansion is exact only while its products stay within [2^-960, 2^1024)");
    if (y == 0.0)
    {
        h[0] = x;
        return 1;
    }
    h[0] = y;
    h[1] = x;
    return 2;
}

/// h = e + f, or e - f when negate_f; h holds en + fn and aliases neither input.
[[nodiscard]] i32 float_expansion_sum(f64 const* e, i32 en, f64 const* f, i32 fn, bool negate_f, f64* h);

/// h = e f; h holds 2 en fn and aliases neither input.
[[nodiscard]] i32 float_expansion_product(f64 const* e, i32 en, f64 const* f, i32 fn, f64* h);

/// h = the integer whose magnitude is the n limbs, least significant first; h holds 2 n.
/// The magnitude must stay below 2^1024.
[[nodiscard]] i32 float_expansion_from_limbs(u64 const* limbs, i32 n, bool negative, f64* h);

/// The components summed from the smallest up: within a few ulps of the value, and its sign.
[[nodiscard]] f64 float_expansion_approximate(f64 const* e, i32 en);

[[nodiscard]] inline f64* float_expansion_allocate(i32 components, u32& cap_class)
{
    auto const idx = cc::node_class_index_from_size_and_align(isize(components) * 8, 8);
    CC_ASSERT(int(idx) < 32, "a float_expansion's capacity class fits 5 bits");
    cap_class = u32(idx);
    return reinterpret_cast<f64*>(cc::default_node_allocator().allocate_node_bytes(idx, isize(1) << int(idx), 8)); // NOLINT
}

inline void float_expansion_free(f64* components, u32 cap_class)
{
    cc::node_allocation_free(reinterpret_cast<byte*>(components), cc::node_class_index(cap_class)); // NOLINT
}

/// The components a capacity class holds; class 0 is the inline storage, which the caller knows the size of.
[[nodiscard]] constexpr i32 float_expansion_class_components(u32 cap_class)
{
    return i32((u64(1) << cap_class) / 8);
}
} // namespace tg::impl
