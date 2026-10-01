#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
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

/// The coverage at outline point `at`, sampled with pixels `pixel` outline units wide.
[[nodiscard]] f32 coverage(placed const& p, tg::pos2f at, f32 pixel = 1.0f)
{
    auto const s = p.ref.em_scale;
    return sr::impl::slug_reference_coverage(p.atlas, p.instance, tg::pos2f(at[0] * s, at[1] * s),
                                             tg::vec2f(pixel * s, pixel * s), false);
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

    // a power of two bringing 100 into (1024, 2048], where every integer is exact
    CHECK(shape.em_scale == 16.0f);
    CHECK(shape.em_bounds.min == tg::pos2f(0, 0));
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
        CHECK(sr::impl::slug_reference_coverage(atlas, instance, tg::pos2f(50 * s, 50 * s), tg::vec2f(s, s), false)
              == 1.0f);
        CHECK(sr::impl::slug_reference_coverage(atlas, instance, tg::pos2f(5 * s, 5 * s), tg::vec2f(s, s), false) == 0.0f);
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
