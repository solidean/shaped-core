// What filling a quadric batch costs, and where that cost goes.
//
// The design's motivating workload is a mesh's structure re-authored EVERY FRAME at tens of thousands to millions of
// primitives, so `sv::quadric_set::add` sits on the per-frame path by construction.
// `add` does three things per primitive: build the record, fold it into the content hash, and fold its box into the
// bounds.
// The hash is the part with a choice in it, and this is what says how much of the total it is.
//
// The two candidates it separates:
//
//   folded    one XXH3-128 over the 120-byte record, plus one more over the 32-byte digest pair, per `add` --
//             so 2N hash calls, and `hash()` is free whenever it is asked.
//   bulk      one XXH3-128 over the whole primitive span, lazily, on the first `hash()` after a mutation --
//             one streaming pass, and `hash()` costs that pass once per mutation batch.
//
// Both give the identical invariant: equal contents give equal hashes, order-sensitive, because the byte range IS the
// primitive order.
// `no_hash` is the floor -- the same fill with neither -- so the two are read against what the fill costs anyway
// rather than against each other alone.
//
// Measured on a Ryzen 7 260, release-clang, at the three scales below:
//
//   primitives   no hash    folded      bulk      hashing alone: folded / bulk
//       10,000   27.8 us    278 us      70.5 us   250 us / 42.7 us   = 5.9x
//      100,000    676 us    2.83 ms     1.04 ms   2.15 ms / 364 us   = 5.9x
//    1,000,000    9.0 ms    29.9 ms     13.6 ms   20.9 ms / 4.6 ms   = 4.5x
//
// So the fold is roughly five times the cost of the streaming pass for the same invariant, and at the million the
// design names it is the difference between 30 ms and 14 ms on a path that runs every frame.
// The reason is the call count rather than the bytes: 2N short-input hashes against one long-input pass, and XXH3's
// per-call setup is what dominates at 120 and 32 bytes.

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/scene/quadric.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>

using namespace cc::primitive_defines;

namespace
{
/// Sweep points measure different amounts of work, so they carry no baseline.
constexpr auto sweep_config = nx::bench::run_config{
    .min_time_secs = 0.1,
    .max_samples = 256,
    .target_relative_error = 0.03,
    .no_baseline = true,
};

/// The primitives one fill appends, built once so the benchmark times the FILL rather than the geometry factories.
///
/// Half spheres and half tubes, which is the mesh-structure shape: a closed mesh has roughly three edges per vertex, so
/// a real batch is tube-heavy, and the two records cost the same to fold either way.
cc::vector<sv::quadric_primitive> make_primitives(isize count)
{
    auto out = cc::vector<sv::quadric_primitive>();
    out.reserve(count);

    for (auto i = isize(0); i < count; ++i)
    {
        auto const t = float(i) * 0.013f;
        auto const p = tg::pos3f(tg::sin(tg::angle_f::make_from_radians(t)) * 40.0f,
                                 tg::cos(tg::angle_f::make_from_radians(t * 1.7f)) * 40.0f, float(i) * 0.001f);

        if ((i & 1) == 0)
            out.push_back(sv::quadric_primitive::create_sphere(tg::sphere3f(p, 0.02f)));
        else
            out.push_back(sv::quadric_primitive::create_cylinder(tg::segment3f(p, p + tg::vec3f(0.1f, 0.05f, 0)), 0.008f));
    }
    return out;
}

/// The fill as it is written today: `add` per primitive, folding the hash as it goes.
nx::bench::result folded(cc::string_view name, cc::span<sv::quadric_primitive const> prims)
{
    return nx::bench::run(name, sweep_config,
                          [&](nx::bench::iteration& it)
                          {
                              auto set = sv::quadric_set();
                              set.reserve(prims.size());
                              for (auto const& p : prims)
                                  set.add(p);

                              nx::bench::sink(set.hash().low);
                              it.items(prims.size());
                          });
}

/// The same fill with no hashing at all — the floor the other two are read against.
nx::bench::result no_hash(cc::string_view name, cc::span<sv::quadric_primitive const> prims)
{
    return nx::bench::run(
        name, sweep_config,
        [&](nx::bench::iteration& it)
        {
            auto out = cc::vector<sv::quadric_primitive>();
            out.reserve(prims.size());

            auto bounds = cc::optional<tg::aabb3f>();
            for (auto const& p : prims)
            {
                // The bounds fold, kept: it is O(1) per primitive with no bulk alternative, since a
                // union of boxes is not a byte range.
                if (bounds.has_value())
                {
                    for (auto axis = 0; axis < 3; ++axis)
                    {
                        bounds.value().min[axis] = cc::min(bounds.value().min[axis], p.bounds.min[axis]);
                        bounds.value().max[axis] = cc::max(bounds.value().max[axis], p.bounds.max[axis]);
                    }
                }
                else
                    bounds = p.bounds;

                out.push_back(p);
            }

            nx::bench::sink(out.size() + isize(bounds.has_value()));
            it.items(prims.size());
        });
}

/// The same fill, hashed in ONE streaming pass at the end — what a lazy `hash()` would do.
nx::bench::result bulk(cc::string_view name, cc::span<sv::quadric_primitive const> prims)
{
    return nx::bench::run(
        name, sweep_config,
        [&](nx::bench::iteration& it)
        {
            auto out = cc::vector<sv::quadric_primitive>();
            out.reserve(prims.size());

            auto bounds = cc::optional<tg::aabb3f>();
            for (auto const& p : prims)
            {
                if (bounds.has_value())
                {
                    for (auto axis = 0; axis < 3; ++axis)
                    {
                        bounds.value().min[axis] = cc::min(bounds.value().min[axis], p.bounds.min[axis]);
                        bounds.value().max[axis] = cc::max(bounds.value().max[axis], p.bounds.max[axis]);
                    }
                }
                else
                    bounds = p.bounds;

                out.push_back(p);
            }

            auto const h = cc::hash128::create(cc::span<sv::quadric_primitive const>(out).as_bytes(),
                                               sv::impl::quadric_hash_seed);
            nx::bench::sink(h.low);
            it.items(prims.size());
        });
}

void sweep(isize count)
{
    auto const prims = make_primitives(count);

    (void)no_hash(cc::format("fill {} - no hash", count), prims);
    (void)folded(cc::format("fill {} - folded per add", count), prims);
    (void)bulk(cc::format("fill {} - one bulk hash", count), prims);
}
} // namespace

// The three scales the design talks about: a small drawing, the dense example, and the million the doc promises.
BENCHMARK("bench-quadric-set - fill, folded vs bulk hash")
{
    sweep(10'000);
    sweep(100'000);
    sweep(1'000'000);
}
