#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <typed-geometry/linalg/vec.hh>

// What the benchmarks of SGL against a hand-written original share: the inputs, the targets and the timed loop.

namespace sr_bench::impl
{
using namespace cc::primitive_defines;

constexpr auto k_extent = tg::vec2i(1024, 1024);
constexpr int k_rounds = 64;

/// One side of a pair: what it is called, and what records one dispatch of it, ready once its pipeline is.
struct contender
{
    cc::string name;
    cc::unique_function<bool()> is_ready;
    cc::unique_function<void(sg::command_list&)> record;
    /// What the driver reports of the contender's pipeline, where its context captures it; empty otherwise.
    cc::unique_function<cc::vector<sg::driver_statistic>()> statistics;
};

/// `options` with the one that asks for SGL's own binary on `ctx`'s format, which a context of the text route honours.
[[nodiscard]] inline cc::vector<slib::shader_option> with_native(sg::context const& ctx,
                                                                 cc::vector<slib::shader_option> options)
{
    auto const name
        = ctx.backend() == sg::backend_kind::vulkan ? slib::k_native_spirv_option : slib::k_native_dxil_option;
    options.push_back({.name = cc::string(name), .value = cc::string("true")});
    return options;
}

/// A contender of `pipeline`, once it has built, recorded by `record`.
[[nodiscard]] inline contender contender_of(cc::string_view name,
                                            sg::context& ctx,
                                            sg::async_compute_pipeline const& pipeline,
                                            cc::unique_function<void(sg::command_list&, sg::compute_pipeline const&)>* record)
{
    return {.name = cc::string(name),
            .is_ready =
                [&ctx, &pipeline]
            {
                (void)ctx.backlog.start(pipeline);
                return pipeline->is_ready() && pipeline->try_value() != nullptr;
            },
            .record = [record, &pipeline](sg::command_list& cmd) { (*record)(cmd, **pipeline->try_value()); },
            .statistics = [&pipeline] { return (*pipeline->try_value())->driver_statistics(); }};
}

/// Each statistic the driver reports of the contenders' pipelines, a line per statistic and a column per contender.
/// Nothing where none reports any, which is every context but a vulkan one asked to capture them.
inline void log_driver_statistics(cc::string_view kernel, cc::span<contender> contenders)
{
    auto all = cc::vector<cc::vector<sg::driver_statistic>>();
    auto any = false;
    for (auto& c : contenders)
    {
        all.push_back(c.statistics ? c.statistics() : cc::vector<sg::driver_statistic>());
        any = any || !all.back().empty();
    }
    if (!any)
        return;
    for (auto const& s : all[0])
    {
        auto line = cc::format("{} driver: {} {}:", kernel, s.executable, s.name);
        for (auto i = isize(0); i < contenders.size(); ++i)
        {
            auto value = cc::string("-");
            for (auto const& t : all[i])
                if (t.executable == s.executable && t.name == s.name)
                    value = cc::format("{}", t.value);
            line.appendf(" {} {}", contenders[i].name, value);
        }
        CC_LOG_INFO("{}", line);
    }
}

/// The GPU milliseconds of each span, sorted.
[[nodiscard]] inline cc::vector<f64> sorted_ms(cc::span<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp> const> spans)
{
    auto ms = cc::vector<f64>();
    for (auto const& [from, to] : spans)
        ms.push_back(1e3 * (to.try_get_seconds().value() - from.try_get_seconds().value()));
    cc::sort(ms);
    return ms;
}

/// A value of `n` spread over [0, 1) by a fixed hash, so every run sees the same inputs.
[[nodiscard]] inline f32 noise(u32 n)
{
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    n ^= n >> 16;
    return f32(n >> 8) / f32(1u << 24);
}

/// An rgba32_float texture of `k_extent`, filled from `texel` and uploaded on a list of its own.
inline cc::shared_async<sg::texture_2d> filled_texture(sg::context& ctx,
                                                       u32 seed,
                                                       cc::function_ref<tg::vec4f(i32, i32, u32)> texel)
{
    auto texels = cc::vector<tg::vec4f>();
    texels.reserve(isize(k_extent[0]) * k_extent[1]);
    for (auto y = 0; y < k_extent[1]; ++y)
        for (auto x = 0; x < k_extent[0]; ++x)
            texels.push_back(texel(x, y, seed * 0x9e3779b9u + u32(y * k_extent[0] + x) * 31u));
    auto t = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                               .width = k_extent[0],
                                               .height = k_extent[1],
                                               .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(t.raw(), cc::span<tg::vec4f const>(texels).as_bytes());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_return t;
}

[[nodiscard]] inline sg::texture_2d target_texture(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = k_extent[0],
         .height = k_extent[1],
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
}

/// Records `rounds` dispatches of each contender, alternating, and logs what they took.
/// `batch` dispatches stand between one pair of timestamps, for a device whose timestamps are coarse.
/// `by_wall_clock` times each batch from one queue completion to the next instead, for a device whose timestamps do not
/// bracket the work between them; a device without timestamp queries is timed so as well.
inline cc::shared_async<cc::unit> measure(sg::context& ctx,
                                          cc::string_view kernel,
                                          cc::span<contender> contenders,
                                          int batch = 1,
                                          bool by_wall_clock = false)
{
    // Untimed until every contender has executed once, which is what its shaders compiling takes.
    auto const start = cc::current_time_steady_secs();
    for (auto& c : contenders)
    {
        while (!c.is_ready())
        {
            REQUIRE(cc::current_time_steady_secs() - start < 120.0);
            (void)ctx.routines.tick();
            ctx.advance_epoch();
            co_await ctx.idle_completion();
        }
        auto cmd = ctx.create_command_list();
        c.record(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }
    log_driver_statistics(kernel, contenders);

    // Without timestamp queries a dispatch is timed as a batch of them on the wall clock, from one completion to the
    // next, and the batches of a pair alternate as single dispatches do.
    if (by_wall_clock || !ctx.supports(sg::feature::timestamp_query))
    {
        auto wall = cc::vector<cc::vector<f64>>();
        for (auto i = isize(0); i < contenders.size(); ++i)
            wall.push_back({});
        // a completion is seen on a tick of about 16 ms in a browser runtime, so a batch must be long beside it, and few
        for (auto r = 0; r < 8; ++r)
            for (auto i = isize(0); i < contenders.size(); ++i)
            {
                co_await ctx.idle_completion();
                // recorded before the clock starts, so it times the GPU running the batch and not the CPU writing it
                auto cmd = ctx.create_command_list();
                for (auto k = 0; k < batch; ++k)
                    contenders[i].record(*cmd);
                auto const from = cc::current_time_steady_secs();
                ctx.submit_command_list(cc::move(cmd));
                ctx.advance_epoch();
                co_await ctx.idle_completion();
                wall[i].push_back(1e3 * (cc::current_time_steady_secs() - from) / batch);
            }
        for (auto& w : wall)
            cc::sort(w);
        for (auto i = isize(0); i < contenders.size(); ++i)
        {
            auto const median = wall[i][wall[i].size() / 2];
            // the minimum's ratio too: other work on the GPU only ever adds to a batch, so it is the steadier one
            CC_LOG_INFO("{}: {} median {} ms of wall time per dispatch, min {} ms, {}x {} by median, {}x by min",
                        kernel, contenders[i].name, median, wall[i][0], median / wall[0][wall[0].size() / 2],
                        contenders[0].name, wall[i][0] / wall[0][0]);
        }
        co_return;
    }

    auto spans = cc::vector<cc::vector<cc::pair<sg::gpu_timestamp, sg::gpu_timestamp>>>();
    for (auto i = isize(0); i < contenders.size(); ++i)
        spans.push_back({});
    for (auto r = 0; r < k_rounds; ++r)
        for (auto i = isize(0); i < contenders.size(); ++i)
        {
            auto cmd = ctx.create_command_list();
            auto const from = cmd->query.record_gpu_timestamp();
            for (auto k = 0; k < batch; ++k)
                contenders[i].record(*cmd);
            auto const to = cmd->query.record_gpu_timestamp();
            ctx.submit_command_list(cc::move(cmd));
            ctx.advance_epoch();
            spans[i].push_back({from, to});
        }
    co_await ctx.idle_completion();

    auto const baseline = sorted_ms(spans[0]);
    for (auto i = isize(0); i < contenders.size(); ++i)
    {
        auto const ms = sorted_ms(spans[i]);
        auto const median = ms[ms.size() / 2] / batch;
        CC_LOG_INFO("{}: {} median {} ms, min {} ms, {}x {}", kernel, contenders[i].name, median, ms[0] / batch,
                    ms[ms.size() / 2] / baseline[baseline.size() / 2], contenders[0].name);
    }
}
} // namespace sr_bench::impl
