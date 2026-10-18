#include "../approx.hh"
#include "conventions.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/geometry/query/query.hh>

// The round trip is view_projection's contract, so it is checked as a property over random inputs.
// Every convention is checked with a finite, an infinite, an off-center and an orthographic projection.
// The oracle for clipping is the point projection alone: a world point is visible when it is in front of the camera and lands inside the clip volume.

namespace
{
using namespace cc::primitive_defines;

struct setup
{
    cc::string name;
    tg::view_projection_d vp;
    bool infinite_far = false;
};

cc::vector<setup> all_setups()
{
    auto out = cc::vector<setup>();
    for (auto const& fam : tgtest::k_projection_families)
    {
        auto const eye = tg::pos3d(2, 1, -6);
        auto const target = tg::pos3d(0.5, -0.5, 1);
        auto const view = fam.right_handed ? tg::look_at_rh(eye, target) : tg::look_at(eye, target);
        auto const fov = tg::angle_d::make_from_degree(60);
        auto const name = cc::string(fam.name);
        out.push_back({.name = name + " perspective",
                       .vp = tg::view_projection_d::make(view, fam.perspective(fov, 1.5, 0.5, 40), fam.depth)});
        out.push_back({.name = name + " infinite",
                       .vp = tg::view_projection_d::make(view, fam.perspective_infinite(fov, 1.5, 0.5), fam.depth),
                       .infinite_far = true});
        out.push_back({.name = name + " orthographic",
                       .vp = tg::view_projection_d::make(view, fam.orthographic(8, 6, 0.5, 40), fam.depth)});
        out.push_back({.name = name + " off-center",
                       .vp = tg::view_projection_d::make(
                           view, fam.perspective_off_center(-0.3, 0.6, -0.2, 0.4, 0.5, 40), fam.depth)});
    }
    return out;
}

/// a depth strictly inside the range, never at an infinite far plane
f64 inner_depth(tg::view_projection_d const& vp, cc::random& rng)
{
    auto const lo = vp.near_depth() < vp.far_depth() ? vp.near_depth() : vp.far_depth();
    auto const hi = vp.near_depth() < vp.far_depth() ? vp.far_depth() : vp.near_depth();
    return rng.uniform(lo + 0.02 * (hi - lo), hi - 0.02 * (hi - lo));
}

/// Inside the clip volume by more than `margin`; a negative margin also accepts points just outside it.
bool is_visible(tg::view_projection_d const& vp, tg::pos3d p, f64 margin = 1e-6)
{
    auto const w = tg::impl::projective_w(vp.world_to_clip(), p);
    if (w <= 0)
        return false;
    auto const q = vp.project(p);
    auto const lo = vp.near_depth() < vp.far_depth() ? vp.near_depth() : vp.far_depth();
    auto const hi = vp.near_depth() < vp.far_depth() ? vp.far_depth() : vp.near_depth();
    return -1 + margin < q.data[0] && q.data[0] < 1 - margin && -1 + margin < q.data[1] && q.data[1] < 1 - margin
        && lo + margin < q.data[2] && q.data[2] < hi - margin;
}

tg::pos3d random_world_point(cc::random& rng)
{
    return tg::pos3d(rng.uniform(-30.0, 30.0), rng.uniform(-30.0, 30.0), rng.uniform(-30.0, 30.0));
}

/// the parameter an end at infinity stands for
auto constexpr k_unbounded = 1e300;

/// The parameter along `origin + t * dir` that the NDC end `ndc` of a projected object stands for.
/// On an infinite far plane it is the vanishing point, so the end is at infinity, and the pick ray through it runs along the object.
f64 end_parameter(setup const& s, tg::pos3d origin, tg::vec3d dir, tg::pos3d ndc, bool upper)
{
    if (s.infinite_far && tgtest::approx(ndc.data[2], s.vp.far_depth(), 1e-9))
    {
        auto const pick = s.vp.unproject(tg::pos2d(ndc.data[0], ndc.data[1]));
        CHECK(tgtest::approx(pick.dir, (upper ? dir : -dir).normalized(), 1e-6));
        return upper ? k_unbounded : -k_unbounded;
    }
    auto const p = s.vp.unproject(ndc);
    auto const t = tg::dot(p - origin, dir) / tg::dot(dir, dir);
    CHECK(tgtest::approx(origin + dir * t, p, 1e-6)); // the end lies on the object
    return t;
}

/// Every visible sample in [lo, hi] lies inside [t0, t1], and every sample well inside [t0, t1] is visible.
/// An empty interval, t0 > t1, therefore checks that nothing sampled is visible.
template <class AtT>
void check_interval_against_samples(tg::view_projection_d const& vp, AtT const& at, f64 t0, f64 t1, f64 lo, f64 hi)
{
    auto constexpr samples = 400;
    for (auto k = 0; k <= samples; ++k)
    {
        auto const t = lo + (hi - lo) * k / samples;
        // a sample clearly inside the volume is inside the interval, and one inside the interval is at worst on the volume's boundary
        if (is_visible(vp, at(t)))
            CHECK((t0 - 1e-6 <= t && t <= t1 + 1e-6));
        if (t0 + 1e-3 < t && t < t1 - 1e-3)
            CHECK(is_visible(vp, at(t), -1e-9));
    }
}
} // namespace

