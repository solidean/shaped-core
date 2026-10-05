#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/slug_reference.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_path.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// Paths and strokes on the CPU.
// A stroke is checked against its definition: a point is inside exactly when it lies within half the width of the path,
// measured by the cap and join the style asks for, which is a distance this file computes without the stroker.

namespace
{
[[nodiscard]] f64 cross(f64 ax, f64 ay, f64 bx, f64 by)
{
    return ax * by - ay * bx;
}

/// The winding number of `outline` around `p`, from the crossings of a ray toward +x.
[[nodiscard]] int winding(sr::slug_outline const& outline, tg::pos2f p)
{
    auto w = 0;
    for (auto const& c : outline.curves)
    {
        // y(t) = a t^2 + b t + c0, solved for y(t) == p.y on [0, 1)
        auto const a = f64(c.p1[1]) - 2.0 * f64(c.p2[1]) + f64(c.p3[1]);
        auto const b = 2.0 * (f64(c.p2[1]) - f64(c.p1[1]));
        auto const c0 = f64(c.p1[1]) - f64(p[1]);
        f64 roots[2] = {-1.0, -1.0};
        if (a == 0.0)
        {
            if (b != 0.0)
                roots[0] = -c0 / b;
        }
        else
        {
            auto const disc = b * b - 4.0 * a * c0;
            if (disc >= 0.0)
            {
                auto const sq = tg::sqrt(disc);
                roots[0] = (-b - sq) / (2.0 * a);
                roots[1] = (-b + sq) / (2.0 * a);
            }
        }
        for (auto const t : roots)
        {
            if (t < 0.0 || t >= 1.0)
                continue;
            auto const u = 1.0 - t;
            auto const x = u * u * f64(c.p1[0]) + 2.0 * u * t * f64(c.p2[0]) + t * t * f64(c.p3[0]);
            auto const dy = 2.0 * a * t + b;
            if (x > f64(p[0]) && dy != 0.0)
                w += dy > 0.0 ? 1 : -1;
        }
    }
    return w;
}

[[nodiscard]] f32 distance_to_segment(tg::pos2f p, tg::pos2f a, tg::pos2f b)
{
    auto const ab = b - a;
    auto const len2 = tg::dot(ab, ab);
    auto const t = len2 > 0.0f ? cc::clamp(tg::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}

/// The path as a dense polyline, close enough that distances to it are exact at test precision.
[[nodiscard]] cc::vector<tg::pos2f> sampled(sr::slug_curve const& c, int n = 2048)
{
    auto out = cc::vector<tg::pos2f>();
    for (auto i = 0; i <= n; ++i)
    {
        auto const t = f32(i) / f32(n);
        auto const u = 1.0f - t;
        out.push_back(tg::pos2f(u * u * c.p1[0] + 2.0f * u * t * c.p2[0] + t * t * c.p3[0],
                                u * u * c.p1[1] + 2.0f * u * t * c.p2[1] + t * t * c.p3[1]));
    }
    return out;
}

/// The distance from `p` to the nearest point of `path`'s curves: the region a round join and round cap stroke covers.
[[nodiscard]] f32 distance_to_path(tg::pos2f p, sr::slug_path const& path)
{
    auto best = 3.0e38f;
    for (auto const& c : path.curves())
    {
        auto const points = sampled(c);
        for (auto i = isize(1); i < points.size(); ++i)
            best = cc::min(best, distance_to_segment(p, points[i - 1], points[i]));
    }
    return best;
}

/// Every point of a grid over `box` is covered exactly when `inside` says so, but within `margin` of the boundary.
/// The winding is never negative either way: the stroker's pieces add.
template <class F>
void check_region(sr::slug_outline const& outline, tg::aabb2f box, f32 step, f32 margin, F&& signed_distance)
{
    // offset by an irrational fraction, so no sample lies on a curve's end height
    for (auto y = box.min[1] + step * 0.3819f; y < box.max[1]; y += step)
        for (auto x = box.min[0] + step * 0.2718f; x < box.max[0]; x += step)
        {
            auto const p = tg::pos2f(x, y);
            auto const d = signed_distance(p);
            auto const w = winding(outline, p);
            auto const where = cc::format("({}, {}), signed distance {}, winding {}", x, y, d, w);
            CHECK(w >= 0).context(where);
            if (d < -margin)
                CHECK(w > 0).context(where);
            else if (d > margin)
                CHECK(w == 0).context(where);
        }
}

/// A shape placed in its own atlas, read back through the CPU reference of Slug's pixel shader.
[[nodiscard]] f32 reference_coverage(sr::slug_outline const& outline, tg::pos2f at)
{
    auto atlas = sr::slug_atlas();
    auto const ref = atlas.add(sr::compile_slug_shape(outline)).value();
    auto const instance
        = sr::make_slug_instance(ref, tg::pos2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1));
    auto const s = ref.em_scale;
    auto const stored = tg::pos2f((at[0] - ref.stored_origin[0]) * s, (at[1] - ref.stored_origin[1]) * s);
    return sr::impl::slug_reference_coverage(atlas, instance, stored, tg::vec2f(s, s), false);
}

[[nodiscard]] isize contour_count(sr::slug_path const& p)
{
    return p.contours().size();
}
} // namespace

