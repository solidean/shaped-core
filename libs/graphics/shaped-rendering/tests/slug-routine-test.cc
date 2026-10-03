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

    /// Draws `count` instances of a persistent buffer from `first` on, cleared to black, and reads the target back.
    [[nodiscard]] cc::shared_async<cc::pinned_data<byte const>> draw_range(sg::buffer<sr::slug_instance> const& buffer,
                                                                           isize first,
                                                                           isize count)
    {
        sr::slug_routine::prewarm(*ctx, {.color = target_format, .depth = sg::pixel_format::undefined});
        (void)co_await ctx->routines.idle_completion();
        nx::allow_warnings("was closed and reopened around a barrier", "sg");

        auto cmd = ctx->create_command_list();
        atlas.prepare(*cmd);
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
            CHECK(sr::slug_routine::execute(pass, atlas, buffer, first, count, {.object_to_clip = pixels_to_clip()})
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

    /// Draws a job over the atlas's records, cleared to black, and reads the target back.
    /// `probe_depth`, when given, is uploaded into an r32_float texture one row tall that the frames probe.
    [[nodiscard]] cc::shared_async<cc::pinned_data<byte const>> draw_job(cc::vector<sr::slug_frame> frames,
                                                                         cc::vector<sr::slug_quad> quads,
                                                                         cc::vector<f32> probe_depth = {})
    {
        sr::slug_routine::prewarm(*ctx, {.color = target_format, .depth = sg::pixel_format::undefined});
        (void)co_await ctx->routines.idle_completion();
        nx::allow_warnings("was closed and reopened around a barrier", "sg");

        auto cmd = ctx->create_command_list();
        auto probe = sg::texture_2d();
        if (!probe_depth.empty())
        {
            auto const width = int(probe_depth.size());
            probe = ctx->transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                                      .width = width,
                                                      .height = 1,
                                                      .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
            cmd->upload.bytes_to_texture(
                probe.raw(), cc::span<f32 const>(probe_depth).as_bytes(), {},
                sg::texture_region{.offset = tg::pos3i(0, 0, 0), .size = tg::vec3i(width, 1, 1)});
        }
        auto const job = sr::slug_routine::prepare_job(*cmd, atlas, frames, quads);
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
            CHECK(sr::slug_routine::execute(pass, atlas, job, {.object_to_clip = pixels_to_clip(), .probe_depth = probe})
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

/// Channel `c` of the pixel at (x, y).
[[nodiscard]] f32 channel_at(cc::span<byte const> pixels, int x, int y, int c)
{
    auto const at = (isize(y) * target_size + x) * 8 + c * 2;
    auto const bits = u16(u16(pixels[at]) | (u16(pixels[at + 1]) << 8));
    return f32(tg::half_float::make_from_bits(bits));
}

/// The red channel of the pixel at (x, y), which is its coverage.
[[nodiscard]] f32 red_at(cc::span<byte const> pixels, int x, int y)
{
    return channel_at(pixels, x, y, 0);
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
[[nodiscard]] f32
expected_at(slug_fixture const& f, isize i, int x, int y, sr::slug_shape_ref const& ref, tg::pos2f origin, f32 scale)
{
    auto const ox = (f32(x) + 0.5f - origin[0]) / scale - ref.stored_origin[0];
    auto const oy = (origin[1] - (f32(y) + 0.5f)) / scale - ref.stored_origin[1];
    auto const per_pixel = ref.em_scale / scale;
    return sr::impl::slug_reference_coverage(f.atlas, f.instances[i], tg::pos2f(ox * ref.em_scale, oy * ref.em_scale),
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
    auto const r0
        = add(*f, sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 30))), rect_origin, 1.0f);
    auto const r1 = add(*f, ring(), ring_origin, 1.5f);

    auto const pixels = co_await f->draw();
    REQUIRE(pixels.size() == isize(target_size) * target_size * 8);

    auto worst = 0.0f;
    for (auto y = 0; y < target_size; ++y)
        for (auto x = 0; x < target_size; ++x)
        {
            // the two shapes never overlap, so a pixel's expected value is the larger of the two
            auto const expected = cc::max(expected_at(*f, 0, x, y, r0, rect_origin, 1.0f),
                                          expected_at(*f, 1, x, y, r1, ring_origin, 1.5f));
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

ASYNC_INVOCABLE_TEST("sr::slug_routine - a job draws what the same shapes drawn as instances draw",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);

    // the classic form: a rectangle and the ring, each placed in pixels by its own instance
    auto const rect_origin = tg::pos2f(8.3f, 60.6f);
    auto const ring_origin = tg::pos2f(90.0f, 64.0f);
    auto const rect
        = add(*f, sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 30))), rect_origin, 1.0f);
    auto const r = add(*f, ring(), ring_origin, 1.5f);
    auto const classic = co_await f->draw();

    // the job form: each shape kept once as a record in a y-up drawing plane, and placed by a frame that carries the
    // pixel origin and the scale, so a frame's axes stretch the record exactly as the instance's axes did
    auto const unit = [](sr::slug_shape_ref const& ref)
    { return sr::make_slug_instance(ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, -1), tg::vec4f(1, 1, 1, 1)); };
    sr::slug_instance const records[] = {unit(rect), unit(r)};
    auto const first = f->atlas.add_records(records).value();
    auto const frames = cc::vector<sr::slug_frame>{{.at = tg::pos3f(rect_origin[0], rect_origin[1], 0)},
                                                   {.at = tg::pos3f(ring_origin[0], ring_origin[1], 0),
                                                    .x_axis = tg::vec3f(1.5f, 0, 0),
                                                    .y_axis = tg::vec3f(0, 1.5f, 0)}};
    auto const quads = cc::vector<sr::slug_quad>{{.record = first, .frame = 0}, {.record = first + 1, .frame = 1}};
    auto const job = co_await f->draw_job(frames, quads);

    auto worst = 0.0f;
    for (auto y = 0; y < target_size; ++y)
        for (auto x = 0; x < target_size; ++x)
            worst = cc::max(worst, tg::abs(red_at(job, x, y) - red_at(classic, x, y)));
    CHECK(worst < 0.01f).dump("worst", worst);
    CHECK(red_at(job, 28, 45) > 0.99f);
    CHECK(red_at(job, 90, 64) < 0.01f);
}

