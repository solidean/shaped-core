#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/segment.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::segment3f>, "segment should be trivially copyable");

TEST("tg segment - construction")
{
    SECTION("default has both endpoints at the origin")
    {
        tg::segment3f s;
        CHECK(s.pos0 == tg::pos3f::zero);
        CHECK(s.pos1 == tg::pos3f::zero);
    }

    SECTION("explicit ctor stores the endpoints")
    {
        auto const a = tg::pos3f(0, 0, 0);
        auto const b = tg::pos3f(1, 0, 0);
        tg::segment3f const s(a, b);
        CHECK(s.pos0 == a);
        CHECK(s.pos1 == b);
    }
}

TEST("tg segment - equality")
{
    auto const a = tg::pos2i(0, 0);
    auto const b = tg::pos2i(3, 4);
    tg::segment2i const s(a, b);
    CHECK(s == tg::segment2i(a, b));
    CHECK(s != tg::segment2i(b, a)); // orientation matters
}
