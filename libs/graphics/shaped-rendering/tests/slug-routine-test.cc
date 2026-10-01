#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <typed-geometry/scalar/half_float.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <memory>

using namespace cc::primitive_defines;

// The Slug routine on a live device, held pixel by pixel to the CPU reference of its pixel shader.
// The target is a half-float texture, so a pixel's red channel IS the coverage: white, premultiplied, over black.

namespace
{
constexpr auto target_size = 128;
constexpr auto target_format = sg::pixel_format::rgba16_float;

/// Pixel space as object space: x right, y down, (0, 0) at the target's top-left corner, at depth 0.5.
[[nodiscard]] tg::mat4f pixels_to_clip()
{
    auto m = tg::mat4f::identity;
    m[0, 0] = 2.0f / f32(target_size);
    m[3, 0] = -1.0f;
    m[1, 1] = -2.0f / f32(target_size);
    m[3, 1] = 1.0f;
    m[3, 2] = 0.5f; // every shape at depth one half, which the depth test is measured against
    return m;
}

[[nodiscard]] sr::slug_outline ring()
{
    auto o = sr::slug_outline();
    o.fill_rule = sr::slug_fill_rule::even_odd;
    auto const add_circle = [&](f32 r)
    {
        o.move_to(tg::pos2f(r, 0));
        for (auto i = 0; i < 8; ++i)
        {
            auto const a0 = 0.78539816f * f32(i);
            auto const a1 = 0.78539816f * f32(i + 1);
            auto const am = (a0 + a1) * 0.5f;
            auto const cr = r / tg::cos(tg::angle_f::make_from_radians(0.39269908f));
            o.quad_to(tg::pos2f(cr * tg::cos(tg::angle_f::make_from_radians(am)),
                                cr * tg::sin(tg::angle_f::make_from_radians(am))),
                      tg::pos2f(r * tg::cos(tg::angle_f::make_from_radians(a1)),
                                r * tg::sin(tg::angle_f::make_from_radians(a1))));
        }
        o.close();
    };
    add_circle(20.0f);
    add_circle(10.0f);
    return o;
}

struct slug_fixture
{
    sg::context_handle ctx;
    sr::slug_atlas atlas;
    cc::vector<sr::slug_instance> instances;
    sg::texture_2d target;
    sg::texture_2d depth;

    /// Draws every instance once, cleared to black, and reads the target back.
    [[nodiscard]] cc::shared_async<cc::pinned_data<byte const>> draw(cc::optional<f32> depth_clear = {})
    {
        auto const depth_format = depth_clear.has_value() ? sg::pixel_format::depth32_float : sg::pixel_format::undefined;
        sr::slug_routine::prewarm(*ctx, {.color = target_format, .depth = depth_format});
        (void)co_await ctx->routines.idle_completion();

        // prepare() records every copy before the scope; the first draw's barrier on the uploaded instances still splits it on vulkan.
        nx::allow_warnings("was closed and reopened around a barrier", "sg");

        auto cmd = ctx->create_command_list();
        auto const prepared = sr::slug_routine::prepare(*cmd, atlas, instances);
        {
            auto info
                = sg::rendering_info{.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}};
            if (depth_clear.has_value())
                info.depth_stencil_target = depth.as_depth_stencil_view().cleared(depth_clear.value());
            auto pass = cmd->raster.render_to(info);
            CHECK(sr::slug_routine::execute(pass, atlas, prepared, {.object_to_clip = pixels_to_clip()})
                  == sg::routine_outcome::executed);
        }
        ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->idle_completion();

        auto read = ctx->create_command_list();
        auto const future = read->download.bytes_from_texture(target.raw());
        ctx->submit_command_list(cc::move(read));
        auto const bytes = co_await future.bytes();
        co_return bytes;
    }
};

