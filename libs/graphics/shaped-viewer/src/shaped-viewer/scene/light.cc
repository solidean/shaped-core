#include <clean-core/common/log.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/atomic.hh>
#include <shaped-viewer/scene/light.hh>
#include <typed-geometry/linalg/cross.hh> // tg::cross + tg::dual

namespace sv
{
area_light_gpu area_light_gpu::from(area_light const& light)
{
    auto const& u = light.half_extent_u;
    auto const& v = light.half_extent_v;

    // A negative component can only be area_light's "never assigned" default emission.
    // Warned once per process: this sits on the per-frame path.
    if (light.emission[0] < 0 || light.emission[1] < 0 || light.emission[2] < 0)
    {
        static auto warned = cc::atomic_flag();
        if (!warned.test_and_set())
            CC_LOG_WARNING("an area_light has a negative emission component — set area_light::emission "
                           "(its default is deliberately negative)");
    }

    // cross(u, v) gives the emitting face directly.
    return {.center = tg::vec3f(light.center[0], light.center[1], light.center[2]),
            .u = u,
            .v = v,
            .emission = light.emission,
            .normal = tg::dual(tg::cross(u, v)).normalized()};
}
} // namespace sv
