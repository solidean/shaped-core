#include "kernel_case.hh"

using namespace cc::primitive_defines;

// Float lanes: arithmetic against plain C++, and the cases where an emulation is easiest to get wrong.

namespace
{
template <class T>
auto bits_of(T x)
{
    if constexpr (sizeof(T) == 4)
        return cc::bit_cast<u32>(x);
    else
        return cc::bit_cast<u64>(x);
}

// A payload of one, so a kernel that returns some other NaN is caught.
template <class T>
T quiet_nan()
{
    if constexpr (sizeof(T) == 4)
        return cc::bit_cast<f32>(0x7FC00001u);
    else
        return cc::bit_cast<f64>(0x7FF8000000000001ull);
}

// The IEEE results, signed zeros included: an emulation that steps -1 up to +0 is wrong here and nowhere else.
template <class T, class K>
void check_rounding()
{
    using V = cimd::simd<T, 16, K>;
    auto const kn = cimd::kernel_name(K::id);
    auto const x = cimd::storage<T, 16>{{T(-0.0), T(-0.3), T(-0.5), T(-0.7), T(-1.0), T(0.3), T(2.5), T(-2.5), T(1.5),
                                         T(-1.5), T(0.0), T(0.5), T(0.7), T(-2.7), T(8388609.0), T(3.5)}};
    auto const floor = cimd::storage<T, 16>{{T(-0.0), T(-1), T(-1), T(-1), T(-1), T(0), T(2), T(-3), T(1), T(-2), T(0),
                                             T(0), T(0), T(-3), T(8388609.0), T(3)}};
    auto const ceil = cimd::storage<T, 16>{{T(-0.0), T(-0.0), T(-0.0), T(-0.0), T(-1), T(1), T(3), T(-2), T(2), T(-1),
                                            T(0), T(1), T(1), T(-2), T(8388609.0), T(4)}};
    auto const round = cimd::storage<T, 16>{{T(-0.0), T(-0.0), T(-0.0), T(-1), T(-1), T(0), T(2), T(-2), T(2), T(-2),
                                             T(0), T(0), T(1), T(-3), T(8388609.0), T(4)}};
    auto const trunc = cimd::storage<T, 16>{{T(-0.0), T(-0.0), T(-0.0), T(-0.0), T(-1), T(0), T(2), T(-2), T(1), T(-1),
                                             T(0), T(0), T(0), T(-2), T(8388609.0), T(3)}};
    V const v = x;
    cimd::storage<T, 16> const got_floor = v.floor();
    cimd::storage<T, 16> const got_ceil = v.ceil();
    cimd::storage<T, 16> const got_round = v.round();
    cimd::storage<T, 16> const got_trunc = v.trunc();
    for (auto i = 0; i < 16; ++i)
    {
        CHECK(bits_of(got_floor.lanes[i]) == bits_of(floor.lanes[i])).context(kn).dump("x", x.lanes[i]);
        CHECK(bits_of(got_ceil.lanes[i]) == bits_of(ceil.lanes[i])).context(kn).dump("x", x.lanes[i]);
        CHECK(bits_of(got_round.lanes[i]) == bits_of(round.lanes[i])).context(kn).dump("x", x.lanes[i]);
        CHECK(bits_of(got_trunc.lanes[i]) == bits_of(trunc.lanes[i])).context(kn).dump("x", x.lanes[i]);
    }
}

// NEON's vminq and vmaxq order -0 below +0 and propagate a NaN, so it alone is left out.
template <class T, class K>
void check_min_max_ties()
{
    using V = cimd::simd<T, 8, K>;
    auto const kn = cimd::kernel_name(K::id);
    auto const nan = quiet_nan<T>();
    auto const a = cimd::storage<T, 8>{{T(0.0), T(-0.0), T(0.0), T(-0.0), nan, T(1), T(-0.0), nan}};
    auto const b = cimd::storage<T, 8>{{T(-0.0), T(0.0), T(0.0), T(-0.0), T(1), nan, nan, T(0.0)}};
    cimd::storage<T, 8> const lo = V(a).min(V(b));
    cimd::storage<T, 8> const hi = V(a).max(V(b));
    for (auto i = 0; i < 8; ++i)
    {
        CHECK(bits_of(lo.lanes[i]) == bits_of(a.lanes[i])).context(kn).dump("lane", i);
        CHECK(bits_of(hi.lanes[i]) == bits_of(a.lanes[i])).context(kn).dump("lane", i);
    }
}
} // namespace