TEST("tg camera - a point projects and unprojects back")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0xCA3E);
            for (auto i = 0; i < 200; ++i)
            {
                // anywhere in front of the camera, inside the clip volume or not
                auto const q = tg::pos3d(rng.uniform(-3.0, 3.0), rng.uniform(-3.0, 3.0), inner_depth(s.vp, rng));
                auto const p = s.vp.unproject(q);
                CHECK(tgtest::approx(s.vp.project(p), q, 1e-9));
                CHECK(tgtest::approx(s.vp.unproject(s.vp.project(p)), p, 1e-7));
            }
        }
}

TEST("tg camera - the pick ray starts on the near plane, stays on its pixel and points into the scene")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x91C6);
            auto const toward_far = s.vp.far_depth() - s.vp.near_depth();
            for (auto i = 0; i < 100; ++i)
            {
                auto const xy = tg::pos2d(rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0));
                auto const r = s.vp.unproject(xy);
                CHECK(tgtest::approx(r.dir.length(), 1.0, 1e-9));
                CHECK(tgtest::approx(s.vp.project(r.origin), tg::pos3d(xy.data[0], xy.data[1], s.vp.near_depth()), 1e-9));
                for (auto const t : {0.5, 3.0, 25.0})
                {
                    auto const q = s.vp.project(r.at(t));
                    CHECK(tgtest::approx(tg::pos2d(q.data[0], q.data[1]), xy, 1e-7));
                    // an orthographic ray pointing backward stays on its pixel too, so the depth says which way it goes
                    CHECK((q.data[2] - s.vp.near_depth()) * toward_far > 0);
                }
            }
        }
}

TEST("tg camera - the f32 pick ray stays precise far from the origin")
{
    // the direction comes from the far end, so it does not degrade with the eye's distance over the near distance
    auto const vp_d = tg::view_projection_d::make(tg::look_at(tg::pos3d(1000, 300, -2000), tg::pos3d(1010, 295, -1990)),
                                                  tg::perspective(tg::angle_d::make_from_degree(45), 1.5, 0.1));
    auto const vp_f = tg::view_projection_f::make(tg::look_at(tg::pos3f(1000, 300, -2000), tg::pos3f(1010, 295, -1990)),
                                                  tg::perspective(tg::angle_f::make_from_degree(45), 1.5f, 0.1f));
    for (auto const xy : {tg::pos2d(0, 0), tg::pos2d(0.7, -0.4), tg::pos2d(-1, 1)})
    {
        auto const exact = vp_d.unproject(xy).dir;
        auto const r = vp_f.unproject(tg::pos2f(f32(xy.data[0]), f32(xy.data[1])));
        auto const d = tg::vec3d(r.dir.data[0], r.dir.data[1], r.dir.data[2]);
        CHECK((d - exact).length() < 1e-5);
    }
}

TEST("tg camera - a point round-trips in f32 too")
{
    auto const vp = tg::view_projection_f::make(tg::look_at(tg::pos3f(2, 1, -6), tg::pos3f(0.5f, -0.5f, 1)),
                                                tg::perspective(tg::angle_f::make_from_degree(60), 1.5f, 0.5f, 40.0f));
    auto rng = cc::random(0xF32F);
    for (auto i = 0; i < 100; ++i)
    {
        auto const q = tg::pos3f(rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f), rng.uniform(0.05f, 0.95f));
        CHECK(tgtest::approx(vp.project(vp.unproject(q)), q, 1e-4f));
    }
}

TEST("tg camera - a segment projects to its visible part")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x5E61);
            auto visible_count = 0;
            for (auto i = 0; i < 300; ++i)
            {
                auto const seg = tg::segment3d(random_world_point(rng), random_world_point(rng));
                auto const dir = seg.pos1 - seg.pos0;
                auto const projected = s.vp.project(seg);

                // the visible interval, as parameters along the segment; empty when nothing came back
                auto t0 = 1.0;
                auto t1 = 0.0;
                if (projected.has_value())
                {
                    ++visible_count;
                    t0 = end_parameter(s, seg.pos0, dir, projected.value().pos0, false);
                    t1 = end_parameter(s, seg.pos0, dir, projected.value().pos1, true);
                    CHECK(t0 > -1e-9);
                    CHECK(t1 < 1 + 1e-9);
                }
                check_interval_against_samples(s.vp, [&](f64 t) { return seg.pos0 + dir * t; }, t0, t1, 0, 1);
            }
            CHECK(visible_count > 10); // the property was not checked vacuously
        }
}