ASYNC_INVOCABLE_TEST("sr::slug_routine - a job places one record under many frames, each tinting it",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);
    auto const ref
        = f->atlas
              .add(sr::compile_slug_shape(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1)))))
              .value();
    auto const record
        = sr::make_slug_instance(ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1));
    auto const first = f->atlas.add_records(cc::span<sr::slug_instance const>(&record, 1)).value();

    // one unit square, stretched into a 20 x 20 red square on the left and a 40 x 10 green bar on the right
    auto const frames = cc::vector<sr::slug_frame>{{.at = tg::pos3f(10, 10, 0),
                                                    .x_axis = tg::vec3f(20, 0, 0),
                                                    .y_axis = tg::vec3f(0, 20, 0),
                                                    .tint = sr::pack_rgba8(tg::vec4f(1, 0, 0, 1))},
                                                   {.at = tg::pos3f(70, 10, 0),
                                                    .x_axis = tg::vec3f(40, 0, 0),
                                                    .y_axis = tg::vec3f(0, 10, 0),
                                                    .tint = sr::pack_rgba8(tg::vec4f(0, 1, 0, 1))}};
    auto const quads = cc::vector<sr::slug_quad>{{.record = first, .frame = 0}, {.record = first, .frame = 1}};
    auto const pixels = co_await f->draw_job(frames, quads);

    CHECK(channel_at(pixels, 20, 20, 0) > 0.99f);
    CHECK(channel_at(pixels, 20, 20, 1) < 0.01f);
    CHECK(channel_at(pixels, 100, 15, 1) > 0.99f);
    CHECK(channel_at(pixels, 100, 15, 0) < 0.01f);
    // the bar is ten pixels tall, so its frame's y axis stretched the square as much as the x axis did
    CHECK(channel_at(pixels, 100, 25, 1) < 0.01f);
    CHECK(channel_at(pixels, 50, 15, 3) < 0.01f);
}

