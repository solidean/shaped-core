#include "../backends/sg_backends.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/bench/run_async.hh>
#include <nexus/test.hh>
#include <sg_test_shaders.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/shader_asset.hh>

// What it costs to create a frame's render targets instead of keeping them.
//
// A frame of a simulated post-process chain: ten render targets, each pass reading one and writing the next.
// The three variants differ only in where the targets come from each frame:
//   - persistent: ctx.persistent.create_* every frame, released when the frame drops them;
//   - transient:  ctx.transient.create_* every frame, force-expired at the next epoch;
//   - preallocated: created once and reused, which is the floor the other two are read against.
// A backend without real memory heaps turns every transient create into a dedicated allocation.
// How much that costs is the question this answers with a number rather than a guess.

namespace
{
constexpr int frames_per_iteration = 10;
constexpr int targets_per_frame = 10;

enum class target_source
{
    persistent,
    transient,
    preallocated,
};

sg::texture_2d_description target_description(int width, int height)
{
    return {.format = sg::pixel_format::rgba16_float,
            .width = width,
            .height = height,
            .usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture};
}

cc::vector<sg::texture_2d> make_targets(sg::context& ctx, target_source source, int width, int height)
{
    auto targets = cc::vector<sg::texture_2d>();
    for (auto i = 0; i < targets_per_frame; ++i)
        targets.push_back(source == target_source::transient
                              ? ctx.transient.create_texture_2d(target_description(width, height))
                              : ctx.persistent.create_texture_2d(target_description(width, height)));
    return targets;
}

cc::shared_async<cc::unit> render_frames(sg::context_handle ctx,
                                         sg::compute_pipeline_handle pipeline,
                                         sg::binding_group_layout_handle layout,
                                         target_source source,
                                         int width,
                                         int height,
                                         cc::vector<sg::texture_2d> const* preallocated)
{
    for (auto frame = 0; frame < frames_per_iteration; ++frame)
    {
        auto const targets
            = source == target_source::preallocated ? *preallocated : make_targets(*ctx, source, width, height);

        auto cmd = ctx->create_command_list();
        cmd->compute.bind_pipeline(*pipeline);
        for (auto pass = 0; pass < targets_per_frame; ++pass)
        {
            auto const& from = targets[pass];
            auto const& to = targets[(pass + 1) % targets_per_frame];
            auto const group
                = ctx->transient.create_binding_group(layout, {{.name = "gSource", .view = from.as_readonly_view()},
                                                               {.name = "gTarget", .view = to.as_readwrite_view()}});
            cmd->compute.bind_group(0, *group);
            cmd->compute.dispatch_threads(width, height);
        }
        (void)ctx->submit_command_list(cc::move(cmd));

        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    co_await ctx->idle_completion();
}

char const* name_of(target_source source)
{
    switch (source)
    {
    case target_source::persistent:
        return "persistent per frame";
    case target_source::transient:
        return "transient per frame";
    case target_source::preallocated:
        return "preallocated";
    }
    return "?";
}
} // namespace

ASYNC_BENCHMARK("sg transient - per-frame render targets")
{
    (void)sg_test::shader_fixtures(); // the library the generated globals resolve through

    for (auto const& factory : sg_test::context_factories())
    {
        auto created = factory.create();
        if (created.has_error())
            continue; // no device for this backend on this machine
        auto const ctx = created.value();

        auto const& compiled = co_await sg::test::shaders::ping_pong.compute.main->acquire(*ctx);

        auto const layout = ctx->cached.acquire_binding_group_layout(compiled.bindings);
        auto const pipeline = co_await ctx->cached.acquire_compute_pipeline(
            {.shader = compiled,
             .layout = ctx->cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {layout}})});

        for (auto const [width, height] : {cc::pair{1920, 1080}, cc::pair{256, 256}})
        {
            auto const preallocated = make_targets(*ctx, target_source::preallocated, width, height);
            for (auto const source : {target_source::preallocated, target_source::persistent, target_source::transient})
                (void)co_await nx::bench::run_async(
                    cc::format("{} {}x{} {}", factory.backend, width, height, name_of(source)),
                    [&] { return render_frames(ctx, pipeline, layout, source, width, height, &preallocated); });
        }
        co_await ctx->idle_completion();
    }
}
