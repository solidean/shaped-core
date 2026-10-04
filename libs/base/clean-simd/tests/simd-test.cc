#include "kernel_case.hh"

#include <clean-core/math/bit.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Every kernel is checked against plain C++ on the same lanes, so a kernel that disagrees with the scalar one is
// caught even where both are wrong in different ways.

namespace
{
template <class T>
T random_lane(cc::random& rng)
{
    if constexpr (std::is_same_v<T, f32>)
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
    if constexpr (std::is_same_v<T, f32>)
        return a + b;
    else
        return T(u32(a) + u32(b));
}

template <class T>
T wrap_sub(T a, T b)
{
    if constexpr (std::is_same_v<T, f32>)
        return a - b;
    else
        return T(u32(a) - u32(b));
}

template <class T>
T wrap_mul(T a, T b)
{
    if constexpr (std::is_same_v<T, f32>)
        return a * b;
    else
        return T(u32(a) * u32(b));
}

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
} // namespace

TEST("cimd simd - lane-wise arithmetic, compares, select and reductions agree with plain C++")
{
    auto rng = nx::test_random();
    for_each_kernel_and_width(
        [&]<class K, int N>
        {
            check_element<f32, K, N>(rng);
            check_element<i32, K, N>(rng);
            check_element<u32, K, N>(rng);
        });
}

TEST("cimd simd - the layout is the same on every kernel")
{
    for_each_kernel(
        []<class K>
        {
            static_assert(sizeof(cimd::f32x8<K>) == 32 && alignof(cimd::f32x8<K>) == 32);
            static_assert(sizeof(cimd::f32x16<K>) == 64 && alignof(cimd::f32x16<K>) == 64);
            static_assert(sizeof(cimd::simd<f32, 32, K>) == 128 && alignof(cimd::simd<f32, 32, K>) == 64);
            static_assert(sizeof(cimd::f32x8_storage) == 32 && alignof(cimd::f32x8_storage) == 32);

            auto const s = cimd::f32x8_storage{{0, 1, 2, 3, 4, 5, 6, 7}};
            cimd::f32x8<K> const v = s;
            for (auto i = 0; i < 8; ++i)
                CHECK(v.lane(i) == f32(i)).context(cimd::kernel_name(K::id));
        });
}

