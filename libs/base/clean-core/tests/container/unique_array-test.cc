#include <clean-core/container/unique_array.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// the factories return the derived container, so a unique_array's must not decay to a copyable cc::array
static_assert(std::is_same_v<decltype(cc::unique_array<int>::create_defaulted(3)), cc::unique_array<int>>);
static_assert(std::is_same_v<decltype(cc::unique_array<int>::create_filled(3, 7)), cc::unique_array<int>>);
static_assert(!std::is_copy_constructible_v<cc::unique_array<int>>);

TEST("unique_array - factories build a unique_array")
{
    auto a = cc::unique_array<int>::create_filled(3, 7);
    CHECK(a.size() == 3);
    CHECK(a[2] == 7);
}

TEST("unique_array - values() on a temporary owns it")
{
    auto const sum = cc::unique_array<int>::create_filled(4, 2).values().sum_from(0);
    CHECK(sum == 8);

    auto const a = cc::unique_array<int>{1, 2, 3};
    CHECK(a.values().assert_non_empty().max() == 3);
    CHECK(a.indices().contains(2));
}