TEST("sr::slug_path - a segment drawn on from a circle starts where the circle does")
{
    auto p = sr::slug_path::circle(tg::pos2f(100, 100), 50);
    p.line_to(tg::pos2f(200, 100));
    CHECK(p.curves().back().p1 == tg::pos2f(150, 100));
}

TEST("sr::slug_path - contours stay open until closed, and a fill closes them")
{
    auto p = sr::slug_path();
    p.move_to(tg::pos2f(0, 0)).line_to(tg::pos2f(10, 0)).line_to(tg::pos2f(10, 10));
    p.move_to(tg::pos2f(20, 0)).line_to(tg::pos2f(30, 0)).line_to(tg::pos2f(30, 10)).close();
    // a move with nothing drawn after it leaves no contour
    p.move_to(tg::pos2f(50, 50)).move_to(tg::pos2f(60, 60));

    REQUIRE(contour_count(p) == 2);
    CHECK(!p.contours()[0].closed);
    CHECK(p.contours()[0].end == 2);
    CHECK(p.contours()[1].closed);
    CHECK(p.contours()[1].end == 5);

    auto const filled = p.to_outline();
    CHECK(filled.is_closed());
    // the open contour gains its closing line, the closed one already has it
    CHECK(filled.curves.size() == 6);
    CHECK(winding(filled, tg::pos2f(8, 2)) != 0);

    SECTION("drawing on after a close starts a new contour at the closed one's start")
    {
        auto q = sr::slug_path();
        q.move_to(tg::pos2f(0, 0)).line_to(tg::pos2f(1, 0)).close().line_to(tg::pos2f(0, 1));
        REQUIRE(contour_count(q) == 2);
        CHECK(q.curves().back().p1 == tg::pos2f(0, 0));
    }
}

TEST("sr::slug_path - a circle and a rounded rectangle stay within 1/4096 of their size")
{
    auto const r = 100.0f;
    auto const circle = sr::slug_path::circle(tg::pos2f(5, -3), r);
    REQUIRE(contour_count(circle) == 1);
    CHECK(circle.contours()[0].closed);
    CHECK(circle.curves().back().p3 == circle.curves().front().p1);
    for (auto const& c : circle.curves())
        for (auto const& p : sampled(c, 16))
            CHECK(tg::abs((p - tg::pos2f(5, -3)).length() - r) <= 2.0f * r / 4096.0f);

    auto const rounded = sr::slug_path::rounded_rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(80, 40)), 10.0f);
    auto const outline = rounded.to_outline();
    CHECK(winding(outline, tg::pos2f(40, 20)) != 0);
    CHECK(winding(outline, tg::pos2f(1, 1)) == 0);  // cut away by the corner's arc
    CHECK(winding(outline, tg::pos2f(10, 1)) != 0); // under the arc's end, on the straight edge
    // a radius past half the shorter side is clamped to it
    auto const pill = sr::slug_path::rounded_rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(80, 40)), 500.0f);
    CHECK(winding(pill.to_outline(), tg::pos2f(20, 20)) != 0);
    CHECK(winding(pill.to_outline(), tg::pos2f(2, 2)) == 0);
}

