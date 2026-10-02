// The hot-pair closed forms against the GJK floor they replace, on the same random inputs.
// The query-matrix plan priced GJK at roughly 5-10x a closed form for box-box and far more for point-box;
// these say what it costs here.
//
// Run with
//   uv run dev.py benchmark "tg query"

#include <clean-core/math/random.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

namespace
{
constexpr int pair_count = 256;

tg::pos3f random_pos(cc::random& rng, float extent)
{
    return tg::pos3f(rng.uniform(-extent, extent), rng.uniform(-extent, extent), rng.uniform(-extent, extent));
}

tg::aabb3f random_aabb(cc::random& rng)
{
    auto const c = random_pos(rng, 3.0f);
    auto const h = tg::vec3f(rng.uniform(0.1f, 1.5f), rng.uniform(0.1f, 1.5f), rng.uniform(0.1f, 1.5f));
    return tg::aabb3f(c - h, c + h);
}

template <class A, class B, class Make>
void compare_distance(Make make)
{
    auto rng = cc::random(23);
    A as[pair_count] = {};
    B bs[pair_count] = {};
    for (auto i = 0; i < pair_count; ++i)
    {
        as[i] = make.a(rng);
        bs[i] = make.b(rng);
    }

    nx::bench::run("closed form",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = 0.0f;
                       for (auto i = 0; i < pair_count; ++i)
                           acc += nx::bench::keep(as[i]).distance_sqr_to(bs[i]);
                       nx::bench::sink(acc);
                       it.items(pair_count);
                   });
    nx::bench::run("gjk",
                   [&](nx::bench::iteration& it)
                   {
                       auto acc = 0.0f;
                       for (auto i = 0; i < pair_count; ++i)
                       {
                           auto const r = tg::impl::gjk(nx::bench::keep(as[i]), bs[i]);
                           acc += (r.on_b - r.on_a).length_sqr();
                       }
                       nx::bench::sink(acc);
                       it.items(pair_count);
                   });
}

struct aabb_pair
{
    tg::aabb3f a(cc::random& rng) const { return random_aabb(rng); }
    tg::aabb3f b(cc::random& rng) const { return random_aabb(rng); }
};
struct sphere_aabb_pair
{
    tg::sphere3f a(cc::random& rng) const { return tg::sphere3f(random_pos(rng, 3.0f), rng.uniform(0.1f, 2.0f)); }
    tg::aabb3f b(cc::random& rng) const { return random_aabb(rng); }
};
struct segment_pair
{
    tg::segment3f a(cc::random& rng) const { return tg::segment3f(random_pos(rng, 4.0f), random_pos(rng, 4.0f)); }
    tg::segment3f b(cc::random& rng) const { return tg::segment3f(random_pos(rng, 4.0f), random_pos(rng, 4.0f)); }
};
struct point_triangle_pair
{
    tg::pos3f a(cc::random& rng) const { return random_pos(rng, 4.0f); }
    tg::triangle3f b(cc::random& rng) const
    {
        return tg::triangle3f(random_pos(rng, 4.0f), random_pos(rng, 4.0f), random_pos(rng, 4.0f));
    }
};
} // namespace

BENCHMARK("tg query - aabb distance: closed form vs gjk")
{
    compare_distance<tg::aabb3f, tg::aabb3f>(aabb_pair());
}

BENCHMARK("tg query - ball to aabb distance: closed form vs gjk")
{
    compare_distance<tg::sphere3f, tg::aabb3f>(sphere_aabb_pair());
}

BENCHMARK("tg query - segment distance: closed form vs gjk")
{
    compare_distance<tg::segment3f, tg::segment3f>(segment_pair());
}

BENCHMARK("tg query - point to triangle distance: closed form vs gjk")
{
    compare_distance<tg::pos3f, tg::triangle3f>(point_triangle_pair());
}