[[nodiscard]] std::unique_ptr<slug_fixture> make_fixture(sg::context_handle const& ctx)
{
    auto f = std::make_unique<slug_fixture>();
    f->ctx = ctx;
    f->target
        = ctx->persistent.create_texture_2d({.format = target_format,
                                             .width = target_size,
                                             .height = target_size,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    f->depth = ctx->persistent.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                  .width = target_size,
                                                  .height = target_size,
                                                  .usage = sg::texture_usage::depth_stencil});
    return f;
}

/// The red channel of the pixel at (x, y), which is its coverage.
[[nodiscard]] f32 red_at(cc::span<byte const> pixels, int x, int y)
{
    auto const at = (isize(y) * target_size + x) * 8;
    auto const bits = u16(u16(pixels[at]) | (u16(pixels[at + 1]) << 8));
    return f32(tg::half_float::make_from_bits(bits));
}

/// Places `outline` with its outline origin at pixel `origin`, `scale` pixels per outline unit, y up.
sr::slug_shape_ref add(slug_fixture& f, sr::slug_outline const& outline, tg::pos2f origin, f32 scale)
{
    auto const ref = f.atlas.add(sr::compile_slug_shape(outline)).value();
    f.instances.push_back(
        sr::make_slug_instance(ref, origin, tg::vec2f(scale, 0), tg::vec2f(0, -scale), tg::vec4f(1, 1, 1, 1)));
    return ref;
}

/// The reference coverage of pixel (x, y) under instance `i`, sampled at the pixel's centre.
[[nodiscard]] f32 expected_at(slug_fixture const& f, isize i, int x, int y, f32 em_scale, tg::pos2f origin, f32 scale)
{
    auto const ox = (f32(x) + 0.5f - origin[0]) / scale;
    auto const oy = (origin[1] - (f32(y) + 0.5f)) / scale;
    auto const per_pixel = em_scale / scale;
    return sr::impl::slug_reference_coverage(f.atlas, f.instances[i], tg::pos2f(ox * em_scale, oy * em_scale),
                                             tg::vec2f(per_pixel, per_pixel), false);
}
} // namespace

ASYNC_INVOCABLE_TEST("sr::slug_routine - every pixel matches the CPU reference of the pixel shader",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);

    // a rectangle with a sub-pixel offset, and an even-odd ring, side by side
    auto const rect_origin = tg::pos2f(8.3f, 60.6f);
    auto const ring_origin = tg::pos2f(90.0f, 64.0f);
    auto const s0
        = add(*f, sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 30))), rect_origin, 1.0f).em_scale;
    auto const s1 = add(*f, ring(), ring_origin, 1.5f).em_scale;

    auto const pixels = co_await f->draw();
    REQUIRE(pixels.size() == isize(target_size) * target_size * 8);

    auto worst = 0.0f;
    for (auto y = 0; y < target_size; ++y)
        for (auto x = 0; x < target_size; ++x)
        {
            // the two shapes never overlap, so a pixel's expected value is the larger of the two
            auto const expected = cc::max(expected_at(*f, 0, x, y, s0, rect_origin, 1.0f),
                                          expected_at(*f, 1, x, y, s1, ring_origin, 1.5f));
            worst = cc::max(worst, tg::abs(red_at(pixels, x, y) - expected));
        }
    // half-float output and per-target rounding, nothing more
    CHECK(worst < 0.01f).dump("worst", worst);

    // and the picture is the one meant: inside the rectangle, inside the ring's band, and in its hole
    CHECK(red_at(pixels, 28, 45) > 0.99f);
    CHECK(red_at(pixels, 90 + 22, 64) > 0.99f);
    CHECK(red_at(pixels, 90, 64) < 0.01f);
}

ASYNC_INVOCABLE_TEST("sr::slug_routine - a scope with depth tests against it and writes nothing",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);
    (void)add(*f, sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 40))), tg::pos2f(40, 80), 1.0f);

    // the shape sits at depth one half, which a depth cleared to 1 lets through and one cleared to a quarter does not
    auto const passed = co_await f->draw(1.0f);
    CHECK(red_at(passed, 60, 60) > 0.99f);

    auto const blocked = co_await f->draw(0.25f);
    CHECK(red_at(blocked, 60, 60) < 0.01f);
}