TEST("sr::stroke_outline - a round-joined, round-capped polyline covers what lies within half its width")
{
    tg::pos2f const points[]
        = {tg::pos2f(10, 10), tg::pos2f(90, 20), tg::pos2f(30, 60), tg::pos2f(80, 90), tg::pos2f(82, 40)};
    auto const path = sr::slug_path::polyline(points);
    auto const style = sr::stroke_style{.width = 12.0f, .join = sr::stroke_join::round, .cap = sr::stroke_cap::round};
    auto const outline = sr::stroke_outline(path, style, 0.01f);
    REQUIRE(!outline.is_empty());
    CHECK(outline.is_closed());
    check_region(outline, tg::aabb2f(tg::pos2f(-5, -5), tg::pos2f(105, 105)), 0.7f, 0.05f,
                 [&](tg::pos2f p) { return distance_to_path(p, path) - 6.0f; });
}

TEST("sr::stroke_outline - a curve bending tighter than the half-width still covers exactly its reach")
{
    // a hairpin: the curvature at its apex is far tighter than the half-width, so its inner offset folds over
    auto path = sr::slug_path();
    path.move_to(tg::pos2f(0, 0)).quad_to(tg::pos2f(100, 50), tg::pos2f(0, 100));
    auto hairpin = sr::slug_path();
    hairpin.move_to(tg::pos2f(20, 50)).quad_to(tg::pos2f(80, 52), tg::pos2f(20, 54));

    auto const style = sr::stroke_style{.width = 16.0f, .join = sr::stroke_join::round, .cap = sr::stroke_cap::round};
    for (auto const* p : {&path, &hairpin})
    {
        auto const outline = sr::stroke_outline(*p, style, 0.01f);
        REQUIRE(!outline.is_empty());
        check_region(outline, tg::aabb2f(tg::pos2f(-15, -15), tg::pos2f(115, 115)), 0.9f, 0.1f,
                     [&](tg::pos2f q) { return distance_to_path(q, *p) - 8.0f; });
    }
}

TEST("sr::stroke_outline - a curve that goes out and comes back strokes both ways")
{
    auto path = sr::slug_path();
    path.move_to(tg::pos2f(0, 0)).quad_to(tg::pos2f(10, 0), tg::pos2f(0, 0));
    auto const outline = sr::stroke_outline(path, {.width = 1.0f}, 0.01f);
    CHECK(winding(outline, tg::pos2f(4, 0)) != 0);
    CHECK(winding(outline, tg::pos2f(4, 2)) == 0);
}

