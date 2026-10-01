// Uniform sampling of a ball: rejection from the enclosing cube, which sample_uniform uses, against the direct method.
// Direct draws a fixed four values and pays a sin_cos, a sqrt and a cbrt; rejection draws three values per attempt and
// needs 6 / pi ~ 1.91 attempts on average in 3D, 4 / pi ~ 1.27 in 2D, with no transcendental at all.
// On a Ryzen 9 5900X rejection measured 2.5x faster in 3D and 2.1x in 2D, which is why it won despite its varying
// draw count; rerun this when the scalar math routing changes.
//
// Run with
//   uv run dev.py benchmark "tg sampling"

#include <clean-core/math/random.hh>
#include <nexus/bench/run.hh>
#include <nexus/test.hh>
#include <typed-geometry/geometry/impl/sampling.hh>

namespace
{
constexpr int batch = 1024;

/// a direction at a radius whose D-th power is uniform: a fixed four draws, and the transcendentals
template <int D>
tg::vec<D, float> by_direct_method(cc::random& rng)
{
    auto const u = rng.uniform(0.0f, 1.0f);
    auto const dir = tg::impl::uniform_direction<D, float>(rng);
    if constexpr (D == 2)
        return dir * tg::sqrt(u);
    else
        return dir * tg::pow(u, 1.0f / 3.0f);
}

template <int D>
void compare()
{
    nx::bench::run("direct",
                   [&](nx::bench::iteration& it)
                   {
                       auto rng = cc::random(1);
                       auto acc = 0.0f;
                       for (auto i = 0; i < batch; ++i)
                           acc += by_direct_method<D>(rng).data[0];
                       nx::bench::sink(acc);
                       it.items(batch);
                   });
    nx::bench::run("rejection",
                   [&](nx::bench::iteration& it)
                   {
                       auto rng = cc::random(1);
                       auto acc = 0.0f;
                       for (auto i = 0; i < batch; ++i)
                           acc += tg::impl::uniform_in_unit_ball<D, float>(rng).data[0];
                       nx::bench::sink(acc);
                       it.items(batch);
                   });
}
} // namespace

BENCHMARK("tg sampling - ball 3D: direct vs rejection")
{
    compare<3>();
}

BENCHMARK("tg sampling - disk 2D: direct vs rejection")
{
    compare<2>();
}
