#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-shader-library/fwd.hh>
#include <shaped-xr/xr.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// One hand as `sxr::controller_routine` draws it, in the session's space.
struct sxr::controller_pose
{
    tg::rigid_transform3f grip;
    /// Where the hand points, along +z; no ray is drawn without it.
    cc::optional<tg::rigid_transform3f> aim;
};

namespace sxr
{
/// The shaders shaped-xr's routines draw with, for the application's `slib::shader_library` to mount.
[[nodiscard]] slib::shader_package const& shader_package();

/// Every tracked hand, from the `grip` and `aim` actions of `standard_controller_actions`, which the session must have.
[[nodiscard]] cc::vector<controller_pose> controller_poses(session const& s);
} // namespace sxr

/// Draws each hand as a plain box and its aim ray as a thin one, into an already open rendering scope of a frame.
///
/// The scope must cover both views in one pass (`view_count = 2`) with `frame.depth`, whose format the pipeline bakes.
/// A real controller model can replace the box behind this same call.
/// The shaders come from `sxr::shader_package()`, which the application's shader library must mount.
///
/// **Parametrized on the target's pixel format**, which is only known once a frame exists, so `execute` can decline.
/// Asking registers the routine and `ctx.routines.tick()` builds it, so the first frames draw no controllers.
class sxr::controller_routine : public sg::render_routine<controller_routine, sg::pixel_format>
{
public:
    /// Draws `hands` with `frame`'s views into the open `scope`.
    /// Declines while the shaders are still compiling, and after a compile that failed.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     frame const& frame,
                                                     cc::span<controller_pose const> hands);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::async_raster_pipeline _pipeline;
};
