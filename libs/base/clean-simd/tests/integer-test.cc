#include "kernel_case.hh"

using namespace cc::primitive_defines;

// Integer lanes: wrapping arithmetic and unsigned compares against plain C++, the bit operations, and permute's index.

TEST("cimd simd - i32 arithmetic, compares, select and reductions agree with plain C++")
{
    auto rng = nx::test_random();
    for_each_kernel_and_width([&]<class K, int N> { check_element<i32, K, N>(rng); });
}

TEST("cimd simd - u32 arithmetic, compares, select and reductions agree with plain C++")
{
    auto rng = nx::test_random();
    for_each_kernel_and_width([&]<class K, int N> { check_element<u32, K, N>(rng); });
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
