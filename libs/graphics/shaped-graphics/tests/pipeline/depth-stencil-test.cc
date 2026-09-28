#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"
#include "rects.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

namespace
{
/// `rects.depth_tested` with `edit` applied last.
cc::shared_async<sg::raster_pipeline_handle> depth_pipeline(sg::context& ctx,
                                                            cc::unique_function<void(sg::depth_stencil_state&)> edit)
{
    co_return co_await ctx.cached.acquire_raster_pipeline(
        shaders::rects.depth_tested, {},
        [edit = cc::move(edit)](sg::raster_pipeline_description& d) mutable { edit(d.depth_stencil); });
}

/// `rects.stencil_tested` with `edit` applied last.
cc::shared_async<sg::raster_pipeline_handle> stencil_pipeline(sg::context& ctx,
                                                              cc::unique_function<void(sg::raster_pipeline_description&)> edit)
{
    co_return co_await ctx.cached.acquire_raster_pipeline(shaders::rects.stencil_tested, {}, cc::move(edit));
}

/// What `op` leaves of `value`, with `reference` for `replace`.
u8 stencil_result(sg::stencil_op op, u8 value, u8 reference)
{
    switch (op)
    {
    case sg::stencil_op::keep:
        return value;
    case sg::stencil_op::zero:
        return 0;
    case sg::stencil_op::replace:
        return reference;
    case sg::stencil_op::increment_clamp:
        return value == 255 ? 255 : u8(value + 1);
    case sg::stencil_op::decrement_clamp:
        return value == 0 ? 0 : u8(value - 1);
    case sg::stencil_op::invert:
        return u8(~value);
    case sg::stencil_op::increment_wrap:
        return u8(value + 1);
    case sg::stencil_op::decrement_wrap:
        return u8(value - 1);
    }
    return value;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - each depth compare_op passes its own rows of a depth staircase",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // Column c is ops[c]: its three rows hold depth 0.25, 0.5 and 0.75, and the op's rect is drawn over them at 0.5.
    // Each op then passes a different set of rows, so replacing any op by another fails a pixel.
    struct expectation
    {
        sg::compare_op op;
        bool passes[3]; // per row, against 0.25, 0.5 and 0.75
    };
    constexpr expectation ops[] = {
        {sg::compare_op::never, {false, false, false}},       {sg::compare_op::less, {false, false, true}},
        {sg::compare_op::equal, {false, true, false}},        {sg::compare_op::less_equal, {false, true, true}},
        {sg::compare_op::greater, {true, false, false}},      {sg::compare_op::not_equal, {true, false, true}},
        {sg::compare_op::greater_equal, {true, true, false}}, {sg::compare_op::always, {true, true, true}},
    };
    constexpr int width = 8;
    constexpr int height = 3;
    constexpr float steps[] = {0.25f, 0.5f, 0.75f};

    auto const black = tg::vec4f(0, 0, 0, 1);
    auto const white = tg::vec4f(1, 1, 1, 1);
    auto rects = cc::vector<sg_test::rect>();
    for (auto r = 0; r < height; ++r)
        rects.push_back(sg_test::rect_at(0, r, width, r + 1, width, height, steps[r], black));
    for (auto c = 0; c < width; ++c)
        rects.push_back(sg_test::rect_at(c, 0, c + 1, height, width, height, 0.5f, white));
    auto const batch = sg_test::rect_batch(*ctx, rects);

    auto const fill
        = co_await depth_pipeline(*ctx, [](sg::depth_stencil_state& ds) { ds.depth_compare = sg::compare_op::always; });
    auto tests = cc::vector<sg::raster_pipeline_handle>();
    for (auto const& e : ops)
        tests.push_back(co_await depth_pipeline(*ctx,
                                                [op = e.op](sg::depth_stencil_state& ds)
                                                {
                                                    ds.depth_compare = op;
                                                    ds.depth_write = false;
                                                }));

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = width,
                                                          .height = height,
                                                          .colors = {sg::pixel_format::rgba8_unorm},
                                                          .depth_stencil = sg::pixel_format::depth32_float,
                                                          .target_set = shaders::rect_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*fill);
                                                             batch.draw(scope, 0, height);
                                                             for (auto c = 0; c < width; ++c)
                                                             {
                                                                 scope.bind_pipeline(*tests[c]);
                                                                 batch.draw(scope, height + c);
                                                             }
                                                         });

    for (auto c = 0; c < width; ++c)
        for (auto r = 0; r < height; ++r)
            CHECK(pixels[0].rgba8(c, r)[0] == (ops[c].passes[r] ? 255 : 0))
                .context(cc::format("compare_op #{} against depth {}", c, steps[r]));
}

