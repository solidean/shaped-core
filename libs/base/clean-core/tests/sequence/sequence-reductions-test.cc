#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/array.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
struct item
{
    int id;
    float weight;
};
} // namespace

TEST("sequence - chain is non-empty when either side is, the empty one first included")
{
    auto const none = cc::vector<int>();
    auto const some = cc::vector<int>{4, 2};

    auto chained = none.values().chain(some.values().assert_non_empty());
    static_assert(decltype(chained)::is_non_empty);
    CHECK(cc::move(chained).sum() == 6);
    CHECK(none.values().chain(some.values().assert_non_empty()).min() == 2);
    CHECK(none.values().chain(some.values().assert_non_empty()).first() == 4);
    CHECK(none.values().chain(some.values().assert_non_empty()).last() == 2);
    CHECK(some.values().assert_non_empty().chain(none).last() == 2);
}

TEST("sequence - pointer reductions")
{
    auto v = cc::vector<int>{3, 1, 4, 1, 5, 9, 2, 6};

    CHECK(v.values().min_ptr() == &v[1]); // the first of equal ones
    CHECK(v.values().max_ptr() == &v[5]);
    auto const none = cc::vector<int>();
    CHECK(none.values().min_ptr() == nullptr);

    auto items = cc::vector<item>{{1, 2.0f}, {2, 0.5f}, {3, 4.0f}, {4, 0.5f}};
    CHECK(items.values().min_ptr_by(&item::weight) == &items[1]);
    CHECK(items.values().max_ptr_by([](item const& i) { return i.weight; }) == &items[2]);

    // the pointer is into the container, so the element can be changed through it
    *v.values().max_ptr() = 0;
    CHECK(v[5] == 0);
}

TEST("sequence - into reductions fold several sequences into one result")
{
    auto const a = cc::vector<int>{3, 7, 1};
    auto const b = cc::vector<int>{9, 4};
    auto const empty = cc::vector<int>();

    int total = 0;
    a.values().sum_into(total);
    b.values().sum_into(total);
    CHECK(total == 24);

    int lo = 100;
    a.values().min_into(lo);
    b.values().min_into(lo);
    CHECK(lo == 1);

    auto hi = cc::optional<int>();
    empty.values().max_into(hi);
    CHECK(!hi.has_value());
    a.values().max_into(hi);
    b.values().max_into(hi);
    CHECK(hi.value() == 9);

    int const* p = nullptr;
    a.values().min_ptr_into(p);
    b.values().min_ptr_into(p);
    CHECK(p == &a[2]);

    auto items_a = cc::vector<item>{{1, 2.0f}, {2, 3.0f}};
    auto items_b = cc::vector<item>{{3, 5.0f}, {4, 1.0f}};
    item* heaviest = nullptr;
    items_a.values().max_ptr_by_into(heaviest, &item::weight);
    items_b.values().max_ptr_by_into(heaviest, &item::weight);
    CHECK(heaviest == &items_b[0]);
}

TEST("sequence - materializers")
{
    auto const v = cc::vector<int>{5, 3, 8, 1};

    auto const arr = v.values().map([](int x) { return x * 2; }).to_array();
    static_assert(std::is_same_v<decltype(arr), cc::array<int> const>);
    CHECK(arr.size() == 4);
    CHECK(arr[2] == 16);

    auto const fixed = v.values().to_fixed_array<4>();
    CHECK(fixed[3] == 1);

    auto const sorted = v.values().to_sorted_vector();
    CHECK(sorted[0] == 1);
    CHECK(sorted[3] == 8);
    auto const descending = v.values().to_sorted_vector(cc::default_greater{});
    CHECK(descending[0] == 8);

    auto const items = cc::vector<item>{{1, 2.0f}, {2, 0.5f}, {3, 4.0f}};
    auto const by_weight = items.values().to_sorted_vector_by(&item::weight);
    CHECK(by_weight[0].id == 2);
    CHECK(by_weight[2].id == 3);

    auto const words = cc::vector<int>{11, 23, 15, 27, 19};
    auto const by_tens = words.values().to_grouped_map([](int x) { return x / 10; });
    CHECK(by_tens.size() == 2);
    CHECK(by_tens.get(1).size() == 3);
    CHECK(by_tens.get(1)[2] == 19); // in sequence order
    CHECK(by_tens.get(2)[0] == 23);
}
