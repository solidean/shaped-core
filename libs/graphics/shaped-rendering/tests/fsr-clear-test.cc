#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <sr_sgl_shaders.hh>

using namespace cc::primitive_defines;

// FSR 3.1's clear job, fsr_clear_float.sgl and fsr_clear_uint.sgl, compiled for each format sr::fsr_upscale_routine
// clears and run on an image of it.
//
// The upscale tests measure a whole stream, which a clear that wrote the wrong channel or the wrong value would only
// nudge, so each format's bytes are read back here.

namespace
{
constexpr auto clear_usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src;

/// The value FSR's float clears are given here: one distinct value per channel, each exact in every format below.
constexpr f32 k_value[] = {1.0f, -1.0f, 0.25f, 2.0f};

/// A 2x2 image of `format` cleared by the compile of `entry` for it, read back whole.
template <class Entry, class Target, class Constants>
[[nodiscard]] cc::shared_async<cc::vector<byte>> cleared(sg::context& ctx,
                                                         Entry const& entry,
                                                         typename Entry::options const& values,
                                                         Constants const& constants,
                                                         sg::pixel_format format)
{
    auto const pipeline = entry.acquire_pipeline(ctx, values);
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    REQUIRE(built != nullptr).context(cc::format("the clear of pixel_format {} did not build", int(format)));
    auto const layout = ctx.cached.acquire_binding_group_layout(Target::declared_bindings(values));

    auto const image
        = ctx.persistent.create_texture_2d({.format = format, .width = 2, .height = 2, .usage = clear_usage});
    auto cmd = ctx.create_command_list();
    cmd->compute.bind_pipeline(**built);
    cmd->compute.bind_group(
        0, *ctx.transient.create_binding_group(*cmd, layout, Target{.target = image.as_any_image_view()}));
    cmd->compute.set_inline_constants(constants.to_block());
    cmd->compute.dispatch_threads(2, 2, 1);
    auto const back = cmd->download.bytes_from_texture(image.raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const data = co_await back.bytes();
    auto bytes = cc::vector<byte>::create_filled(data.size(), byte(0));
    cc::memcpy(bytes.data(), data.data(), bytes.size());
    co_return bytes;
}

/// The `T`s of a tightly-packed readback.
template <class T>
[[nodiscard]] cc::vector<T> values_of(cc::span<byte const> bytes)
{
    auto values = cc::vector<T>::create_filled(bytes.size() / isize(sizeof(T)), T{});
    cc::memcpy(values.data(), bytes.data(), values.size() * isize(sizeof(T)));
    return values;
}

[[nodiscard]] cc::shared_async<cc::vector<byte>> cleared_float(sg::context& ctx, sg::pixel_format format)
{
    co_return co_await cleared<decltype(sr::sgl_shaders::fsr_clear_float.main_cs), sr::sgl_shaders::fsr_clear_float_target>(
        ctx, sr::sgl_shaders::fsr_clear_float.main_cs, {.target_format = format},
        sr::sgl_shaders::fsr_clear_float_constants{.value = tg::vec4f(k_value[0], k_value[1], k_value[2], k_value[3])},
        format);
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - fsr's clear writes each format's leading channels of the value it is given",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // a unorm channel stores 1.0 as its largest step
    for (auto const v : values_of<u8>(co_await cleared_float(ctx, sg::pixel_format::r8_unorm)))
        CHECK(v == 255);

    // a half stores 1.0 and -1.0 exactly: 0x3c00 and 0xbc00
    for (auto const v : values_of<u16>(co_await cleared_float(ctx, sg::pixel_format::r16_float)))
        CHECK(v == 0x3c00);
    auto const two = values_of<u16>(co_await cleared_float(ctx, sg::pixel_format::rg16_float));
    REQUIRE(two.size() == 8);
    for (auto i = isize(0); i < two.size(); i += 2)
    {
        CHECK(two[i] == 0x3c00);
        CHECK(two[i + 1] == 0xbc00);
    }

    auto const four = values_of<f32>(co_await cleared_float(ctx, sg::pixel_format::rgba32_float));
    REQUIRE(four.size() == 16);
    for (auto i = isize(0); i < four.size(); ++i)
        CHECK(four[i] == k_value[i % 4]);

    // an unsigned image holds the float's bits, which the routine hands over as they are
    auto const bits
        = co_await cleared<decltype(sr::sgl_shaders::fsr_clear_uint.main_cs), sr::sgl_shaders::fsr_clear_uint_target>(
            ctx, sr::sgl_shaders::fsr_clear_uint.main_cs, {.target_format = sg::pixel_format::r32_uint},
            sr::sgl_shaders::fsr_clear_uint_constants{.value = 0x3f800000u}, sg::pixel_format::r32_uint);
    for (auto const v : values_of<u32>(bits))
        CHECK(v == 0x3f800000u);
}