ASYNC_INVOCABLE_TEST("sg - each stencil_op leaves its own value, in the slot and on the face it is set for",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A column per (op, start value), a row per (face, slot): 8 × 3 by 2 × 3 pixels.
    // Each pixel is seeded to its start, has one op applied, and is then probed for the value the op should leave.
    // From 5 every op but the two increments and the two decrements leaves a different value.
    // 255 splits the increments and 0 the decrements, since only there does clamping differ from wrapping.
    constexpr sg::stencil_op ops[] = {
        sg::stencil_op::keep,
        sg::stencil_op::zero,
        sg::stencil_op::replace,
        sg::stencil_op::increment_clamp,
        sg::stencil_op::decrement_clamp,
        sg::stencil_op::invert,
        sg::stencil_op::increment_wrap,
        sg::stencil_op::decrement_wrap,
    };
    constexpr u8 starts[] = {5, 255, 0};
    constexpr u8 reference = 9;
    enum class slot
    {
        fail,       // the stencil test fails
        depth_fail, // the stencil test passes and the depth test fails
        pass,       // both pass
    };
    constexpr slot slots[] = {slot::fail, slot::depth_fail, slot::pass};
    constexpr int width = 8 * 3;
    constexpr int height = 2 * 3;

    auto rects = cc::vector<sg_test::rect>();
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            rects.push_back(sg_test::rect_at(x, y, x + 1, y + 1, width, height, 0.5f, tg::vec4f(0, 0, 0, 1)));
    // The probe's rects, drawn white where the value is the expected one.
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            rects.push_back(sg_test::rect_at(x, y, x + 1, y + 1, width, height, 0.5f, tg::vec4f(1, 1, 1, 1)));
    auto const batch = sg_test::rect_batch(*ctx, rects);
    auto const pixel = [](int x, int y) { return y * width + x; };

    auto const both_faces = [](sg::stencil_face face)
    {
        return [face](sg::raster_pipeline_description& d)
        {
            d.depth_stencil.stencil_front = face;
            d.depth_stencil.stencil_back = face;
        };
    };
    auto const seed = co_await stencil_pipeline(
        *ctx, both_faces({.pass = sg::stencil_op::replace, .compare = sg::compare_op::always}));
    auto const probe = co_await stencil_pipeline(*ctx, both_faces({.compare = sg::compare_op::equal}));

    // The op under test in its slot, and `replace` in the other two, so an op applied from the wrong slot shows.
    // Testing `replace` itself, the other slots `zero` instead.
    auto const tested_face = [](sg::stencil_op op, slot s)
    {
        auto const other = op == sg::stencil_op::replace ? sg::stencil_op::zero : sg::stencil_op::replace;
        return sg::stencil_face{
            .fail = s == slot::fail ? op : other,
            .depth_fail = s == slot::depth_fail ? op : other,
            .pass = s == slot::pass ? op : other,
            .compare = s == slot::fail ? sg::compare_op::never : sg::compare_op::always,
        };
    };
    auto appliers = cc::vector<sg::raster_pipeline_handle>();
    for (auto const back : {false, true})
        for (auto const s : slots)
            for (auto const op : ops)
                appliers.push_back(co_await stencil_pipeline(
                    *ctx,
                    [&, back, s, op](sg::raster_pipeline_description& d)
                    {
                        // The rects wind counter-clockwise, so declaring clockwise the front makes them back faces.
                        // The face not under test turns every value to 0, which no expectation below leaves alone.
                        auto const unused = sg::stencil_face{.fail = sg::stencil_op::zero,
                                                             .depth_fail = sg::stencil_op::zero,
                                                             .pass = sg::stencil_op::zero};
                        d.rasterization.front = back ? sg::front_face::clockwise : sg::front_face::counter_clockwise;
                        d.depth_stencil.stencil_front = back ? unused : tested_face(op, s);
                        d.depth_stencil.stencil_back = back ? tested_face(op, s) : unused;
                        if (s == slot::depth_fail)
                        {
                            d.depth_stencil.depth_test = true;
                            d.depth_stencil.depth_compare = sg::compare_op::never;
                        }
                    }));

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = width,
         .height = height,
         .colors = {sg::pixel_format::rgba8_unorm},
         .depth_stencil = sg::pixel_format::depth32_float_stencil8,
         .target_set = shaders::rect_target::name},
        [&](sg::rendering_scope& scope)
        {
            for (auto y = 0; y < height; ++y)
                for (auto x = 0; x < width; ++x)
                {
                    scope.bind_pipeline(*seed);
                    scope.set_stencil_reference(starts[x % 3]);
                    batch.draw(scope, pixel(x, y));

                    scope.bind_pipeline(*appliers[(y / 3) * 3 * 8 + (y % 3) * 8 + x / 3]);
                    scope.set_stencil_reference(reference);
                    batch.draw(scope, pixel(x, y));

                    scope.bind_pipeline(*probe);
                    scope.set_stencil_reference(u32(stencil_result(ops[x / 3], starts[x % 3], reference)));
                    batch.draw(scope, width * height + pixel(x, y));
                }
        });

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            CHECK(pixels[0].rgba8(x, y)[0] == 255)
                .context(cc::format("stencil op #{} from {} in slot #{} of the {} face", x / 3, starts[x % 3], y % 3,
                                    y < 3 ? "front" : "back"));
}

