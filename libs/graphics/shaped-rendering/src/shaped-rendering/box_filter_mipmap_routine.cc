#include <clean-core/common/assert.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <shaped-rendering/impl/build_failure.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
namespace
{
/// The entry point of `shape` compiled for `values`, and the bindings of its group.
struct shape_program
{
    sg::async_compute_pipeline pipeline;
    cc::vector<sg::binding> bindings;
};

/// Each shape reads and writes through a binding of its own, which is why this is a switch over generated types.
[[nodiscard]] shape_program program_of(sg::context& ctx,
                                       mipmap_variant shape,
                                       sgl_shaders::box_filter_mipmap::options const& values)
{
    namespace file = sgl_shaders::box_filter_mipmap;
    switch (shape)
    {
    case mipmap_variant::tex_1d:
        return {.pipeline = file::main_1d_cs.acquire_pipeline(ctx, values),
                .bindings = file::mipmap_1d::declared_bindings(values)};
    case mipmap_variant::tex_1d_array:
        return {.pipeline = file::main_1d_array_cs.acquire_pipeline(ctx, values),
                .bindings = file::mipmap_1d_array::declared_bindings(values)};
    case mipmap_variant::tex_2d:
        return {.pipeline = file::main_2d_cs.acquire_pipeline(ctx, values),
                .bindings = file::mipmap_2d::declared_bindings(values)};
    case mipmap_variant::tex_2d_array:
        return {.pipeline = file::main_2d_array_cs.acquire_pipeline(ctx, values),
                .bindings = file::mipmap_2d_array::declared_bindings(values)};
    case mipmap_variant::tex_3d:
        return {.pipeline = file::main_3d_cs.acquire_pipeline(ctx, values),
                .bindings = file::mipmap_3d::declared_bindings(values)};
    case mipmap_variant::count_:
        break;
    }
    CC_UNREACHABLE("a mipmap key names one of the five shapes");
}

/// The group of `shape` reading `source` and writing `target`, filled through that shape's generated binding.
[[nodiscard]] sg::binding_group_handle group_of(sg::command_list& cmd,
                                                sg::binding_group_layout_handle const& layout,
                                                mipmap_variant shape,
                                                sg::raw_texture_view const& source,
                                                sg::raw_texture_view const& target)
{
    auto& transient = cmd.context().transient;
    switch (shape)
    {
    case mipmap_variant::tex_1d:
        return transient.create_binding_group(cmd, layout,
                                              sgl_shaders::box_filter_mipmap::mipmap_1d{
                                                  .source = source.as_texture<sg::tv_1d>(),
                                                  .target = sg::any_texture_view<sg::tv_1d>(target),
                                              });
    case mipmap_variant::tex_1d_array:
        return transient.create_binding_group(cmd, layout,
                                              sgl_shaders::box_filter_mipmap::mipmap_1d_array{
                                                  .source = source.as_texture<sg::tv_1d_array>(),
                                                  .target = sg::any_texture_view<sg::tv_1d_array>(target),
                                              });
    case mipmap_variant::tex_2d:
        return transient.create_binding_group(cmd, layout,
                                              sgl_shaders::box_filter_mipmap::mipmap_2d{
                                                  .source = source.as_texture<sg::tv_2d>(),
                                                  .target = sg::any_texture_view<sg::tv_2d>(target),
                                              });
    case mipmap_variant::tex_2d_array:
        return transient.create_binding_group(cmd, layout,
                                              sgl_shaders::box_filter_mipmap::mipmap_2d_array{
                                                  .source = source.as_texture<sg::tv_2d_array>(),
                                                  .target = sg::any_texture_view<sg::tv_2d_array>(target),
                                              });
    case mipmap_variant::tex_3d:
        return transient.create_binding_group(cmd, layout,
                                              sgl_shaders::box_filter_mipmap::mipmap_3d{
                                                  .source = source.as_texture<sg::tv_3d>(),
                                                  .target = sg::any_texture_view<sg::tv_3d>(target),
                                              });
    case mipmap_variant::count_:
        break;
    }
    CC_UNREACHABLE("a mipmap key names one of the five shapes");
}
} // namespace

cc::shared_async<cc::unit> box_filter_mipmap_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shaders.
    _program = nullptr;

    // One key per instance, so only the shapes and formats a caller actually asks for are ever compiled.
    auto const program = program_of(ctx, params().shape, {.target_format = params().format});
    co_await cc::async_settled(program.pipeline);
    auto const* const built = program.pipeline->try_value();
    if (built == nullptr)
    {
        impl::log_build_failure(program.pipeline, "box_filter_mipmap", params().format);
        fail_init(); // the shader did not build, which a format SGL has no image of never does
        co_return;
    }

    // Published as one immutable block, so execute reads a program that is either wholly there or not there at all.
    _program = std::make_shared<mipmap_program const>(
        mipmap_program{.layout = ctx.cached.acquire_binding_group_layout(program.bindings), .pipeline = *built});
    co_return;
}

void box_filter_mipmap_routine::_dispatch_level(sg::command_list& cmd,
                                                box_filter_mipmap_routine const& self,
                                                sg::raw_view const& source,
                                                sg::raw_view const& target,
                                                int x,
                                                int y,
                                                int z)
{
    // execute already established that the routine is ready and its program built, once for the whole chain.
    auto const group = group_of(cmd, self._program->layout, self.params().shape, sg::as_texture_view(source),
                                sg::as_texture_view(target));

    cmd.compute.bind_pipeline(*self._program->pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(x, y, z);
}
} // namespace sr