TEST("sr::stroke_outline - joins and caps shape the corners and ends")
{
    // a right angle from (0, 0) to (50, 0) to (50, 50), stroked 10 wide: its outer corner is near (55, -5)
    tg::pos2f const points[] = {tg::pos2f(0, 0), tg::pos2f(50, 0), tg::pos2f(50, 50)};
    auto const path = sr::slug_path::polyline(points);
    auto const covered = [&](sr::stroke_style const& style, tg::pos2f at)
    { return winding(sr::stroke_outline(path, style, 0.01f), at) != 0; };

    auto const corner = tg::pos2f(54.5f, -4.5f);
    CHECK(covered({.width = 10.0f, .join = sr::stroke_join::miter}, corner));
    CHECK(!covered({.width = 10.0f, .join = sr::stroke_join::bevel}, corner));
    CHECK(!covered({.width = 10.0f, .join = sr::stroke_join::round}, corner));
    // the miter reaches sqrt(2) half-widths, past a limit of 1.2, so that corner bevels
    CHECK(!covered({.width = 10.0f, .join = sr::stroke_join::miter, .miter_limit = 1.2f}, corner));
    // on the corner's diagonal, inside a round join's reach and outside a bevel's
    CHECK(covered({.width = 10.0f, .join = sr::stroke_join::round}, tg::pos2f(53.3f, -3.3f)));
    CHECK(!covered({.width = 10.0f, .join = sr::stroke_join::bevel}, tg::pos2f(53.3f, -3.3f)));

    // the open end at (0, 0) runs toward -x
    auto const before_start = tg::pos2f(-3.0f, 3.0f);
    CHECK(!covered({.width = 10.0f, .cap = sr::stroke_cap::butt}, before_start));
    CHECK(covered({.width = 10.0f, .cap = sr::stroke_cap::square}, before_start));
    CHECK(covered({.width = 10.0f, .cap = sr::stroke_cap::round}, before_start));
    CHECK(!covered({.width = 10.0f, .cap = sr::stroke_cap::round}, tg::pos2f(-4.0f, 4.0f)));

    SECTION("a closed contour joins at its start rather than capping it")
    {
        auto const square = sr::slug_path::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(50, 50)));
        auto const outline = sr::stroke_outline(square, {.width = 10.0f, .cap = sr::stroke_cap::round}, 0.01f);
        CHECK(winding(outline, tg::pos2f(-4.5f, -4.5f)) != 0); // the mitred corner at the start
        CHECK(winding(outline, tg::pos2f(25, 25)) == 0);       // the hole stays empty
    }
}

TEST("sr::stroke_outline - Slug's own coverage reads a stroke as solid where its pieces overlap")
{
    tg::pos2f const points[] = {tg::pos2f(0, 0), tg::pos2f(60, 0), tg::pos2f(10, 30)};
    auto const outline
        = sr::stroke_outline(sr::slug_path::polyline(points),
                             {.width = 8.0f, .join = sr::stroke_join::round, .cap = sr::stroke_cap::round}, 0.01f);
    // inside one body, inside the sharp turn where both bodies and the join overlap, and between the two legs
    CHECK(tg::abs(reference_coverage(outline, tg::pos2f(30, 0.3f)) - 1.0f) <= 0.01f);
    CHECK(tg::abs(reference_coverage(outline, tg::pos2f(55, 1.5f)) - 1.0f) <= 0.01f);
    CHECK(reference_coverage(outline, tg::pos2f(30, 8)) <= 0.01f);
}

