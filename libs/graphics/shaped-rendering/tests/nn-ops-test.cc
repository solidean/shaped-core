#include "shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>


using namespace cc::primitive_defines;

// The network's operations beyond the convolution: the transfer the inputs go through and back.
// The pool and the upsample are in nn-resample-test.cc.
//
// The first test here is that they COMPILE, which is not something the build says.
// A shader package generates its descriptors from the source text and compiles at run time, so a shader error reaches nobody unless something acquires the shader and reads the error back.
// A denoise member that silently declines every frame looks exactly like one that is still warming up.

namespace
{
/// The options every pipeline here is built with: an output the tests read back as floats.
constexpr auto k_values = sr::sgl_shaders::nn_io_options{.target_format = sg::pixel_format::rgba32_float};

/// Builds one entry point of nn_io.sgl, failing with the compiler's own text rather than a null handle.
template <class Entry>
[[nodiscard]] cc::shared_async<sg::compute_pipeline_handle> build(sg::context& ctx, Entry const& entry, cc::string_view name)
{
    auto const shader = entry.acquire(ctx, k_values);
    co_await cc::async_settled(shader);
    if (shader->has_error())
        FAIL(cc::format("{} did not compile:\n{}", name, shader->try_error()->underlying().to_string()));

    auto const built = co_await entry.acquire_pipeline(ctx, k_values);
    REQUIRE(built != nullptr).context(cc::format("{} did not build a pipeline", name));
    co_return built;
}

/// OIDN's transfer curve, transcribed from `core/color.h` a second time and evaluated in double.
/// The shader is held to this rather than to a copy of its own arithmetic, so a mistyped constant in either shows.
[[nodiscard]] f64 pu_forward_raw(f64 y)
{
    if (y <= 1.57945760e-06)
        return 1.41283765e+03 * y;
    if (y <= 3.22087631e-02)
        return 1.64593172e+00 * tg::pow(y, 4.31384981e-01) + -2.94139609e-03;
    return 1.92653254e-01 * tg::log(y + 6.26026094e-03) + 9.98620152e-01;
}

[[nodiscard]] f64 pu_forward(f64 y)
{
    return pu_forward_raw(y) / pu_forward_raw(65504.0);
}

/// One texel per value through the input pass, and what it wrote: the tensor of twelve channels per texel.
[[nodiscard]] cc::shared_async<cc::vector<f32>> encode(sg::context& ctx,
                                                       sg::compute_pipeline_handle const& pipeline,
                                                       cc::span<tg::vec4f const> color,
                                                       cc::span<tg::vec4f const> albedo,
                                                       cc::span<tg::vec4f const> normal)
{
    auto const width = i32(color.size());
    auto const make = [&]
    {
        return ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                                 .width = width,
                                                 .height = 1,
                                                 .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    };
    auto const color_texture = make();
    auto const albedo_texture = make();
    auto const normal_texture = make();

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(color_texture.raw(), color.as_bytes());
    cmd->upload.bytes_to_texture(albedo_texture.raw(), albedo.as_bytes());
    cmd->upload.bytes_to_texture(normal_texture.raw(), normal.as_bytes());

    // Padded to a multiple of four channels, nine becoming twelve, which is what lets the convolution after it read four channels in one load.
    auto const packed = ctx.transient.create_buffer<f32>(
        width * 12, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::nn_input_images>();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *ctx.transient.create_binding_group(
                                   *cmd, layout,
                                   sr::sgl_shaders::nn_input_images{.color = color_texture.as_texture_view(),
                                                                    .albedo = albedo_texture.as_texture_view(),
                                                                    .normal = normal_texture.as_texture_view(),
                                                                    .target = packed.as_readwrite_buffer()}));
    cmd->compute.set_inline_constants(sr::sgl_shaders::nn_input_constants{.width = u32(width),
                                                                          .height = 1,
                                                                          .source_width = u32(width),
                                                                          .source_height = 1,
                                                                          .source_offset_x = 0,
                                                                          .source_offset_y = 0,
                                                                          .input_scale = 1.0f}
                                          .to_block());
    cmd->compute.dispatch_threads(width, 1, 1);

    auto const readback = sg::data_future<f32>(cmd->download.data_from_buffer(packed));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const encoded = co_await readback.data();
    auto out = cc::vector<f32>();
    for (auto const v : encoded)
        out.push_back(v);
    co_return out;
}
} // namespace

