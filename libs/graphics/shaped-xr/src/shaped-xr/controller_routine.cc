#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-xr/controller_routine.hh>
#include <sxr_sgl_shaders.hh>
#include <typed-geometry/linalg/mat.hh>

using namespace cc::primitive_defines;

namespace
{
namespace shaders = sxr::sgl_shaders::controller;

constexpr auto grip_size = tg::vec3f(0.04f, 0.04f, 0.10f);
constexpr auto ray_width = 0.002f;
constexpr auto ray_length = 2.0f;
constexpr auto grip_color = tg::vec4f(0.20f, 0.22f, 0.26f, 1.0f);
constexpr auto ray_color = tg::vec4f(0.45f, 0.75f, 1.0f, 1.0f);

/// The unit box scaled by `size`, its centre at `center` in `pose`'s frame.
[[nodiscard]] tg::mat4f box_in(tg::rigid_transform3f const& pose, tg::vec3f size, tg::vec3f center)
{
    auto box = tg::mat4f::identity;
    for (auto i = 0; i < 3; ++i)
    {
        box[i, i] = size[i];
        box[3, i] = center[i];
    }
    return pose.to_mat() * box;
}
} // namespace

slib::shader_package const& sxr::shader_package()
{
    return sgl_shaders::package();
}

cc::vector<sxr::controller_pose> sxr::controller_poses(session const& s)
{
    auto out = cc::vector<controller_pose>();
    for (auto const h : {hand::left, hand::right})
        if (auto const grip = s.pose("grip", h); grip.has_value())
            out.push_back({.grip = grip.value(), .aim = s.pose("aim", h)});
    return out;
}

cc::shared_async<cc::unit> sxr::controller_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    auto const vs = shaders::main_vs->acquire(ctx);
    auto const ps = shaders::main_ps->acquire(ctx);

    // settled, so a shader that did not compile becomes this routine's verdict rather than an error
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    _pipeline = {};
    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init();
        co_return;
    }

    // A description naming no target set takes the shader's, which would tie this to one pass.
    // It draws into the caller's, so it names none at all.
    auto fragment_shader = *compiled_ps;
    fragment_shader.target_set = {};

    _pipeline = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = shaders::main_vs.acquire_layout(ctx),
        .vertex_shader = *compiled_vs,
        .fragment_shader = fragment_shader,
        .topology = sg::primitive_topology::triangle_strip,
        .rasterization = {.cull = sg::cull_mode::none},
        .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::less},
        .color_targets = {{.format = params()}},
        .depth_stencil_format = sg::pixel_format::depth32_float,
        .view_count = 2,
    });
    co_await cc::async_settled(_pipeline);
}

sg::routine_outcome sxr::controller_routine::execute(sg::rendering_scope& scope,
                                                     frame const& frame,
                                                     cc::span<controller_pose const> hands)
{
    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "controllers must be drawn into a scope with a color target");
    CC_ASSERT(frame.views.size() == 2, "controllers are drawn into a frame that renders both eyes");

    auto const self = try_acquire(cmd, scope.color_formats()[0]);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;

    auto const left = frame.views[0].world_to_clip();
    auto const right = frame.views[1].world_to_clip();
    auto const draw = [&](tg::mat4f const& box, tg::vec4f color)
    {
        scope.set_inline_constants(
            shaders::constants{.left_eye = left * box, .right_eye = right * box, .color = color}.to_block());
        scope.draw({.vertex_range = {.offset = 0, .size = 14}});
    };

    scope.bind_pipeline(**pipeline);
    for (auto const& h : hands)
    {
        draw(box_in(h.grip, grip_size, tg::vec3f(0, 0, 0)), grip_color);
        if (h.aim.has_value())
            draw(box_in(h.aim.value(), tg::vec3f(ray_width, ray_width, ray_length), tg::vec3f(0, 0, ray_length * 0.5f)),
                 ray_color);
    }
    return sg::routine_outcome::executed;
}
