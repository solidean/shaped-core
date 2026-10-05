#pragma once

#include <clean-core/math/bit.hh>
#include <clean-simd/all.hh>
#include <nexus/test.hh>

// The per-kernel machinery the clean-simd tests share, and the plain C++ every kernel is checked against.
// Every kernel is checked against plain C++ on the same lanes, so a kernel that disagrees with the scalar one is
// caught even where both are wrong in different ways.

#if !defined(CIMD_EXHAUSTIVE_TESTS)
#error "kernel_case.hh: CIMD_EXHAUSTIVE_TESTS must be 0 or 1 (SC_SIMD_EXHAUSTIVE_TESTS in CMake)"
#endif

// Runs `f.template operator()<K>()` for every kernel this test TU can compile.
// A kernel above the build's floor is not here; its tests go through cimd_dispatch, the way production code does.
template <class F>
void for_each_kernel(F&& f)
{
    f.template operator()<cimd::scalar>();
#if CIMD_HAS_SSE2
    f.template operator()<cimd::sse2>();
#endif
#if CIMD_HAS_SSE42
    f.template operator()<cimd::sse42>();
#endif
#if CIMD_HAS_AVX2
    f.template operator()<cimd::avx2>();
#endif
#if CIMD_HAS_AVX512
    f.template operator()<cimd::avx512>();
#endif
#if CIMD_HAS_NEON
    f.template operator()<cimd::neon>();
#endif
#if CIMD_HAS_SIMD128
    f.template operator()<cimd::simd128>();
#endif
}

// Runs `f.template operator()<K, N>()` for every kernel at the 32-bit lane counts under test.
// With CIMD_EXHAUSTIVE_TESTS that is every count all kernels generate flat; without, 8 lanes, which is 256 bits.
template <class F>
void for_each_kernel_and_width(F&& f)
{
    for_each_kernel(
        [&]<class K>
        {
            if constexpr (CIMD_EXHAUSTIVE_TESTS)
            {
                f.template operator()<K, 4>();
                f.template operator()<K, 8>();
                f.template operator()<K, 16>();
                f.template operator()<K, 32>();
            }
            else
            {
                f.template operator()<K, 8>();
            }
        });
}

/// The reduction order every kernel promises: lane i with lane i + n/2, recursively.
template <class T, class Op>
T reference_tree(T const* v, int n, Op op)
{
    T tmp[64];
    for (auto i = 0; i < n; ++i)
        tmp[i] = v[i];
    while (n > 1)
    {
        n /= 2;
        for (auto i = 0; i < n; ++i)
            tmp[i] = op(tmp[i], tmp[i + n]);
    }
    return tmp[0];
}

template <class T>
T random_lane(cc::random& rng)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return rng.uniform(-1000.f, 1000.f);
    else
        return T(rng.next_u32());
}

template <class T, int N>
cimd::storage<T, N> random_storage(cc::random& rng)
{
    cimd::storage<T, N> s;
    for (auto i = 0; i < N; ++i)
        s.lanes[i] = random_lane<T>(rng);
    return s;
}

// Wrapping integer arithmetic, the way every SIMD unit and the scalar kernel do it.
template <class T>
T wrap_add(T a, T b)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return a + b;
    else
        return T(cimd::u32(a) + cimd::u32(b));
}

template <class T>
T wrap_sub(T a, T b)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return a - b;
    else
        return T(cimd::u32(a) - cimd::u32(b));
}

template <class T>
T wrap_mul(T a, T b)
{
    if constexpr (std::is_same_v<T, cimd::f32>)
        return a * b;
    else
        return T(cimd::u32(a) * cimd::u32(b));
}

/// The lane-wise arithmetic, compares, select, reductions and lane access of `simd<T, N, K>`, on random lanes.
template <class T, class K, int N>
void check_element(cc::random& rng)
{
    using V = cimd::simd<T, N, K>;
    auto const kn = cimd::kernel_name(K::id);

    static_assert(V::generated);
    static_assert(V::is_loop_free);
    static_assert(sizeof(V) == N * sizeof(T));
    static_assert(alignof(V) == (N * sizeof(T) < 64 ? N * sizeof(T) : 64));
    static_assert(!requires(V v) { v * 2.0; }, "only exactly the element type broadcasts");

    for (auto iter = 0; iter < 20; ++iter)
    {
        auto const sa = random_storage<T, N>(rng);
        auto const sb = random_storage<T, N>(rng);
        V const a = sa;
        V const b = sb;

        cimd::storage<T, N> const sum = a + b;
        cimd::storage<T, N> const diff = a - b;
        cimd::storage<T, N> const prod = a * b;
        cimd::storage<T, N> const lo = a.min(b);
        cimd::storage<T, N> const hi = a.max(b);
        auto const lt = (a < b).bits();
        auto const eq = (a == a).bits();
        cimd::storage<T, N> const picked = (a < b).select(a, b);

        for (auto i = 0; i < N; ++i)
        {
            auto const x = sa.lanes[i];
            auto const y = sb.lanes[i];
            CHECK(sum.lanes[i] == wrap_add(x, y)).context(kn);
            CHECK(diff.lanes[i] == wrap_sub(x, y)).context(kn);
            CHECK(prod.lanes[i] == wrap_mul(x, y)).context(kn);
            CHECK(lo.lanes[i] == (y < x ? y : x)).context(kn);
            CHECK(hi.lanes[i] == (x < y ? y : x)).context(kn);
            CHECK((((lt >> i) & 1u) != 0) == (x < y)).context(kn);
            CHECK(((eq >> i) & 1u) != 0).context(kn);
            CHECK(picked.lanes[i] == (x < y ? x : y)).context(kn);
        }

        CHECK(a.reduce_add() == reference_tree(sa.lanes, N, [](T x, T y) { return wrap_add(x, y); })).context(kn);
        CHECK(a.reduce_min() == reference_tree(sa.lanes, N, [](T x, T y) { return y < x ? y : x; })).context(kn);
        CHECK(a.reduce_max() == reference_tree(sa.lanes, N, [](T x, T y) { return x < y ? y : x; })).context(kn);

        auto const lane = rng.uniform(0, N - 1);
        CHECK(a.lane(lane) == sa.lanes[lane]).context(kn);
        CHECK(a.with_lane(lane, sb.lanes[lane]).lane(lane) == sb.lanes[lane]).context(kn);
    }

    auto const iota = cimd::storage<T, N>(V::iota());
    auto const zero = cimd::storage<T, N>(V::zero());
    auto const two = cimd::storage<T, N>(V(T(2)));
    for (auto i = 0; i < N; ++i)
    {
        CHECK(iota.lanes[i] == T(i)).context(kn);
        CHECK(zero.lanes[i] == T(0)).context(kn);
        CHECK(two.lanes[i] == T(2)).context(kn);
    }
}
