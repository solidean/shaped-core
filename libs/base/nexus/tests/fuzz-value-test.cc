#include <clean-core/common/utility.hh>
#include <nexus/fuzz/value.hh>
#include <nexus/test.hh>

using namespace nx::fuzz;

TEST("fuzz value - boxes and reads a value")
{
    auto v = fuzz_value::create(42);
    CHECK(v.is_valid());
    CHECK(v.is<int>());
    CHECK(!v.is<float>());
    CHECK(v.get<int>() == 42);
}

TEST("fuzz value - get<T&> mutates in place")
{
    auto v = fuzz_value::create(5);
    v.get<int&>() = 9;
    CHECK(v.get<int>() == 9);
}

TEST("fuzz value - move leaves the source invalid")
{
    auto a = fuzz_value::create(7);
    auto b = cc::move(a);
    CHECK(!a.is_valid());
    REQUIRE(b.is_valid());
    CHECK(b.get<int>() == 7);
}

TEST("fuzz value - default is void / invalid")
{
    fuzz_value v;
    CHECK(!v.is_valid());
}

TEST("fuzz value - get_bool")
{
    auto t = fuzz_value::create(true);
    auto f = fuzz_value::create(false);
    CHECK(t.get_bool());
    CHECK(!f.get_bool());
}