// Every operation the network is made of builds.
//
// Cheap, and it is the check that turns a shader mistake into a message here rather than into a member that declines every frame for a reason nothing prints.
ASYNC_INVOCABLE_TEST("sr - every network operation compiles", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const conv = sr::sgl_shaders::nn_conv.main_cs->acquire(ctx);
    co_await cc::async_settled(conv);
    if (conv->has_error())
        FAIL(cc::format("nn_conv did not compile:\n{}", conv->try_error()->underlying().to_string()));
    auto const conv_pipeline = co_await sr::sgl_shaders::nn_conv.main_cs.acquire_pipeline(ctx);
    CHECK(conv_pipeline != nullptr);

    auto const input_pipeline = co_await build(ctx, sr::sgl_shaders::nn_io.input_cs, "nn_io input_cs");
    auto const output_pipeline = co_await build(ctx, sr::sgl_shaders::nn_io.output_cs, "nn_io output_cs");
    CHECK(input_pipeline != nullptr);
    CHECK(output_pipeline != nullptr);
}

// The input pass, value by value against OIDN's curve, and what it does with what is not a number.
//
// The round trip below would pass a curve with a slightly wrong constant, because the inverse would carry the same mistake back out; this pins the encoding itself.
// A NaN or an infinity anywhere in a guide becomes zero, since one of either poisons every pixel the network's receptive field reaches.
ASYNC_INVOCABLE_TEST("sr - the network's input encodes as OIDN's curve does, and zeroes what is no finite number",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const pipeline = co_await build(ctx, sr::sgl_shaders::nn_io.input_cs, "nn_io input_cs");

    auto const nan = cc::bit_cast<f32>(0x7fc00000u);
    auto const inf = cc::bit_cast<f32>(0x7f800000u);

    // Either side of both knees, and the ends of the range.
    auto const values = cc::vector<f32>{0.0f, 1e-7f, 1.5e-6f, 1.6e-6f, 1e-4f, 0.03f, 0.033f, 0.5f, 100.0f, 65504.0f};
    auto color = cc::vector<tg::vec4f>();
    auto albedo = cc::vector<tg::vec4f>();
    auto normal = cc::vector<tg::vec4f>();
    for (auto const v : values)
    {
        color.push_back(tg::vec4f(v, v * 0.5f, -v, 0));
        albedo.push_back(tg::vec4f(0.25f, 1.5f, -0.5f, 0));
        normal.push_back(tg::vec4f(0.5f, -2.0f, 0.0f, 0));
    }
    // What is not a finite number, in every channel position and every guide.
    color.push_back(tg::vec4f(nan, inf, -inf, 0));
    albedo.push_back(tg::vec4f(inf, nan, 0.5f, 0));
    normal.push_back(tg::vec4f(-inf, 0.5f, nan, 0));

    auto const encoded = co_await encode(ctx, pipeline, color, albedo, normal);
    REQUIRE(encoded.size() == color.size() * 12);

    for (auto i = isize(0); i < values.size(); ++i)
    {
        auto const at = i * 12;
        // Radiance through the curve, the negative channel clamped to zero first.
        auto const expected
            = tg::vec3f(f32(pu_forward(values[i])), f32(pu_forward(values[i] * 0.5f)), f32(pu_forward(0.0)));
        for (auto c = 0; c < 3; ++c)
            CHECK(tg::abs(encoded[at + c] - expected[c]) <= 2e-6f * cc::max(1.0f, expected[c]))
                .context(cc::format("radiance {} channel {}: {} where the curve gives {}", values[i], c,
                                    encoded[at + c], expected[c]));
        // Albedo clamped to [0, 1], a normal clamped to [-1, 1] and mapped onto [0, 1], the padding zero.
        CHECK(encoded[at + 3] == 0.25f);
        CHECK(encoded[at + 4] == 1.0f);
        CHECK(encoded[at + 5] == 0.0f);
        CHECK(encoded[at + 6] == 0.75f);
        CHECK(encoded[at + 7] == 0.0f);
        CHECK(encoded[at + 8] == 0.5f);
        for (auto c = 9; c < 12; ++c)
            CHECK(encoded[at + c] == 0.0f);
    }

    // Every non-finite channel became zero, and the finite ones beside it were encoded as any other.
    auto const last = values.size() * 12;
    CHECK(encoded[last + 0] == 0.0f);
    CHECK(encoded[last + 1] == 0.0f);
    CHECK(encoded[last + 2] == 0.0f);
    CHECK(encoded[last + 3] == 0.0f);
    CHECK(encoded[last + 4] == 0.0f);
    CHECK(encoded[last + 5] == 0.5f);
    CHECK(encoded[last + 6] == 0.5f);
    CHECK(encoded[last + 7] == 0.75f);
    CHECK(encoded[last + 8] == 0.5f);
}

