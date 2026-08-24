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
                                                                                                  variant v)
{
    // One chain rather than two blocking waits: the shader compile and the pipeline build are both async, and
    // awaiting them parks this frame instead of holding a thread.
    // This is where a coroutine belongs — init_declare itself is a `void` virtual and cannot be one.
    using asset_ptr = decltype(sr::shaders::box_filter_mipmap.compute.main_2d_cs);
    asset_ptr const entries[]
        = {sr::shaders::box_filter_mipmap.compute.main_1d_cs, sr::shaders::box_filter_mipmap.compute.main_1d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_2d_cs, sr::shaders::box_filter_mipmap.compute.main_2d_array_cs,
           sr::shaders::box_filter_mipmap.compute.main_3d_cs};
    static_assert(sizeof(entries) / sizeof(entries[0]) == int(variant::count_), "one entry point per variant");

    auto const& compiled = co_await entries[int(v)]->acquire(ctx);

    auto layout = ctx.cached.acquire_binding_group_layout(compiled.bindings);
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {layout}});
    auto pipeline = co_await ctx.cached.acquire_compute_pipeline({.shader = compiled, .layout = pipeline_layout});

    co_return std::make_shared<mipmap_program const>(
        mipmap_program{.layout = cc::move(layout), .pipeline = cc::move(pipeline)});
}

void box_filter_mipmap_routine::init_declare(sg::context& ctx)
{
    // Nothing is compiled here.
    // init_declare is documented to *kick off* work, and a caller that only ever mips 2D textures should not
    // wait on the 1D, 3D and array shaders to reach its first frame.
    // Re-init also clears what was built against the previous shaders, which is what a reload needs.
    _programs.init(ctx, [](sg::context& c, variant const& v) { return _build_program(c, v); });
}

void box_filter_mipmap_routine::_dispatch_level(sg::command_list& cmd,
                                                sg::raw_view const& source,
                                                sg::raw_view const& target,
                                                variant v,
                                                int x,
                                                int y,
                                                int z)
{
    auto const& self = acquire(cmd);

    // Fallible rather than throwing: this runs inside the caller's command list, and an exception unwinding out
    // of here would leave it unsubmitted.
    auto const program = self._programs.try_acquire(v);
    if (program.has_error() || program.value() == nullptr)
        return; // this variant's shader did not compile

    auto& ctx = cmd.context();
    auto const& names = traits_of[int(v)];

    auto const group = ctx.transient.create_binding_group(
        program.value()->layout, {{.name = names.source, .view = source}, {.name = names.target, .view = target}});

    cmd.compute.bind_pipeline(*program.value()->pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(x, y, z);
}
} // namespace sr
