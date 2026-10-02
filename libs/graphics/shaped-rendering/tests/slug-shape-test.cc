#include <clean-core/container/vector.hh>
#include <font/font_builder.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-rendering/slug_traced.hh>
#include <typed-geometry/linalg/pos_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// Slug's CPU half: compiling outlines, placing them, and the coverage they produce, read through the CPU reference of the pixel shader.
// No device: the atlas holds its own copy of everything it places.

namespace
{
/// A shape placed in its own atlas, with the instance that draws it at one outline unit per object unit.
struct placed
{
    sr::slug_atlas atlas;
    sr::slug_shape_ref ref;
    sr::slug_instance instance;
};

[[nodiscard]] placed place(sr::slug_outline const& outline)
{
    auto p = placed();
    p.ref = p.atlas.add(sr::compile_slug_shape(outline)).value();
    p.instance = sr::make_slug_instance(p.ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1));
    return p;
}

/// Outline point `at` in `ref`'s stored space.
[[nodiscard]] tg::pos2f stored(sr::slug_shape_ref const& ref, tg::pos2f at)
{
    return tg::pos2f((at[0] - ref.stored_origin[0]) * ref.em_scale, (at[1] - ref.stored_origin[1]) * ref.em_scale);
}

/// The coverage at outline point `at`, sampled with pixels `pixel` outline units wide.
[[nodiscard]] f32 coverage(placed const& p, tg::pos2f at, f32 pixel = 1.0f)
{
    auto const s = p.ref.em_scale;
    return sr::impl::slug_reference_coverage(p.atlas, p.instance, stored(p.ref, at), tg::vec2f(pixel * s, pixel * s),
                                             false);
}

/// A circle of four quadratic arcs per quadrant pair, close enough to round for a coverage estimate.
[[nodiscard]] sr::slug_outline circle(tg::pos2f c, f32 r, int segments, bool clockwise = false)
{
    auto o = sr::slug_outline();
    auto const at = [&](f32 a)
    {
        return tg::pos2f(c[0] + r * tg::cos(tg::angle_f::make_from_radians(a)),
                         c[1] + r * tg::sin(tg::angle_f::make_from_radians(a)));
    };
    auto const step = (clockwise ? -1.0f : 1.0f) * 6.2831853f / f32(segments);
    // a quadratic through the arc's end points whose control point sits where the end tangents meet
    auto const control_scale = r / tg::cos(tg::angle_f::make_from_radians(step * 0.5f));
    o.move_to(at(0.0f));
    for (auto i = 0; i < segments; ++i)
    {
        auto const mid = step * (f32(i) + 0.5f);
        auto const control = tg::pos2f(c[0] + control_scale * tg::cos(tg::angle_f::make_from_radians(mid)),
                                       c[1] + control_scale * tg::sin(tg::angle_f::make_from_radians(mid)));
        o.quad_to(control, at(step * f32(i + 1)));
    }
    o.close();
    return o;
}
} // namespace

TEST("sr::slug - a rectangle compiles into one run and bands that cover it")
{
    auto const shape
        = sr::compile_slug_shape(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(100, 50))));
    REQUIRE(!shape.is_empty());

    // four curves and the closing texel, on one run
    CHECK(shape.curve_texels.size() == 5);
    REQUIRE(shape.run_ends.size() == 1);
    CHECK(shape.run_ends[0] == 5);

    // centred on the rectangle, and scaled by a power of two bringing its half-extent of 50 into (1024, 2048],
    // where every integer is exact
    CHECK(shape.stored_origin == tg::pos2f(50, 25));
    CHECK(shape.em_scale == 32.0f);
    CHECK(shape.em_bounds.min == tg::pos2f(-1600, -800));
    CHECK(shape.em_bounds.max == tg::pos2f(1600, 800));

    // horizontal lines are in no horizontal band, vertical lines in no vertical one: two curves each
    for (auto const& band : shape.horizontal_bands)
        CHECK(band.size() == 2);
    for (auto const& band : shape.vertical_bands)
        CHECK(band.size() == 2);
}

