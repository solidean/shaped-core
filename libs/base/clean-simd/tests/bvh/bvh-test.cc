#include "bvh8-query.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/math/random.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The acceptance test for clean-simd's design: an 8-wide BVH over cimd::storage nodes, queried through runtime
// dispatch by every kernel the CPU runs, against a brute-force scan.
// The query lives in a static library that dispatches, which is the link case the kernel-library ordering has to hold
// for — cimd_check_link_map(clean-simd-test) reads it.

namespace
{
cc::vector<bvh8_box> random_boxes(cc::random& rng, int count, f32 extent)
{
    auto boxes = cc::vector<bvh8_box>();
    for (auto i = 0; i < count; ++i)
    {
        auto b = bvh8_box{};
        for (auto a = 0; a < 3; ++a)
        {
            auto const c = rng.uniform(0.f, 100.f);
            auto const h = rng.uniform(0.f, extent);
            b.min[a] = c - h;
            b.max[a] = c + h;
        }
        boxes.push_back(b);
    }
    return boxes;
}

bool overlaps(bvh8_box const& a, bvh8_box const& b)
{
    for (auto k = 0; k < 3; ++k)
        if (a.min[k] > b.max[k] || a.max[k] < b.min[k])
            return false;
    return true;
}

cc::vector<i32> brute_force(cc::span<bvh8_box const> prims, bvh8_box const& q)
{
    auto out = cc::vector<i32>();
    for (auto i = 0; i < int(prims.size()); ++i)
        if (overlaps(prims[i], q))
            out.push_back(i);
    return out;
}

cc::vector<i32> dispatched(bvh8 const& tree, bvh8_box const& q)
{
    auto out = cc::vector<i32>::create_defaulted(64);
    auto n = CIMD_DISPATCH(bvh8_query_dispatched)(tree.view(), q, out.data(), int(out.size()));
    if (n > int(out.size()))
    {
        out.resize_to_defaulted(n);
        n = CIMD_DISPATCH(bvh8_query_dispatched)(tree.view(), q, out.data(), int(out.size()));
    }
    out.resize_down_to(n);
    cc::sort(out);
    return out;
}

bool same(cc::span<i32 const> a, cc::span<i32 const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = 0; i < int(a.size()); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

TEST("cimd bvh8 - every kernel finds exactly the boxes a brute-force scan finds")
{
    auto rng = nx::test_random();
    auto const prims = random_boxes(rng, nx::is_thorough() ? 20000 : 2000, 2.f);
    auto const tree = build_bvh8(prims);
    auto const queries = random_boxes(rng, 50, 8.f);

    for (auto const id : {cimd::kernel_id::scalar, cimd::kernel_id::sse2, cimd::kernel_id::sse42, cimd::kernel_id::avx2,
                          cimd::kernel_id::avx512, cimd::kernel_id::neon, cimd::kernel_id::simd128})
    {
        if (!cimd::cpu_supports(id))
            continue;
        cimd::scoped_forced_kernel const forced(id);
        for (auto const& q : queries)
            CHECK(same(dispatched(tree, q), brute_force(prims, q))).context(cimd::kernel_name(id));
    }
}

TEST("cimd bvh8 - the avx512 kernel finds what scalar does")
{
    if (!cimd::cpu_supports(cimd::kernel_id::avx512))
        SKIP("no avx512 on this CPU");
    auto rng = nx::test_random();
    auto const prims = random_boxes(rng, 2000, 2.f);
    auto const tree = build_bvh8(prims);
    for (auto const& q : random_boxes(rng, 50, 8.f))
    {
        cimd::scoped_forced_kernel const forced(cimd::kernel_id::avx512);
        CHECK(same(dispatched(tree, q), brute_force(prims, q)));
    }
}

BENCHMARK("cimd bvh8 - query per kernel against a brute-force scan")
{
    auto rng = cc::random(7);
    for (auto const count : {10000, 1000000})
    {
        auto const prims = random_boxes(rng, count, 0.5f);
        auto const tree = build_bvh8(prims);
        auto const queries = random_boxes(rng, 256, 2.f);
        i32 out[4096];

        if (count <= 10000)
            nx::bench::run(cc::format("{} boxes - brute force", count),
                           [&]
                           {
                               auto hits = 0;
                               for (auto const& q : queries)
                                   for (auto const& p : prims)
                                       hits += overlaps(p, q) ? 1 : 0;
                               nx::bench::sink(hits);
                           });
        for (auto const id :
             {cimd::kernel_id::scalar, cimd::kernel_id::sse2, cimd::kernel_id::sse42, cimd::kernel_id::avx2,
              cimd::kernel_id::avx512, cimd::kernel_id::neon, cimd::kernel_id::simd128})
        {
            if (!cimd::cpu_supports(id))
                continue;
            nx::bench::run(cc::format("{} boxes - {}", count, cimd::kernel_name(id)),
                           [&]
                           {
                               cimd::scoped_forced_kernel const forced(id);
                               auto hits = 0;
                               for (auto const& q : queries)
                                   hits += CIMD_DISPATCH(bvh8_query_dispatched)(tree.view(), q, out, 4096);
                               nx::bench::sink(hits);
                           });
        }
    }
}