TEST("tg camera - a ray projects to its visible part, ending at its vanishing point if it reaches infinity")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x4A75);
            auto reaches_infinity = 0;
            auto visible_count = 0;
            for (auto i = 0; i < 300; ++i)
            {
                auto const r
                    = tg::ray3d(random_world_point(rng), (random_world_point(rng) - tg::pos3d::zero).normalized());
                auto const projected = s.vp.project(r);

                auto t0 = 1.0;
                auto t1 = 0.0;
                if (projected.has_value())
                {
                    ++visible_count;
                    t0 = end_parameter(s, r.origin, r.dir, projected.value().pos0, false);
                    t1 = end_parameter(s, r.origin, r.dir, projected.value().pos1, true);
                    if (t1 == k_unbounded)
                        ++reaches_infinity;
                }
                check_interval_against_samples(s.vp, [&](f64 t) { return r.at(t); }, t0, t1, 0, 100);
            }
            CHECK(visible_count > 10);
            if (s.infinite_far)
                CHECK(reaches_infinity > 0);
        }
}

TEST("tg camera - a line projects to its visible part, in both directions")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x11AE);
            auto visible_count = 0;
            for (auto i = 0; i < 200; ++i)
            {
                auto const l
                    = tg::line3d(random_world_point(rng), (random_world_point(rng) - tg::pos3d::zero).normalized());
                auto const projected = s.vp.project(l);

                auto t0 = 1.0;
                auto t1 = 0.0;
                if (projected.has_value())
                {
                    ++visible_count;
                    t0 = end_parameter(s, l.origin, l.dir, projected.value().pos0, false);
                    t1 = end_parameter(s, l.origin, l.dir, projected.value().pos1, true);
                }
                check_interval_against_samples(s.vp, [&](f64 t) { return l.origin + l.dir * t; }, t0, t1, -100, 100);
            }
            CHECK(visible_count > 10);
        }
}

TEST("tg camera - the frustum is the view volume, in every convention")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0xF7A5);
            auto const f = s.vp.frustum();
            CHECK(f.has_far_plane() == !s.infinite_far);
            for (auto i = 0; i < 300; ++i)
            {
                auto const p = random_world_point(rng);
                if (is_visible(s.vp, p))
                    CHECK(f.contains(p));
            }
            // a point just outside each side of the clip volume is outside the frustum
            for (auto const q :
                 {tg::pos3d(1.01, 0, 0.5), tg::pos3d(-1.01, 0, 0.5), tg::pos3d(0, 1.01, 0.5), tg::pos3d(0, -1.01, 0.5)})
            {
                auto const depth = (s.vp.near_depth() + s.vp.far_depth()) / 2;
                CHECK(!f.contains(s.vp.unproject(tg::pos3d(q.data[0], q.data[1], depth))));
            }

            // the near plane is planes[4], with the eye's side outside it
            auto const axis = s.vp.unproject(tg::pos2d(0, 0));
            auto const on_plane = [](tg::plane3d const& pl, tg::pos3d p)
            { return tgtest::approx(tg::dot(pl.normal, p - tg::pos3d::zero), pl.dist, 1e-6); };
            CHECK(on_plane(f.planes[4], axis.origin));
            CHECK(!f.contains(axis.at(-0.05)));
            CHECK(f.contains(axis.at(0.05)));
            if (!s.infinite_far)
            {
                // and the far plane is planes[5], with everything beyond it outside
                auto const far_center = s.vp.unproject(tg::pos3d(0, 0, s.vp.far_depth()));
                CHECK(on_plane(f.planes[5], far_center));
                CHECK(!f.contains(far_center + axis.dir * 0.05));
                CHECK(f.contains(far_center - axis.dir * 0.05));
            }
        }
}

TEST("tg camera - pixels and normalized device coordinates")
{
    auto const size = tg::vec2d(640, 480);
    CHECK(tgtest::approx(tg::ndc_from_pixel(tg::pos2d(0, 0), size), tg::pos2d(-1, 1)));
    CHECK(tgtest::approx(tg::ndc_from_pixel(tg::pos2d(640, 480), size), tg::pos2d(1, -1)));
    CHECK(tgtest::approx(tg::ndc_from_pixel(tg::pos2d(320, 240), size), tg::pos2d(0, 0)));

    auto rng = cc::random(0xB1C5);
    for (auto i = 0; i < 50; ++i)
    {
        auto const px = tg::pos2d(rng.uniform(0.0, 640.0), rng.uniform(0.0, 480.0));
        CHECK(tgtest::approx(tg::pixel_from_ndc(tg::ndc_from_pixel(px, size), size), px, 1e-9));
    }
}
