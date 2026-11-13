#include "../differential.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/diff_floats.hh>
#include <sgl_modules/diff_words.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

// The setups of the buffer-shaped corpus: one input of 256 elements, one output of four elements a thread.
// The inputs are generated rather than random, so every run on every machine reads the same ones.

using namespace cc::primitive_defines;

namespace
{
constexpr isize k_threads = 256;
constexpr isize k_outputs_per_thread = 4;

[[nodiscard]] u32 hashed(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb'352du;
    x ^= x >> 15;
    x *= 0x846c'a68bu;
    return x ^ (x >> 16);
}

[[nodiscard]] cc::vector<u32> word_inputs()
{
    auto words = cc::vector<u32>{0u, 1u, 0x8000'0000u, 0xffff'ffffu, 0x7fff'ffffu, 0x8000'0001u, 2u, 31u, 32u, 33u};
    for (auto i = words.size(); i < k_threads; ++i)
        words.push_back(hashed(u32(i)));
    return words;
}

/// -64 to 64, never zero, of every magnitude between: small fractions, whole numbers and values just off them.
[[nodiscard]] cc::vector<f32> float_inputs()
{
    auto floats = cc::vector<f32>();
    for (auto i = isize(0); i < k_threads; ++i)
    {
        auto const unit = f32(hashed(u32(i) + 0x9e37'79b9u) >> 8) / f32(1 << 24); // [0, 1)
        auto const magnitude = (i % 4 == 0) ? 64.0f * unit : (i % 4 == 1) ? unit : f32(i % 17) + 0.5f * unit;
        auto const value = cc::max(magnitude, 1.0f / 1024.0f);
        floats.push_back(i % 2 == 0 ? value : -value);
    }
    return floats;
}

template <class Group, class T>
cc::shared_async<cc::vector<sg_diff::output>> run_buffers(sg::context* ctx,
                                                          sg::compute_pipeline_handle pipeline,
                                                          cc::vector<T> inputs)
{
    auto const input = ctx->persistent.create_buffer_from_data(inputs, sg::buffer_usage::readonly_buffer);
    auto const output
        = ctx->persistent.create_buffer_from_data(cc::vector<T>::create_filled(k_threads * k_outputs_per_thread, T(0)),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const layout = ctx->cached.acquire_binding_group_layout<Group>();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, Group{.input = input.as_readonly_buffer(), .output = output.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(int(k_threads));
    auto const back = cmd->download.data_from_buffer(output);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await back.data();
    auto result = cc::vector<sg_diff::output>();
    result.push_back({.name = "output", .bytes = cc::vector<byte>::create_copy_of(cc::span<T const>(data).as_bytes())});
    co_return result;
}

bool const words_registered = sg_diff::register_setup({
    .name = "words",
    .layout = [](sg::context& ctx) { return ctx.cached.acquire_pipeline_layout<sgl_modules::diff_words::io>(); },
    .run = [](sg::context* ctx, sg::compute_pipeline_handle pipeline)
    { return run_buffers<sgl_modules::diff_words::io>(ctx, cc::move(pipeline), word_inputs()); },
    .routes = {},
    .across = {},
});

bool const floats_registered = sg_diff::register_setup({
    .name = "floats",
    .layout = [](sg::context& ctx) { return ctx.cached.acquire_pipeline_layout<sgl_modules::diff_floats::io>(); },
    .run = [](sg::context* ctx, sg::compute_pipeline_handle pipeline)
    { return run_buffers<sgl_modules::diff_floats::io>(ctx, cc::move(pipeline), float_inputs()); },
    // one context's driver compiles both routes' IR, so an add or a multiply rounds alike; a transcendental may not
    .routes = {.kind = sg_diff::tolerance::measure::ulps, .amount = 2},
    .across = {.kind = sg_diff::tolerance::measure::ulps, .amount = 16},
});
} // namespace
