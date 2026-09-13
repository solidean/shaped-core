#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
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

cc::shared_async<cc::unit> box_filter_mipmap_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shaders.
    _program = nullptr;

    using asset_ptr = decltype(sr::shaders::box_filter_mipmap.compute.main_2d_cs);
    asset_ptr const entries[]
        = {sr::shaders::box_filter_mipmap.compute.main_1d_cs, sr::shaders::box_filter_mipmap.compute.main_1d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_2d_cs, sr::shaders::box_filter_mipmap.compute.main_2d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_3d_cs};
    static_assert(sizeof(entries) / sizeof(entries[0]) == int(mipmap_variant::count_), "one entry point per variant");

    // One variant per instance, so only what a caller actually asks for is ever compiled — the laziness the
    // per-variant cache used to provide, now a property of which instances exist.
    auto const shader = entries[int(params())]->acquire(ctx);
    co_await cc::async_settled(shader);
    auto const* const compiled = shader->try_value();
    if (compiled == nullptr)
    {
        fail_init(); // the shader did not build, and will not until a reload
        co_return;
    }

    auto layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}});

    auto const pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    if (built == nullptr)
    {
        fail_init();
        co_return;
    }

    // Published as one immutable block, so execute reads a program that is either wholly there or not there at all.
    _program = std::make_shared<mipmap_program const>(mipmap_program{.layout = cc::move(layout), .pipeline = *built});
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
    auto& ctx = cmd.context();
    auto const& names = traits_of[int(self.params())];

    auto const group = ctx.transient.create_binding_group(
        self._program->layout, {{.name = names.source, .view = source}, {.name = names.target, .view = target}});

    cmd.compute.bind_pipeline(*self._program->pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(x, y, z);
}
} // namespace sr
