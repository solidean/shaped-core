#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes _build_program a coroutine
#include <shaped-graphics/all.hh>
#include <shaped-rendering/box_filter_mipmap_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
namespace
{
/// Everything that differs per variant: the entry point it compiles, and the binding names it declares.
/// The names differ because HLSL cannot overload a resource declaration on its dimension.
struct variant_traits
{
    cc::string_view source;
    cc::string_view target;
};

constexpr variant_traits traits_of[] = {
    {.source = "gSource1D", .target = "gTarget1D"}, {.source = "gSource1DArray", .target = "gTarget1DArray"},
    {.source = "gSource2D", .target = "gTarget2D"}, {.source = "gSource2DArray", .target = "gTarget2DArray"},
    {.source = "gSource3D", .target = "gTarget3D"},
};
} // namespace

cc::shared_async<std::shared_ptr<mipmap_program const>> box_filter_mipmap_routine::_build_program(sg::context& ctx,
                                                                                                  mipmap_variant v)
{
    // One chain rather than two waits: the shader compile and the pipeline build are both async, and awaiting them
    // in a coroutine parks instead of holding a thread.
    // init_declare still drives this to completion, because a `void` virtual cannot await — that is exactly what
    // changes when the phases become coroutines, and then this function IS init.
    using asset_ptr = decltype(sr::shaders::box_filter_mipmap.compute.main_2d_cs);
    asset_ptr const entries[]
        = {sr::shaders::box_filter_mipmap.compute.main_1d_cs, sr::shaders::box_filter_mipmap.compute.main_1d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_2d_cs, sr::shaders::box_filter_mipmap.compute.main_2d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_3d_cs};
    static_assert(sizeof(entries) / sizeof(entries[0]) == int(mipmap_variant::count_), "one entry point per variant");

    auto const& compiled = co_await entries[int(v)]->acquire(ctx);

    auto layout = ctx.cached.acquire_binding_group_layout(compiled.bindings);
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = compiled, .layout = pipeline_layout});

    co_return std::make_shared<mipmap_program const>(
        mipmap_program{.layout = cc::move(layout), .pipeline = cc::move(pipeline)});
}

void box_filter_mipmap_routine::init_declare(sg::context& ctx)
{
    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shaders.
    _program = nullptr;

    // One variant per instance, so only what a caller actually asks for is ever compiled — the laziness the
    // per-variant cache used to provide, now a property of which instances exist.
    auto node = _build_program(ctx, params());
    (void)cc::try_async_blocking_get(node);
    if (auto const* const built = node->try_value())
        _program = *built;
    else
        fail_init(); // the shader or the pipeline did not build, and will not until a reload
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
    auto& ctx = cmd.context();
    auto const& names = traits_of[int(self.params())];

    auto const group = ctx.transient.create_binding_group(
        self._program->layout, {{.name = names.source, .view = source}, {.name = names.target, .view = target}});

    cmd.compute.bind_pipeline(*self._program->pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(x, y, z);
}
} // namespace sr
