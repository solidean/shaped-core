#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <nexus/tests/typed_value.hh>

using nx::typed_value;

TEST("typed value - boxes and reads a value")
{
    auto v = typed_value::create(42);
    CHECK(v.is_valid());
    CHECK(v.is<int>());
    CHECK(!v.is<float>());
    CHECK(v.get<int>() == 42);
}

TEST("typed value - get<T&> mutates in place")
{
    auto v = typed_value::create(5);
    v.get<int&>() = 9;
    CHECK(v.get<int>() == 9);
}

TEST("typed value - move leaves the source invalid")
{
    auto a = typed_value::create(7);
    auto b = cc::move(a);
    CHECK(!a.is_valid());
    REQUIRE(b.is_valid());
    CHECK(b.get<int>() == 7);
}

TEST("typed value - default is void / invalid")
{
    typed_value v;
    CHECK(!v.is_valid());
}

TEST("typed value - get_bool")
{
    auto t = typed_value::create(true);
    auto f = typed_value::create(false);
    CHECK(t.get_bool());
    CHECK(!f.get_bool());
}
