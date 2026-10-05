#include "kernel_case.hh"

using namespace cc::primitive_defines;

// The shape of a value and its mask: the layout, the looped types above eight registers, and the mask operations.
// float-test.cc and integer-test.cc check what the lanes compute.

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