ASYNC_INVOCABLE_TEST("sr::slug_routine - a frame's probe shows or hides it by the depth found there",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);
    auto const ref
        = f->atlas
              .add(sr::compile_slug_shape(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(1, 1)))))
              .value();
    auto const record
        = sr::make_slug_instance(ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1));
    auto const first = f->atlas.add_records(cc::span<sr::slug_instance const>(&record, 1)).value();

    // Five 16-pixel squares in a row, each probing one of two depth texels.
    // The left texel holds something at depth one half, twice the near distance away; the right one holds nothing.
    auto const square = [](f32 x, sr::slug_visibility visibility, f32 probe_x, f32 depth)
    {
        return sr::slug_frame{.at = tg::pos3f(x, 56, 0),
                              .x_axis = tg::vec3f(16, 0, 0),
                              .y_axis = tg::vec3f(0, 16, 0),
                              .visibility = visibility,
                              .probe = tg::vec2f(probe_x, 0.5f),
                              .probe_depth = depth};
    };
    auto const frames = cc::vector<sr::slug_frame>{
        square(4, sr::slug_visibility::if_visible, 0.25f, 0.8f),  // behind what the left texel holds: hidden
        square(28, sr::slug_visibility::if_hidden, 0.25f, 0.8f),  // so its hidden look draws
        square(52, sr::slug_visibility::if_visible, 0.75f, 0.8f), // nothing on the right: visible
        square(76, sr::slug_visibility::if_hidden, 0.75f, 0.8f),
        square(100, sr::slug_visibility::if_visible, 0.25f, 0.5f), // on the surface itself, within the tolerance
    };
    auto quads = cc::vector<sr::slug_quad>();
    for (auto i = u32(0); i < u32(frames.size()); ++i)
        quads.push_back({.record = first, .frame = i});

    auto const probed = co_await f->draw_job(frames, quads, {0.5f, 1.0f});
    CHECK(red_at(probed, 12, 64) < 0.01f);
    CHECK(red_at(probed, 36, 64) > 0.99f);
    CHECK(red_at(probed, 60, 64) > 0.99f);
    CHECK(red_at(probed, 84, 64) < 0.01f);
    CHECK(red_at(probed, 108, 64) > 0.99f);

    // with no depth to probe, every probe counts as visible
    auto const unprobed = co_await f->draw_job(frames, quads);
    CHECK(red_at(unprobed, 12, 64) > 0.99f);
    CHECK(red_at(unprobed, 36, 64) < 0.01f);
    CHECK(red_at(unprobed, 84, 64) < 0.01f);
}

ASYNC_INVOCABLE_TEST("sr::slug_routine - a retained draw covers its sub-range of a persistent buffer and nothing else",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);
    auto const square = sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(20, 20)));
    for (auto const x : {10.0f, 50.0f, 90.0f})
        (void)add(*f, square, tg::pos2f(x, 80), 1.0f);
    auto const buffer = ctx->persistent.create_buffer_from_data(f->instances, sg::buffer_usage::vertex_buffer);

    // the middle square alone: the ones before and after it in the buffer stay black
    auto const pixels = co_await f->draw_range(buffer, 1, 1);
    CHECK(red_at(pixels, 60, 70) > 0.99f);
    CHECK(red_at(pixels, 20, 70) < 0.01f);
    CHECK(red_at(pixels, 100, 70) < 0.01f);
}

ASYNC_INVOCABLE_TEST("sr::slug_routine - a draw covers the whole target whatever viewport the scope was left with",
                     (sg::context_handle const& ctx),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx != nullptr);
    auto const f = make_fixture(ctx);
    (void)add(*f, sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(20, 20))), tg::pos2f(90, 110), 1.0f);

    sr::slug_routine::prewarm(*ctx, {.color = target_format, .depth = sg::pixel_format::undefined});
    (void)co_await ctx->routines.idle_completion();
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    auto cmd = ctx->create_command_list();
    auto const prepared = sr::slug_routine::prepare(*cmd, f->atlas, f->instances);
    {
        auto pass = cmd->raster.render_to(
            {.color_targets = {f->target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
        // as a layout draw before it leaves them: narrowed to the top-left quarter
        pass.set_viewport({.offset = tg::pos2f(0, 0), .size = tg::vec2f(64, 64)});
        pass.set_scissor(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(64, 64)));
        CHECK(sr::slug_routine::execute(pass, f->atlas, prepared, {.object_to_clip = pixels_to_clip()})
              == sg::routine_outcome::executed);
    }
    ctx->submit_command_list(cc::move(cmd));
    ctx->advance_epoch();
    co_await ctx->idle_completion();

    auto read = ctx->create_command_list();
    auto const future = read->download.bytes_from_texture(f->target.raw());
    ctx->submit_command_list(cc::move(read));
    auto const pixels = co_await future.bytes();

    // the square sits in the bottom-right quarter, where it lands, and nothing of it in the top-left one
    CHECK(red_at(pixels, 100, 100) > 0.99f);
    CHECK(red_at(pixels, 30, 30) < 0.01f);
}