TEST("cimd simd - f32 arithmetic, compares, select and reductions agree with plain C++")
{
    auto rng = nx::test_random();
    for_each_kernel_and_width([&]<class K, int N> { check_element<f32, K, N>(rng); });
}

TEST("cimd simd - float negation, abs and mul_add")
{
    auto rng = nx::test_random();
    for_each_kernel(
        [&]<class K>
        {
            auto const kn = cimd::kernel_name(K::id);
            auto const sa = random_storage<f32, 8>(rng);
            auto const sb = random_storage<f32, 8>(rng);
            auto const sc = random_storage<f32, 8>(rng);
            cimd::f32x8<K> const a = sa;
            cimd::f32x8_storage const neg = -a;
            cimd::f32x8_storage const abs = (-a).abs();
            cimd::f32x8_storage const fma = a.mul_add(sb, sc);
            for (auto i = 0; i < 8; ++i)
            {
                auto const x = sa.lanes[i];
                CHECK(neg.lanes[i] == -x).context(kn);
                CHECK(abs.lanes[i] == (x < 0 ? -x : x)).context(kn);
                // Fused or not is the kernel's business: one rounding or two, each within an f32 ulp of what it rounds.
                auto const prod = f64(x) * f64(sb.lanes[i]);
                auto const exact = prod + f64(sc.lanes[i]);
                auto const err = f64(fma.lanes[i]) - exact;
                auto const scale = (prod < 0 ? -prod : prod) + (exact < 0 ? -exact : exact);
                CHECK((err < 0 ? -err : err) <= scale * 0x1p-23).context(kn);
            }
            auto const negzero = cimd::f32x8_storage((cimd::f32x8<K>(-0.f)).abs());
            CHECK(cc::bit_cast<u32>(negzero.lanes[0]) == 0u).context(kn);
        });
}

TEST("cimd simd - floor, ceil, round and trunc give the IEEE result, the sign of zero included")
{
    for_each_kernel(
        []<class K>
        {
            check_rounding<f32, K>();
            check_rounding<f64, K>();
        });
}

TEST("cimd simd - min and max return a on a tie of zeros or a NaN, on every kernel but neon")
{
    for_each_kernel(
        []<class K>
        {
            if constexpr (K::id != cimd::kernel_id::neon)
            {
                check_min_max_ties<f32, K>();
                check_min_max_ties<f64, K>();
            }
        });
}

TEST("cimd simd - conversions between f32 and 32-bit integers")
{
    auto rng = nx::test_random();
    for_each_kernel(
        [&]<class K>
        {
            auto const kn = cimd::kernel_name(K::id);
            auto const sf = random_storage<f32, 8>(rng);
            auto const su = random_storage<u32, 8>(rng);
            auto const si = random_storage<i32, 8>(rng);
            cimd::i32x8_storage const truncated = cimd::f32x8<K>(sf).template convert<i32>();
            cimd::f32x8_storage const from_u = cimd::u32x8<K>(su).template convert<f32>();
            cimd::f32x8_storage const from_i = cimd::i32x8<K>(si).template convert<f32>();
            for (auto i = 0; i < 8; ++i)
            {
                CHECK(truncated.lanes[i] == i32(sf.lanes[i])).context(kn);
                CHECK(from_u.lanes[i] == f32(su.lanes[i])).context(kn);
                CHECK(from_i.lanes[i] == f32(si.lanes[i])).context(kn);
            }
        });
}