TEST("sr::slug - a rectangle is covered inside, uncovered outside, and half covered on its edge")
{
    auto const p = place(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(100, 50))));
    CHECK(coverage(p, tg::pos2f(50, 25)) == 1.0f);
    CHECK(coverage(p, tg::pos2f(10, 40)) == 1.0f);
    CHECK(coverage(p, tg::pos2f(150, 25)) == 0.0f);
    CHECK(coverage(p, tg::pos2f(50, -10)) == 0.0f);
    CHECK(tg::abs(coverage(p, tg::pos2f(100, 25)) - 0.5f) < 0.01f);
    CHECK(tg::abs(coverage(p, tg::pos2f(50, 0)) - 0.5f) < 0.01f);
    // a quarter of a pixel inside the edge
    CHECK(tg::abs(coverage(p, tg::pos2f(99.75f, 25)) - 0.75f) < 0.01f);
}

TEST("sr::slug - a shape far from the origin keeps the precision it has at the origin")
{
    // Scaling by the largest coordinate rather than the half-extent turned a 3x3 square at (10000, 10000) into nothing.
    for (auto const side : {3.0f, 10.0f})
    {
        auto const near = place(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(side, side))));
        auto const far = place(
            sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(10000, 10000), tg::pos2f(10000 + side, 10000 + side))));
        CHECK(near.ref.em_scale == far.ref.em_scale);
        CHECK(near.ref.em_bounds == far.ref.em_bounds);
        for (auto const t : {-0.25f, 0.0f, 0.1f, 0.5f, 0.9f, 1.0f, 1.25f})
        {
            auto const at = tg::pos2f(side * t, side * 0.5f);
            auto const pixel = side / 16.0f;
            CHECK(coverage(near, at, pixel) == coverage(far, at + tg::vec2f(10000, 10000), pixel));
        }
    }
}

TEST("sr::slug - a reversed inner contour is a hole under the nonzero rule")
{
    auto o = sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(100, 100)));
    // clockwise, against the outer contour
    o.move_to(tg::pos2f(30, 30));
    o.line_to(tg::pos2f(30, 70));
    o.line_to(tg::pos2f(70, 70));
    o.line_to(tg::pos2f(70, 30));
    o.close();
    auto const p = place(o);
    CHECK(coverage(p, tg::pos2f(50, 50)) == 0.0f);
    CHECK(coverage(p, tg::pos2f(10, 50)) == 1.0f);
}

TEST("sr::slug - an inner contour wound the same way fills under nonzero and is a hole under even-odd")
{
    auto o = sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(100, 100)));
    auto const inner = sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(30, 30), tg::pos2f(70, 70)));
    for (auto const& c : inner.curves)
        o.curves.push_back(c);
    o.contour_ends.push_back(i32(o.curves.size()));

    auto const nonzero = place(o);
    CHECK(coverage(nonzero, tg::pos2f(50, 50)) == 1.0f);

    o.fill_rule = sr::slug_fill_rule::even_odd;
    auto const even_odd = place(o);
    CHECK(coverage(even_odd, tg::pos2f(50, 50)) == 0.0f);
    CHECK(coverage(even_odd, tg::pos2f(10, 50)) == 1.0f);
}

TEST("sr::slug - a circle of quadratic arcs covers its area")
{
    auto const p = place(circle(tg::pos2f(50, 50), 40.0f, 16));

    auto covered = 0.0;
    for (auto y = 0; y < 100; ++y)
        for (auto x = 0; x < 100; ++x)
            covered += coverage(p, tg::pos2f(f32(x) + 0.5f, f32(y) + 0.5f));
    auto const expected = 3.14159265 * 40.0 * 40.0;
    CHECK(tg::abs(covered - expected) / expected < 0.01);

    // the curves bend: points just inside and outside the arc, away from the polygon's chords
    CHECK(coverage(p, tg::pos2f(50, 89)) == 1.0f);
    CHECK(coverage(p, tg::pos2f(50, 91.5f)) == 0.0f);
}

