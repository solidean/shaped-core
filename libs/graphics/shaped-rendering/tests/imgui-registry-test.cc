#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/imgui_draw_math.hh>

// The arithmetic behind the imgui renderer, checked against hand-computed numbers.
// None of this needs a device, so it runs on every platform — which matters,
// because these are the parts most likely to be subtly wrong (an off-by-one scissor clamp, a row-pitch mistake) and the hardest to spot in a rendered image.

using namespace sr;

namespace
{
// nexus has no approx matcher; these are exact-ish affine results, so a tight epsilon is right.
[[nodiscard]] bool close(float a, float b)
{
    return a - b < 1e-5f && b - a < 1e-5f;
}
} // namespace

TEST("sr::impl - ortho constants map the display corners to clip space")
{
    // A plain 800x600 display at the origin: top-left -> (-1, +1), bottom-right -> (+1, -1).
    auto const c = impl::compute_ortho_constants(tg::pos2f(0.0f, 0.0f), tg::vec2f(800.0f, 600.0f));

    auto const project
        = [&](float x, float y) { return tg::vec2f(x * c.scale[0] + c.translate[0], y * c.scale[1] + c.translate[1]); };

    auto const top_left = project(0.0f, 0.0f);
    CHECK(close(top_left[0], -1.0f));
    CHECK(close(top_left[1], 1.0f)); // y flips: imgui is y-down, clip space is y-up

    auto const bottom_right = project(800.0f, 600.0f);
    CHECK(close(bottom_right[0], 1.0f));
    CHECK(close(bottom_right[1], -1.0f));

    auto const center = project(400.0f, 300.0f);
    CHECK(close(center[0], 0.0f));
    CHECK(close(center[1], 0.0f));
}

TEST("sr::impl - ortho constants honour a non-zero display pos")
{
    // Under multi-viewport the display origin is not (0,0); the corners must still map to the clip corners.
    auto const c = impl::compute_ortho_constants(tg::pos2f(100.0f, 50.0f), tg::vec2f(200.0f, 100.0f));

    auto const project
        = [&](float x, float y) { return tg::vec2f(x * c.scale[0] + c.translate[0], y * c.scale[1] + c.translate[1]); };

    auto const top_left = project(100.0f, 50.0f);
    CHECK(close(top_left[0], -1.0f));
    CHECK(close(top_left[1], 1.0f));

    auto const bottom_right = project(300.0f, 150.0f);
    CHECK(close(bottom_right[0], 1.0f));
    CHECK(close(bottom_right[1], -1.0f));
}

TEST("sr::impl - scissor passes a rect that is fully inside")
{
    auto const r = impl::compute_scissor(tg::aabb2f(tg::pos2f(10, 20), tg::pos2f(110, 220)), tg::pos2f(0, 0),
                                         tg::vec2f(1, 1), tg::vec2i(800, 600));

    REQUIRE(r.has_value());
    CHECK(r.value().min == tg::pos2i(10, 20));
    CHECK(r.value().max == tg::pos2i(110, 220));
}

TEST("sr::impl - scissor clamps a rect straddling the top-left")
{
    // imgui emits negative clip coordinates for a window dragged off-screen; both tier-1 backends reject a negative scissor outright, so it must be clamped rather than passed through.
    auto const r = impl::compute_scissor(tg::aabb2f(tg::pos2f(-50, -30), tg::pos2f(100, 200)), tg::pos2f(0, 0),
                                         tg::vec2f(1, 1), tg::vec2i(800, 600));

    REQUIRE(r.has_value());
    CHECK(r.value().min == tg::pos2i(0, 0));
    CHECK(r.value().max == tg::pos2i(100, 200));
}

TEST("sr::impl - scissor clamps a rect straddling the bottom-right")
{
    auto const r = impl::compute_scissor(tg::aabb2f(tg::pos2f(700, 500), tg::pos2f(900, 700)), tg::pos2f(0, 0),
                                         tg::vec2f(1, 1), tg::vec2i(800, 600));

    REQUIRE(r.has_value());
    CHECK(r.value().min == tg::pos2i(700, 500));
    CHECK(r.value().max == tg::pos2i(800, 600));
}

