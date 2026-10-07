#include <clean-core/common/assert.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
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
                                       sgl_shaders::box_filter_mipmap_options const& values)
{
    auto const& file = sgl_shaders::box_filter_mipmap;
    switch (shape)
    {
    case mipmap_variant::tex_1d:
        return {.pipeline = file.main_1d_cs.acquire_pipeline(ctx, values),
                .bindings = sgl_shaders::mipmap_1d::declared_bindings(values)};
    case mipmap_variant::tex_1d_array:
        return {.pipeline = file.main_1d_array_cs.acquire_pipeline(ctx, values),
                .bindings = sgl_shaders::mipmap_1d_array::declared_bindings(values)};
    case mipmap_variant::tex_2d:
        return {.pipeline = file.main_2d_cs.acquire_pipeline(ctx, values),
                .bindings = sgl_shaders::mipmap_2d::declared_bindings(values)};
    case mipmap_variant::tex_2d_array:
        return {.pipeline = file.main_2d_array_cs.acquire_pipeline(ctx, values),
                .bindings = sgl_shaders::mipmap_2d_array::declared_bindings(values)};
    case mipmap_variant::tex_3d:
        return {.pipeline = file.main_3d_cs.acquire_pipeline(ctx, values),
                .bindings = sgl_shaders::mipmap_3d::declared_bindings(values)};
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
    // Every shape's binding declares its source first and its target second.
    auto& ctx = cmd.context();
    sg::slotted_view const views[]
        = {{.slot = sg::binding_slot(0), .view = source}, {.slot = sg::binding_slot(1), .view = target}};
    auto const group = ctx.transient.create_binding_group(self._program->layout, views);

    cmd.compute.bind_pipeline(*self._program->pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(x, y, z);
}
} // namespace sr
