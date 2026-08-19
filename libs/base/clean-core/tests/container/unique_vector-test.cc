#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/unique_vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// Move-only element, to pin that unique_vector's "unique" is about the container's ownership and not its elements.
struct MoveOnly
{
    int value = 0;

    MoveOnly() = default;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(MoveOnly&& rhs) noexcept : value(rhs.value) { rhs.value = -1; }
    MoveOnly& operator=(MoveOnly&& rhs) noexcept
    {
        value = rhs.value;
        rhs.value = -1;
        return *this;
    }
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;
};
} // namespace

TEST("unique_vector - push_back_range")
{
    cc::unique_vector<int> v;
    v.push_back(0);

    int const source[] = {1, 2, 3};
    v.push_back_range(cc::span<int const>(source));

    REQUIRE(v.size() == 4);
    CHECK(v[0] == 0);
    CHECK(v[3] == 3);
}

TEST("unique_vector - insertion family")
{
    SECTION("insert_at and emplace_at")
    {
        cc::unique_vector<int> v;
        for (int i = 0; i < 3; ++i)
            v.push_back(i); // 0 1 2

        v.insert_at(1, 9);
        v.emplace_at(0, 8);

        REQUIRE(v.size() == 5);
        CHECK(v[0] == 8);
        CHECK(v[1] == 0);
        CHECK(v[2] == 9);
        CHECK(v[3] == 1);
        CHECK(v[4] == 2);
    }

    SECTION("insert_range_at")
    {
        cc::unique_vector<int> v;
        v.push_back(0);
        v.push_back(3);

        int const source[] = {1, 2};
        auto const inserted = v.insert_range_at(1, cc::span<int const>(source));

        REQUIRE(v.size() == 4);
        CHECK(v[1] == 1);
        CHECK(v[2] == 2);
        CHECK(v[3] == 3);
        CHECK(inserted.size() == 2);
    }

    SECTION("replace_range")
    {
        cc::unique_vector<int> v;
        for (int i = 0; i < 5; ++i)
            v.push_back(i); // 0 1 2 3 4

        int const source[] = {7, 8, 9};
        v.replace_range({.offset = 1, .size = 2}, cc::span<int const>(source));

        REQUIRE(v.size() == 6);
        CHECK(v[0] == 0);
        CHECK(v[1] == 7);
        CHECK(v[3] == 9);
        CHECK(v[4] == 3);
        CHECK(v[5] == 4);
    }

    SECTION("move-only elements")
    {
        cc::unique_vector<MoveOnly> v;
        v.emplace_back(1);
        v.emplace_back(3);

        v.emplace_at(1, 2);

        REQUIRE(v.size() == 3);
        CHECK(v[0].value == 1);
        CHECK(v[1].value == 2);
        CHECK(v[2].value == 3);
    }
}