TEST("sr::slug - the point test agrees with the coverage wherever the coverage is not an edge's")
{
    // a circle with a hole wound against it, and the same pair under even-odd, which the point test must both follow
    auto o = circle(tg::pos2f(50, 50), 40.0f, 16);
    auto const hole = circle(tg::pos2f(50, 50), 15.0f, 8, true);
    for (auto const& c : hole.curves)
        o.curves.push_back(c);
    o.contour_ends.push_back(i32(o.curves.size()));
    auto const nonzero = place(o);
    o.fill_rule = sr::slug_fill_rule::even_odd;
    auto const even_odd = place(o);

    auto compared = 0;
    for (auto const* const p : {&nonzero, &even_odd})
        for (auto y = 0; y < 100; ++y)
            for (auto x = 0; x < 100; ++x)
            {
                // off the pixel grid, so no sample lands on a curve's end point or a band's edge by construction
                auto const at = tg::pos2f(f32(x) + 0.37f, f32(y) + 0.61f);
                auto const c = coverage(*p, at, 0.01f);
                if (c > 0.0f && c < 1.0f)
                    continue; // within a hundredth of an edge, where the two may round apart
                auto const inside = sr::impl::slug_reference_contains(p->atlas, p->instance, stored(p->ref, at));
                CHECK(inside == (c == 1.0f)).dump("x", x).dump("y", y);
                ++compared;
            }
    CHECK(compared > 19000);

    CHECK(!sr::impl::slug_reference_contains(nonzero.atlas, nonzero.instance, stored(nonzero.ref, tg::pos2f(50, 50))));
    CHECK(sr::impl::slug_reference_contains(nonzero.atlas, nonzero.instance, stored(nonzero.ref, tg::pos2f(20, 50))));
}

TEST("sr::slug - a traced quad's vertices lie where the instance places its em box")
{
    auto const p = place(sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(40, 20))));
    // two outline units per object unit along x, and y flipped, from an origin of (10, 5)
    auto const instance
        = sr::make_slug_instance(p.ref, tg::pos2f(10, 5), tg::vec2f(2, 0), tg::vec2f(0, -1), tg::vec4f(1, 1, 1, 1));
    auto const v = sr::slug_quad_vertices(cc::span<sr::slug_instance const>(&instance, 1));
    REQUIRE(v.size() == 6);

    // the box's corners back in outline units, mapped through the placement: min, (max.x, min.y), max, min, max, (min.x, max.y)
    auto const lo = p.ref.em_bounds.min;
    auto const hi = p.ref.em_bounds.max;
    auto const s = p.ref.em_scale;
    auto const o = p.ref.stored_origin;
    auto const at = [&](f32 ex, f32 ey) { return tg::pos3f(10 + 2 * (ex / s + o[0]), 5 - (ey / s + o[1]), 0); };
    tg::pos3f const expected[]
        = {at(lo[0], lo[1]), at(hi[0], lo[1]), at(hi[0], hi[1]), at(lo[0], lo[1]), at(hi[0], hi[1]), at(lo[0], hi[1])};
    for (auto i = 0; i < 6; ++i)
        CHECK(tg::distance(v[i], expected[i]) < 1e-4f).dump("vertex", i);
}

TEST("sr::slug - a cubic is followed within its tolerance")
{
    auto o = sr::slug_outline();
    o.move_to(tg::pos2f(0, 0));
    o.cubic_to(tg::pos2f(0, 100), tg::pos2f(100, 100), tg::pos2f(100, 0), 0.05f);
    o.close();
    REQUIRE(o.curves.size() > 2);

    // the cubic's own midpoint, which the quadratics have to pass close to
    auto const mid = tg::pos2f(50.0f, 75.0f);
    auto best = 1e9f;
    for (auto const& c : o.curves)
        for (auto i = 0; i <= 32; ++i)
        {
            auto const t = f32(i) / 32.0f;
            auto const u = 1.0f - t;
            auto const x = u * u * c.p1[0] + 2 * u * t * c.p2[0] + t * t * c.p3[0];
            auto const y = u * u * c.p1[1] + 2 * u * t * c.p2[1] + t * t * c.p3[1];
            best = cc::min(best, tg::sqrt((x - mid[0]) * (x - mid[0]) + (y - mid[1]) * (y - mid[1])));
        }
    CHECK(best < 0.1f);
}