ASYNC_INVOCABLE_TEST("sg - the stencil read mask narrows the comparison and the write mask narrows the write",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // Every pixel starts at 0xF0, then one pipeline per column:
    //  0: `replace` with 0x05 through write mask 0x0F, which keeps the high nibble: 0xF5;
    //  1: `equal` against 0x00 through read mask 0x0F, which passes, and `pass = zero` writes 0x00;
    //  2: the same comparison through read mask 0xFF, which fails, and `fail = keep` leaves 0xF0.
    // A mask a backend ignored leaves 0x05, 0xF0 or 0x00 instead.
    constexpr int width = 3;
    constexpr u8 expected[] = {0xF5, 0x00, 0xF0};

    auto rects = cc::vector<sg_test::rect>();
    for (auto x = 0; x < width; ++x)
        rects.push_back(sg_test::rect_at(x, 0, x + 1, 1, width, 1, 0.5f, tg::vec4f(0, 0, 0, 1)));
    for (auto x = 0; x < width; ++x)
        rects.push_back(sg_test::rect_at(x, 0, x + 1, 1, width, 1, 0.5f, tg::vec4f(1, 1, 1, 1)));
    auto const batch = sg_test::rect_batch(*ctx, rects);

    auto const faces = [](sg::stencil_face face, u8 read_mask, u8 write_mask)
    {
        return [=](sg::raster_pipeline_description& d)
        {
            d.depth_stencil.stencil_front = face;
            d.depth_stencil.stencil_back = face;
            d.depth_stencil.stencil_read_mask = read_mask;
            d.depth_stencil.stencil_write_mask = write_mask;
        };
    };
    auto const seed = co_await stencil_pipeline(
        *ctx, faces({.pass = sg::stencil_op::replace, .compare = sg::compare_op::always}, 0xFF, 0xFF));
    auto const probe = co_await stencil_pipeline(*ctx, faces({.compare = sg::compare_op::equal}, 0xFF, 0xFF));
    auto const masked_equal
        = sg::stencil_face{.fail = sg::stencil_op::keep, .pass = sg::stencil_op::zero, .compare = sg::compare_op::equal};
    sg::raster_pipeline_handle const columns[] = {
        co_await stencil_pipeline(
            *ctx, faces({.pass = sg::stencil_op::replace, .compare = sg::compare_op::always}, 0xFF, 0x0F)),
        co_await stencil_pipeline(*ctx, faces(masked_equal, 0x0F, 0xFF)),
        co_await stencil_pipeline(*ctx, faces(masked_equal, 0xFF, 0xFF)),
    };
    u32 const references[] = {0x05, 0x00, 0x00};

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = width,
                                                          .height = 1,
                                                          .colors = {sg::pixel_format::rgba8_unorm},
                                                          .depth_stencil = sg::pixel_format::depth32_float_stencil8,
                                                          .target_set = shaders::rect_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             for (auto x = 0; x < width; ++x)
                                                             {
                                                                 scope.bind_pipeline(*seed);
                                                                 scope.set_stencil_reference(0xF0);
                                                                 batch.draw(scope, x);
                                                                 scope.bind_pipeline(*columns[x]);
                                                                 scope.set_stencil_reference(references[x]);
                                                                 batch.draw(scope, x);
                                                                 scope.bind_pipeline(*probe);
                                                                 scope.set_stencil_reference(expected[x]);
                                                                 batch.draw(scope, width + x);
                                                             }
                                                         });

    for (auto x = 0; x < width; ++x)
        CHECK(pixels[0].rgba8(x, 0)[0] == 255).context(cc::format("column {}", x));
}
