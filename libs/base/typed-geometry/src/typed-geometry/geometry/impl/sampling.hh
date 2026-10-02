#pragma once

#include <clean-core/math/random.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>

/// The building blocks every object's `sample_uniform` draws through.
///
/// Each draws a fixed number of values per sample where that is not much slower, so a seeded sequence is reproducible
/// sample by sample; the ball is the exception, where rejection measured twice as fast.
/// libs/base/typed-geometry/tests/benchmarks/sampling-benchmark.cc is that measurement.

namespace tg::impl
{
/// a scalar the generator can draw uniformly: cc::random serves f32 and f64.
template <class T>
concept samplable = requires(cc::random& rng) { rng.uniform(T(0), T(1)); };

/// a unit vector, uniform over the circle or the sphere.
/// The sphere draws z uniformly in [-1, 1] and an angle around z: by Archimedes' hat-box theorem, equal slabs of z
/// carry equal area.
template <int D, class T>
    requires(D == 2 || D == 3)
[[nodiscard]] vec<D, T> uniform_direction(cc::random& rng)
{
    auto const [s, c] = tg::sin_cos(angle<T>::make_from_radians(rng.uniform(T(0), T(2) * tg::pi<T>)));
    if constexpr (D == 2)
        return vec<D, T>(c, s);
    else
    {
        auto const z = rng.uniform(T(-1), T(1));
        auto const r = tg::sqrt(T(1) - z * z);
        return vec<D, T>(r * c, r * s, z);
    }
}

/// a point uniform in the unit ball, by rejection from the enclosing cube.
/// Measured against the direct method (a direction at a radius u^(1/D)) it is 2.1x faster in 2D and 2.5x in 3D, which
/// outweighs its varying draw count: 4 / pi ~ 1.27 attempts on average in 2D, 6 / pi ~ 1.91 in 3D.
template <int D, class T>
    requires(D == 2 || D == 3)
[[nodiscard]] vec<D, T> uniform_in_unit_ball(cc::random& rng)
{
    while (true)
    {
        vec<D, T> v;
        for (int i = 0; i < D; ++i)
            v.data[i] = rng.uniform(T(-1), T(1));
        if (v.length_sqr() <= T(1))
            return v;
    }
}
} // namespace tg::impl