TEST("sr::slug - an atlas keeps each shape's band block on one row and each run beside itself")
{
    auto atlas = sr::slug_atlas();
    auto refs = cc::vector<sr::slug_shape_ref>();
    // enough circles to wrap both textures past their first row
    for (auto i = 0; i < 100; ++i)
    {
        auto const r = atlas.add(sr::compile_slug_shape(circle(tg::pos2f(50, 50), 40.0f, 48)));
        REQUIRE(r.has_value());
        refs.push_back(r.value());
    }
    CHECK(atlas.band_texels().size() > sr::slug_atlas::width);
    CHECK(atlas.curve_texels().size() > sr::slug_atlas::width);

    // every one still covers its own centre, which a list split across rows or a run torn apart would not
    for (auto const& ref : refs)
    {
        auto const instance
            = sr::make_slug_instance(ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1));
        auto const s = ref.em_scale;
        CHECK(sr::impl::slug_reference_coverage(atlas, instance, stored(ref, tg::pos2f(50, 50)), tg::vec2f(s, s), false)
              == 1.0f);
        CHECK(sr::impl::slug_reference_coverage(atlas, instance, stored(ref, tg::pos2f(5, 5)), tg::vec2f(s, s), false)
              == 0.0f);
    }
    CHECK(atlas.has_pending_upload());
}

TEST("sr::slug - an empty outline places nothing and is not drawable")
{
    auto atlas = sr::slug_atlas();
    auto const ref = atlas.add(sr::compile_slug_shape(sr::slug_outline()));
    REQUIRE(ref.has_value());
    CHECK(!ref.value().is_drawable);
    CHECK(!atlas.has_pending_upload());
}

TEST("sr::slug - an outline is closed once every contour ends where it starts and contour_ends covers every curve")
{
    auto o = sr::slug_outline();
    o.move_to(tg::pos2f(0, 0));
    o.line_to(tg::pos2f(100, 0));
    o.line_to(tg::pos2f(100, 100));
    // The open contour still fills, with its missing edge aliased; compilation refuses it rather than draw that.
    CHECK(!o.is_closed());
    CHECK_ASSERTS(sr::compile_slug_shape(o));

    o.close();
    CHECK(o.is_closed());

    // Filled directly: the trailing curves need their contour_ends entry, and the contour has to come back to its start.
    auto direct = sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(10, 10)));
    direct.curves.push_back({.p1 = tg::pos2f(20, 20), .p2 = tg::pos2f(30, 20), .p3 = tg::pos2f(30, 20)});
    CHECK(!direct.is_closed());
    direct.contour_ends.push_back(i32(direct.curves.size()));
    CHECK(!direct.is_closed());
    direct.curves.push_back({.p1 = tg::pos2f(30, 20), .p2 = tg::pos2f(20, 20), .p3 = tg::pos2f(20, 20)});
    direct.contour_ends.back() = i32(direct.curves.size());
    CHECK(direct.is_closed());
}

TEST("sr::slug - a composite glyph that fans out exponentially is refused, not resolved")
{
    // Glyph 1 is a triangle; each glyph above it places the one below it `fan` times, so glyph n costs fan^(n-1).
    auto font = babel_test::test_font();
    font.glyphs.push_back({}); // .notdef
    auto triangle = babel_test::test_glyph();
    triangle.points.push_back({.position = tg::pos2i(0, 0), .on_curve = true});
    triangle.points.push_back({.position = tg::pos2i(100, 0), .on_curve = true});
    triangle.points.push_back({.position = tg::pos2i(50, 80), .on_curve = true});
    triangle.contour_ends.push_back(2);
    font.glyphs.push_back(triangle);
    auto const fan = 40;
    for (auto level = 0; level < 4; ++level)
    {
        auto g = babel_test::test_glyph();
        for (auto k = 0; k < fan; ++k)
        {
            auto const more = k + 1 < fan ? u16(0x0020) : u16(0);
            g.components.push_back(
                {.glyph = babel::font::glyph_id(u16(font.glyphs.size() - 1)), .flags = u16(0x0002 | more)});
        }
        font.glyphs.push_back(g);
    }
    auto const bytes = babel_test::build_font(font);
    auto const face = babel::font::read(cc::span<byte const>(bytes)).value();

    // two levels: 40 records of 3 points each, well within the bound
    CHECK(sr::slug_outline_of(face, babel::font::glyph_id(2)).has_value());

    // five levels would be 40^4 triangles; the bound stops it long before
    auto const deep = sr::slug_outline_of(face, babel::font::glyph_id(5));
    REQUIRE(deep.has_error());
    CHECK(deep.error().to_string().contains("expands past"));
}
