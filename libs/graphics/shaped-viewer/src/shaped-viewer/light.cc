#include <shaped-viewer/light.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual

namespace sv
{
area_light_gpu area_light_gpu::from(area_light const& light)
{
    auto const& m = light.transform;
    auto const col_xyz = [&](int c) { return tg::vec3f(m[c, 0], m[c, 1], m[c, 2]); };
    auto const u = light.half_extents[0] * col_xyz(0);
    auto const v = light.half_extents[1] * col_xyz(1);

    // cross(u, v) gives the local +z (emitting) face directly.
    return {.center = col_xyz(3),
            .u = u,
            .v = v,
            .emission = light.emission,
            .normal = tg::dual(tg::cross(u, v)).normalized()};
}
} // namespace sv