// The transfer curve, through both shaders that use it.
//
// Radiance spans many orders of magnitude and the curve is what lets a network see all of it.
// So what matters is that it comes back — across the whole range.
// And across both of the curve's two interior knees, where a mistranscribed constant would show up as a step rather than as a small error.
ASYNC_INVOCABLE_TEST("sr - the network's transfer curve round-trips across the HDR range",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const input_pipeline = co_await build(ctx, sr::sgl_shaders::nn_io.input_cs, "nn_io input_cs");
    auto const output_pipeline = co_await build(ctx, sr::sgl_shaders::nn_io.output_cs, "nn_io output_cs");

    // One texel per value, spanning nine orders of magnitude and sitting either side of both knees.
    auto const values = cc::vector<f32>{0.0f, 1e-7f, 1e-6f, 1e-4f, 1e-3f,  0.01f,   0.03f, 0.05f, 0.2f,
                                        0.5f, 1.0f,  4.0f,  20.0f, 100.0f, 1000.0f, 1e4f,  5e4f};
    auto const width = i32(values.size());

    auto pixels = cc::vector<tg::vec4f>();
    for (auto const v : values)
        pixels.push_back(tg::vec4f(v, v * 0.5f, v * 0.25f, 0));
    auto const zeros = cc::vector<tg::vec4f>::create_filled(size_t(width), tg::vec4f(0, 0, 0, 0));

    // The input writes nine channels and the output reads three, which in the real network is what the sixteen convolutions between them do.
    // Here there are none, so the tensor comes back to the host and its first three channels go up again — the shortest path to putting the two halves of the curve against each other.
    auto const encoded = co_await encode(ctx, input_pipeline, pixels, zeros, zeros);
    REQUIRE(encoded.size() == width * 12);

    auto compacted = cc::vector<f32>();
    for (auto i = 0; i < width; ++i)
        for (auto c = 0; c < 4; ++c)
            compacted.push_back(c < 3 ? encoded[i * 12 + c] : 0.0f);

    auto const result = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float,
         .width = width,
         .height = 1,
         .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});

    // Created in the SECOND epoch: a transient buffer does not outlive the one it was made in, and `encode` ran in the first.
    auto cmd = ctx.create_command_list();
    auto const three
        = ctx.transient.create_buffer<f32>(width * 4, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    cmd->upload.data_to_buffer(three, compacted);
    // The caller's color, whose alpha the output carries through; only the three channels the curve maps are compared.
    auto const color
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                            .width = width,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    cmd->upload.bytes_to_texture(color.raw(), cc::span<tg::vec4f const>(pixels).as_bytes());
    auto const output_layout
        = ctx.cached.acquire_binding_group_layout(sr::sgl_shaders::nn_output_images::declared_bindings(k_values),
                                                  sr::sgl_shaders::nn_output_images::declared_samplers());
    cmd->compute.bind_pipeline(*output_pipeline);
    cmd->compute.bind_group(
        0, *ctx.transient.create_binding_group(*cmd, output_layout,
                                               sr::sgl_shaders::nn_output_images{.source = three.as_readonly_buffer(),
                                                                                 .color = color.as_texture_view(),
                                                                                 .target = result.as_any_image_view()}));
    cmd->compute.set_inline_constants(sr::sgl_shaders::nn_output_constants{.width = u32(width),
                                                                           .height = 1,
                                                                           .target_width = u32(width),
                                                                           .target_height = 1,
                                                                           .write_width = u32(width),
                                                                           .write_height = 1,
                                                                           .target_offset_x = 0,
                                                                           .target_offset_y = 0,
                                                                           .read_offset_x = 0,
                                                                           .read_offset_y = 0,
                                                                           .input_scale = 1.0f}
                                          .to_block());
    cmd->compute.dispatch_threads(width, 1, 1);

    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(result.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const got = co_await readback.data();
    REQUIRE(got.size() == width);

    for (auto i = 0; i < width; ++i)
    {
        auto const expected = pixels[i];
        for (auto c = 0; c < 3; ++c)
        {
            // Relative, because the range spans nine orders of magnitude and an absolute bound would say nothing at either end of it.
            auto const e = expected[c];
            auto const d = tg::abs(got[i][c] - e);
            CHECK(d <= 1e-3f * cc::max(e, 1e-3f)).context(cc::format("value {} came back as {}", e, got[i][c]));
        }
    }

    // The curve is not the identity, or none of the above would mean anything.
    CHECK(encoded[10 * 12 + 0] != pixels[10][0]).context("the encoded value equals the input, so no transfer was applied");
}