TEST("sr::dash_path - dashes start the pattern per contour, at the offset, and repeat an odd pattern")
{
    tg::pos2f const points[] = {tg::pos2f(0, 0), tg::pos2f(100, 0)};
    auto const line = sr::slug_path::polyline(points);
    auto const starts = [](sr::slug_path const& p)
    {
        auto out = cc::vector<tg::vec2f>();
        auto first = i32(0);
        for (auto const& c : p.contours())
        {
            out.push_back(tg::vec2f(p.curves()[first].p1[0], p.curves()[c.end - 1].p3[0]));
            first = c.end;
        }
        return out;
    };
    auto const near = [](cc::vector<tg::vec2f> const& got, cc::span<tg::vec2f const> want)
    {
        if (got.size() != want.size())
            return false;
        for (auto i = isize(0); i < got.size(); ++i)
            if (tg::abs(got[i][0] - want[i][0]) > 1e-3f || tg::abs(got[i][1] - want[i][1]) > 1e-3f)
                return false;
        return true;
    };

    tg::vec2f const plain[] = {tg::vec2f(0, 10),  tg::vec2f(15, 25), tg::vec2f(30, 40), tg::vec2f(45, 55),
                               tg::vec2f(60, 70), tg::vec2f(75, 85), tg::vec2f(90, 100)};
    CHECK(near(starts(sr::dash_path(line, {.dashes = {10, 5}})), plain));

    tg::vec2f const shifted[] = {tg::vec2f(0, 5),   tg::vec2f(10, 20), tg::vec2f(25, 35), tg::vec2f(40, 50),
                                 tg::vec2f(55, 65), tg::vec2f(70, 80), tg::vec2f(85, 95)};
    CHECK(near(starts(sr::dash_path(line, {.dashes = {10, 5}, .dash_offset = 5})), shifted));

    tg::vec2f const odd[]
        = {tg::vec2f(0, 10), tg::vec2f(20, 30), tg::vec2f(40, 50), tg::vec2f(60, 70), tg::vec2f(80, 90)};
    CHECK(near(starts(sr::dash_path(line, {.dashes = {10}})), odd));

    for (auto const& c : sr::dash_path(line, {.dashes = {10, 5}}).contours())
        CHECK(!c.closed);

    // a pattern that cannot dash draws solid
    CHECK(contour_count(sr::dash_path(line, {.dashes = {0, 0}})) == 1);
    CHECK(contour_count(sr::dash_path(line, {.dashes = {4, -1}})) == 1);

    SECTION("a closed contour's dash through its start is one dash, joined rather than capped")
    {
        auto const square = sr::slug_path::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(50, 50)));
        auto const style = sr::stroke_style{.width = 10.0f, .dashes = {10, 5}};
        CHECK(winding(sr::stroke_outline(square, style, 0.01f), tg::pos2f(-4.5f, -4.5f)) != 0);
    }

    SECTION("a zero-length dash draws its caps alone")
    {
        tg::pos2f const short_points[] = {tg::pos2f(0, 0), tg::pos2f(20, 0)};
        auto const short_line = sr::slug_path::polyline(short_points);
        auto const round
            = sr::stroke_outline(short_line, {.width = 2.0f, .cap = sr::stroke_cap::round, .dashes = {0, 4}}, 0.01f);
        for (auto const x : {0.0f, 4.0f, 8.0f, 12.0f, 16.0f, 20.0f})
        {
            CHECK(winding(round, tg::pos2f(x, 0.5f)) != 0);
            CHECK(winding(round, tg::pos2f(x + 2.0f, 0.0f)) == 0);
        }
        auto const butt = sr::stroke_outline(short_line, {.width = 2.0f, .dashes = {0, 4}}, 0.01f);
        // a butt dot is a sliver a thousandth of the width long, so beside it is empty where a round cap is not
        CHECK(winding(round, tg::pos2f(8.5f, 0.5f)) != 0);
        CHECK(winding(butt, tg::pos2f(8.5f, 0.5f)) == 0);
    }

    SECTION("along a curve, the dashes hold their share of its length")
    {
        auto const circle = sr::slug_path::circle(tg::pos2f(0, 0), 50.0f);
        auto const dashed = sr::dash_path(circle, {.dashes = {6, 4}});
        auto total = 0.0f;
        for (auto const& c : dashed.curves())
        {
            auto const points = sampled(c, 64);
            for (auto i = isize(1); i < points.size(); ++i)
                total += (points[i] - points[i - 1]).length();
        }
        CHECK(tg::abs(total / (2.0f * 3.14159265f * 50.0f * 0.6f) - 1.0f) <= 0.02f);
    }
}

TEST("sr::stroke_outline - nothing to stroke strokes nothing")
{
    tg::pos2f const points[] = {tg::pos2f(0, 0), tg::pos2f(10, 0)};
    auto const line = sr::slug_path::polyline(points);
    CHECK(sr::stroke_outline(line, {.width = 0.0f}, 0.01f).is_empty());
    CHECK(sr::stroke_outline(sr::slug_path(), {.width = 2.0f}, 0.01f).is_empty());
    auto dot = sr::slug_path();
    dot.move_to(tg::pos2f(5, 5)).line_to(tg::pos2f(5, 5));
    CHECK(sr::stroke_outline(dot, {.width = 2.0f, .cap = sr::stroke_cap::round}, 0.01f).is_empty());
}