TEST("cimd simd - integer bit operations, negation and abs")
{
    auto rng = nx::test_random();
    for_each_kernel(
        [&]<class K>
        {
            auto const kn = cimd::kernel_name(K::id);
            auto const sa = random_storage<i32, 8>(rng);
            auto const sb = random_storage<i32, 8>(rng);
            cimd::i32x8<K> const a = sa;
            cimd::i32x8<K> const b = sb;
            cimd::i32x8_storage const band = a & b;
            cimd::i32x8_storage const bor = a | b;
            cimd::i32x8_storage const bxor = a ^ b;
            cimd::i32x8_storage const bnot = ~a;
            cimd::i32x8_storage const neg = -a;
            cimd::i32x8_storage const abs = a.abs();
            for (auto i = 0; i < 8; ++i)
            {
                auto const x = sa.lanes[i];
                CHECK(band.lanes[i] == (x & sb.lanes[i])).context(kn);
                CHECK(bor.lanes[i] == (x | sb.lanes[i])).context(kn);
                CHECK(bxor.lanes[i] == (x ^ sb.lanes[i])).context(kn);
                CHECK(bnot.lanes[i] == ~x).context(kn);
                CHECK(neg.lanes[i] == i32(0u - u32(x))).context(kn);
                CHECK(abs.lanes[i] == (x < 0 ? i32(0u - u32(x)) : x)).context(kn);
            }
        });
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

TEST("cimd simd - permute reads an 8-bit index unsigned, so all 256 lanes are reachable")
{
    for_each_kernel(
        []<class K>
        {
            cimd::storage<u8, 256> values;
            cimd::storage<i8, 256> indices;
            for (auto i = 0; i < 256; ++i)
            {
                values.lanes[i] = u8(i);
                indices.lanes[i] = i8(u8(255 - i)); // every index of 128 and above is a negative i8
            }
            cimd::storage<u8, 256> const got = cimd::simd<u8, 256, K>(values).permute(indices);
            for (auto i = 0; i < 256; ++i)
                CHECK(got.lanes[i] == u8(255 - i)).context(cimd::kernel_name(K::id));
        });
}

namespace
{
// Looped where the type spans more than eight registers, flat where it does not; the results are the same either way.
template <class T, int N, class K>
void check_looped(cc::random& rng)
{
    using V = cimd::simd<T, N, K>;
    static_assert(V::generated && V::is_loop_free == (V::registers <= 8));
    auto const kn = cimd::kernel_name(K::id);
    auto const sa = random_storage<T, N>(rng);
    auto const sb = random_storage<T, N>(rng);
    V const a = sa;
    V const b = sb;
    auto const less = a.lt(b);
    cimd::storage<T, N> const sum = a.add(b);
    cimd::storage<T, N> const picked = less.select(a, b);
    cimd::storage<T, N> const reversed = a.reverse();
    auto tree = sa;
    for (auto n = N / 2; n > 0; n /= 2)
        for (auto i = 0; i < n; ++i)
            tree.lanes[i] = wrap_add(tree.lanes[i], tree.lanes[i + n]);
    CHECK(a.reduce_add() == tree.lanes[0]).context(kn);
    auto any = false;
    auto all = true;
    for (auto i = 0; i < N; ++i)
    {
        auto const x = sa.lanes[i];
        auto const y = sb.lanes[i];
        CHECK(sum.lanes[i] == wrap_add(x, y)).context(kn);
        CHECK(picked.lanes[i] == (x < y ? x : y)).context(kn);
        CHECK(reversed.lanes[i] == sa.lanes[N - 1 - i]).context(kn);
        if constexpr (N <= 64)
            CHECK((((less.bits() >> i) & 1u) != 0) == (x < y)).context(kn);
        any = any || x < y;
        all = all && x < y;
    }
    CHECK(less.any() == any).context(kn);
    CHECK(less.all() == all).context(kn);
}
} // namespace

TEST("cimd simd - above eight registers every operation loops, with the flat types' results")
{
    auto rng = nx::test_random();
    for_each_kernel(
        [&]<class K>
        {
            // f32x128 loops on every kernel but avx512, the other two on 128-bit kernels; i32x64 has bits().
            check_looped<f32, 128, K>(rng);
            check_looped<i32, 64, K>(rng);
            check_looped<u8, 128, K>(rng);
            static_assert(!cimd::simd<f32, 128, K>::is_loop_free || K::native_bits == 512);
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

TEST("cimd mask - bits, from_bits, any, all and none")
{
    auto rng = nx::test_random();
    for_each_kernel_and_width(
        [&]<class K, int N>
        {
            using M = cimd::mask<32, N, K>;
            auto const kn = cimd::kernel_name(K::id);
            static_assert(M::generated && M::is_loop_free);
            for (auto iter = 0; iter < 20; ++iter)
            {
                auto const full = N == 32 ? 0xFFFFFFFFu : (1u << N) - 1u;
                auto b = rng.next_u32() & full;
                if (iter == 0)
                    b = 0;
                if (iter == 1)
                    b = full;
                auto const m = M::from_bits(b);
                CHECK(m.bits() == b).context(kn);
                CHECK(m.any() == (b != 0)).context(kn);
                CHECK(m.all() == (b == full)).context(kn);
                CHECK(m.none() == (b == 0)).context(kn);
                CHECK((~m).bits() == (~b & full)).context(kn);
                auto const other = M::from_bits(rng.next_u32() & full);
                CHECK((m & other).bits() == (b & other.bits())).context(kn);
                CHECK((m | other).bits() == (b | other.bits())).context(kn);
                CHECK((m ^ other).bits() == (b ^ other.bits())).context(kn);
            }
        });
}