TEST("sr::impl - scissor rejects a fully off-target rect")
{
    // Degenerate after clamping.
    // Submitting this as a zero-area scissor is what a naive clamp does, and it is a validation error rather than a harmless no-op.
    CHECK(!impl::compute_scissor(tg::aabb2f(tg::pos2f(900, 700), tg::pos2f(1000, 800)), tg::pos2f(0, 0),
                                 tg::vec2f(1, 1), tg::vec2i(800, 600))
               .has_value());
    CHECK(!impl::compute_scissor(tg::aabb2f(tg::pos2f(-200, -200), tg::pos2f(-100, -100)), tg::pos2f(0, 0),
                                 tg::vec2f(1, 1), tg::vec2i(800, 600))
               .has_value());

    // Empty in one axis only still has no area.
    CHECK(!impl::compute_scissor(tg::aabb2f(tg::pos2f(10, 20), tg::pos2f(10, 220)), tg::pos2f(0, 0), tg::vec2f(1, 1),
                                 tg::vec2i(800, 600))
               .has_value());
}

TEST("sr::impl - scissor translates by display pos and scales to framebuffer pixels")
{
    // display_pos shifts the rect out of display space; framebuffer_scale then converts to real pixels (2x on a retina-style display).
    // Both apply, in that order.
    auto const r = impl::compute_scissor(tg::aabb2f(tg::pos2f(110, 70), tg::pos2f(210, 170)), tg::pos2f(100, 50),
                                         tg::vec2f(2, 2), tg::vec2i(800, 600));

    REQUIRE(r.has_value());
    CHECK(r.value().min == tg::pos2i(20, 40));   // (110-100)*2, (70-50)*2
    CHECK(r.value().max == tg::pos2i(220, 240)); // (210-100)*2, (170-50)*2
}

TEST("sr::impl - packing a sub-rect tightens the row pitch")
{
    // A 4-wide, 3-tall RGBA image where each byte encodes its own linear index, so a mispacked row or a wrong stride shows up as a concrete wrong number rather than a plausible-looking blur.
    constexpr auto width = 4;
    constexpr auto height = 3;
    constexpr auto bpp = 4;

    auto image = cc::vector<cc::byte>::create_defaulted(width * height * bpp);
    for (auto i = 0; i < image.size(); ++i)
        image[i] = cc::byte(i);

    // Take the 2x2 rect at (1,1): source rows start at byte (1*4 + 1)*4 = 20 and (2*4 + 1)*4 = 36.
    auto const packed = impl::pack_texture_rect(image.data(), width * bpp, bpp, tg::pos2i(1, 1), tg::vec2i(2, 2));

    REQUIRE(packed.size() == 2 * 2 * bpp);
    for (auto i = 0; i < 8; ++i)
        CHECK(packed[i] == cc::byte(20 + i));
    for (auto i = 0; i < 8; ++i)
        CHECK(packed[8 + i] == cc::byte(36 + i));
}

TEST("sr::impl - packing a full-width rect is a straight copy")
{
    constexpr auto width = 3;
    constexpr auto height = 2;
    constexpr auto bpp = 1;

    auto image = cc::vector<cc::byte>::create_defaulted(width * height * bpp);
    for (auto i = 0; i < image.size(); ++i)
        image[i] = cc::byte(i);

    auto const packed
        = impl::pack_texture_rect(image.data(), width * bpp, bpp, tg::pos2i(0, 0), tg::vec2i(width, height));

    REQUIRE(packed.size() == image.size());
    for (auto i = 0; i < packed.size(); ++i)
        CHECK(packed[i] == cc::byte(i));
}

TEST("sr::impl - each pack allocates its own buffer")
{
    // ctx.upload is fire-and-forget and holds the pin until the copy runs, so two packs must not alias — reusing one scratch buffer would let a second repack overwrite bytes a pending copy still reads.
    constexpr auto width = 2;
    constexpr auto bpp = 1;
    auto const image = cc::vector<cc::byte>{cc::byte(1), cc::byte(2), cc::byte(3), cc::byte(4)};

    auto const first = impl::pack_texture_rect(image.data(), width * bpp, bpp, tg::pos2i(0, 0), tg::vec2i(2, 1));
    auto const second = impl::pack_texture_rect(image.data(), width * bpp, bpp, tg::pos2i(0, 1), tg::vec2i(2, 1));

    CHECK(first.data() != second.data());
    CHECK(first[0] == cc::byte(1));
    CHECK(second[0] == cc::byte(3));
}
