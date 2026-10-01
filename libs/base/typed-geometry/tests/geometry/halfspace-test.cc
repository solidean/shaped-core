#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/halfspace.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::halfspace3f>, "halfspace should be trivially copyable");

namespace
{
static_assert(tg::traits::intrinsic_dim<tg::halfspace3f> == 3, "a half-space is full-dimensional");
static_assert(!tg::traits::is_finite<tg::halfspace3f>);
static_assert(sizeof(tg::halfspace3f) == sizeof(tg::plane3f), "it shares the plane's encoding");
} // namespace

TEST("tg halfspace - its boundary is the plane with the same encoding")
{
    auto const h = tg::halfspace3f(tg::vec3f(0, 0, 1), 2.0f);
    auto const p = h.boundary();
    static_assert(std::is_same_v<decltype(p), tg::plane3f const>);
    CHECK(p.normal == h.normal);
    CHECK(p.dist == h.dist);
}

TEST("tg halfspace - transforms as its plane does")
{
    auto const h = tg::halfspace3f(tg::vec3f(0, 0, 1), 2.0f);
    auto const r = h.transformed(tg::translation_transform3f::make_translation(tg::vec3f(0, 0, 3)));
    static_assert(std::is_same_v<decltype(r), tg::halfspace3f const>);
    CHECK(tgtest::approx(r.normal, tg::vec3f(0, 0, 1)));
    CHECK(tgtest::approx(r.dist, 5.0f));
}
