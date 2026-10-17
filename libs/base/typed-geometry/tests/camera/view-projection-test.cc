#include "../approx.hh"
#include "conventions.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/geometry/query/query.hh>

// The round trip is view_projection's contract, so it is checked as a property over random inputs, for every
// convention and for a perspective with a finite far plane, one with an infinite far plane, and an orthographic one.
// The oracle for clipping is the point projection alone: a world point is visible when it is in front of the camera
// and lands inside the clip volume.

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

bool is_visible(tg::view_projection_d const& vp, tg::pos3d p)
{
    auto const w = tg::impl::projective_w(vp.world_to_clip(), p);
    if (w <= 0)
        return false;
    auto const q = vp.project(p);
    auto const lo = vp.near_depth() < vp.far_depth() ? vp.near_depth() : vp.far_depth();
    auto const hi = vp.near_depth() < vp.far_depth() ? vp.far_depth() : vp.near_depth();
    auto constexpr margin = 1e-6;
    return -1 + margin < q.data[0] && q.data[0] < 1 - margin && -1 + margin < q.data[1] && q.data[1] < 1 - margin
        && lo + margin < q.data[2] && q.data[2] < hi - margin;
}

tg::pos3d random_world_point(cc::random& rng)
{
    return tg::pos3d(rng.uniform(-30.0, 30.0), rng.uniform(-30.0, 30.0), rng.uniform(-30.0, 30.0));
}

/// the parameter of `p` along `origin + t * dir`
f64 parameter_along(tg::pos3d origin, tg::vec3d dir, tg::pos3d p)
{
    return tg::dot(p - origin, dir) / tg::dot(dir, dir);
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

TEST("tg camera - the pick ray starts on the near plane and stays on its pixel")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x91C6);
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
                }
            }
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

                // the visible interval, as parameters along the segment
                auto t0 = 1.0;
                auto t1 = 0.0;
                if (projected.has_value())
                {
                    ++visible_count;
                    auto const back = s.vp.unproject(projected.value());
                    t0 = parameter_along(seg.pos0, dir, back.pos0);
                    t1 = parameter_along(seg.pos0, dir, back.pos1);
                    // both ends lie on the segment
                    CHECK(tgtest::approx(seg.pos0 + dir * t0, back.pos0, 1e-6));
                    CHECK(tgtest::approx(seg.pos0 + dir * t1, back.pos1, 1e-6));
                    CHECK(t0 > -1e-9);
                    CHECK(t1 < 1 + 1e-9);
                }
                // every visible sample is inside the interval, and every sample well inside it is visible
                for (auto k = 0; k <= 64; ++k)
                {
                    auto const t = k / 64.0;
                    auto const visible = is_visible(s.vp, seg.pos0 + dir * t);
                    if (visible)
                        CHECK((t0 - 1e-6 <= t && t <= t1 + 1e-6));
                    if (t0 + 1e-3 < t && t < t1 - 1e-3)
                        CHECK(visible);
                }
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
            for (auto i = 0; i < 300; ++i)
            {
                auto const r
                    = tg::ray3d(random_world_point(rng), (random_world_point(rng) - tg::pos3d::zero).normalized());
                auto const projected = s.vp.project(r);

                // samples along the ray, out to well past the far plane
                auto first_visible = -1.0;
                auto last_visible = -1.0;
                for (auto k = 0; k <= 400; ++k)
                {
                    auto const t = k * 0.25;
                    if (is_visible(s.vp, r.at(t)))
                    {
                        if (first_visible < 0)
                            first_visible = t;
                        last_visible = t;
                    }
                }
                if (first_visible < 0)
                    continue; // nothing sampled is visible; a sliver between samples may still be
                REQUIRE(projected.has_value());

                auto const& seg = projected.value();
                auto const start = s.vp.unproject(seg.pos0);
                CHECK(tgtest::approx(start, r.at(r.parameter_of(start)), 1e-6));
                CHECK(r.parameter_of(start) <= first_visible + 1e-6);

                auto const at_far = tgtest::approx(seg.pos1.data[2], s.vp.far_depth(), 1e-9);
                if (s.infinite_far && at_far)
                {
                    // the vanishing point: the pick ray through it runs parallel to r
                    ++reaches_infinity;
                    auto const pick = s.vp.unproject(tg::pos2d(seg.pos1.data[0], seg.pos1.data[1]));
                    CHECK(tgtest::approx(pick.dir, r.dir, 1e-6));
                }
                else
                {
                    auto const end = s.vp.unproject(seg.pos1);
                    CHECK(tgtest::approx(end, r.at(r.parameter_of(end)), 1e-6));
                    CHECK(r.parameter_of(end) >= last_visible - 1e-6);
                }
            }
            if (s.infinite_far)
                CHECK(reaches_infinity > 0);
        }
}

TEST("tg camera - a line projects like the two rays it is made of")
{
    for (auto const& s : all_setups())
        SECTION("{}", s.name)
        {
            auto rng = cc::random(0x11AE);
            for (auto i = 0; i < 100; ++i)
            {
                auto const l
                    = tg::line3d(random_world_point(rng), (random_world_point(rng) - tg::pos3d::zero).normalized());
                auto const projected = s.vp.project(l);
                auto const forward = s.vp.project(tg::ray3d(l.origin, l.dir));
                auto const backward = s.vp.project(tg::ray3d(l.origin, -l.dir));
                CHECK(projected.has_value() == (forward.has_value() || backward.has_value()));
            }
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
            // and a point just outside each side of the clip volume is outside the frustum
            for (auto const q :
                 {tg::pos3d(1.01, 0, 0.5), tg::pos3d(-1.01, 0, 0.5), tg::pos3d(0, 1.01, 0.5), tg::pos3d(0, -1.01, 0.5)})
            {
                auto const depth = (s.vp.near_depth() + s.vp.far_depth()) / 2;
                CHECK(!f.contains(s.vp.unproject(tg::pos3d(q.data[0], q.data[1], depth))));
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
