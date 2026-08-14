#include <clean-core/container/disjoint_set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// Reference implementation: the partitioning as a plain element -> label map, merged the slow way.
struct naive_partition
{
    cc::vector<i32> label;

    explicit naive_partition(i32 n)
    {
        for (i32 i = 0; i < n; ++i)
            label.push_back(i);
    }

    void merge(i32 a, i32 b)
    {
        auto const from = label[b];
        auto const to = label[a];
        if (from == to)
            return;
        for (auto& l : label)
            if (l == from)
                l = to;
    }

    [[nodiscard]] bool same(i32 a, i32 b) const { return label[a] == label[b]; }
};
} // namespace

TEST("disjoint_set basics")
{
    auto ds = cc::disjoint_set<i32>::create_singletons(5);
    CHECK(ds.element_count() == 5);
    CHECK(ds.partition_count() == 5);

    for (i32 i = 0; i < 5; ++i)
    {
        CHECK(ds.is_representative(i));
        CHECK(ds.get_representative(i) == i);
        CHECK(ds.get_parent(i) == i);
        CHECK(ds.size_of_set_by_element(i) == 1);
    }

    CHECK(ds.merge_by_element(0, 1));
    CHECK(!ds.merge_by_element(0, 1)); // already together
    CHECK(ds.partition_count() == 4);
    CHECK(ds.are_in_same_set(0, 1));
    CHECK(!ds.are_in_same_set(0, 2));
    CHECK(ds.size_of_set_by_element(0) == 2);
    CHECK(ds.size_of_set_by_element(1) == 2);
    CHECK(ds.size_of_set_by_representative(ds.get_representative(1)) == 2);

    CHECK(ds.merge_by_representative(ds.get_representative(1), ds.get_representative(2)));
    CHECK(ds.partition_count() == 3);
    CHECK(ds.are_in_same_set(0, 2));
    CHECK(ds.size_of_set_by_element(2) == 3);
}

TEST("disjoint_set default state")
{
    auto ds = cc::disjoint_set<i32>();
    CHECK(ds.element_count() == 0);
    CHECK(ds.partition_count() == 0);

    auto const c = ds.compute_components();
    CHECK(c.component_to_representative.empty());
    CHECK(c.element_to_component.empty());
}

TEST("disjoint_set growth")
{
    auto ds = cc::disjoint_set<i32>();
    for (i32 i = 0; i < 100; ++i)
        CHECK(ds.add_element() == i);
    CHECK(ds.element_count() == 100);
    CHECK(ds.partition_count() == 100);

    // a chain, which is the shape that stresses path compression
    for (i32 i = 1; i < 100; ++i)
        CHECK(ds.merge_by_element(i - 1, i));
    CHECK(ds.partition_count() == 1);
    CHECK(ds.size_of_set_by_element(50) == 100);

    // path halving flattens the tree: one find halves the depth of what it walked, so repeated finds bottom out
    // with every element pointing straight at the root
    auto const root = ds.get_representative(0);
    for (auto pass = 0; pass < 10; ++pass)
        for (i32 i = 0; i < 100; ++i)
            CHECK(ds.get_representative(i) == root);
    for (i32 i = 0; i < 100; ++i)
        CHECK(ds.get_parent(i) == root);

    CHECK(ds.add_elements(3) == 100);
    CHECK(ds.element_count() == 103);
    CHECK(ds.partition_count() == 4);
    CHECK(!ds.are_in_same_set(0, 100));

    ds.reset_to_singletons(4);
    CHECK(ds.element_count() == 4);
    CHECK(ds.partition_count() == 4);
    for (i32 i = 0; i < 4; ++i)
        CHECK(ds.is_representative(i));

    ds.clear();
    CHECK(ds.element_count() == 0);
    CHECK(ds.partition_count() == 0);
    CHECK(ds.capacity() >= 4); // clear keeps the storage
}

TEST("disjoint_set components")
{
    auto ds = cc::disjoint_set<i32>::create_singletons(7);
    ds.merge_by_element(1, 4);
    ds.merge_by_element(4, 6);
    ds.merge_by_element(3, 5);

    auto const [component_to_representative, element_to_component] = ds.compute_components();

    CHECK(component_to_representative.size() == ds.partition_count());
    CHECK(component_to_representative.size() == 4); // {0} {1,4,6} {2} {3,5}
    CHECK(element_to_component.size() == 7);

    // component indices ascend in the representatives' element order
    for (isize c = 1; c < component_to_representative.size(); ++c)
        CHECK(component_to_representative[c - 1] < component_to_representative[c]);

    for (i32 e = 0; e < 7; ++e)
    {
        auto const c = element_to_component[e];
        CHECK(c >= 0);
        CHECK(c < i32(component_to_representative.size()));
        CHECK(ds.are_in_same_set(e, component_to_representative[c]));
        CHECK(ds.is_representative(component_to_representative[c]));
    }

    // two elements share a component index iff they share a set
    for (i32 a = 0; a < 7; ++a)
        for (i32 b = 0; b < 7; ++b)
            CHECK((element_to_component[a] == element_to_component[b]) == ds.are_in_same_set(a, b));
}

TEST("disjoint_set components reuse their storage")
{
    auto ds = cc::disjoint_set<i32>::create_singletons(8);
    ds.merge_by_element(0, 7);

    auto representatives = cc::vector<i32>();
    auto element_to_component = cc::vector<i32>();
    element_to_component.push_back(123); // must be overwritten, not appended to
    representatives.push_back(456);

    ds.compute_components(representatives, element_to_component);
    CHECK(representatives.size() == 7);
    CHECK(element_to_component.size() == 8);
    CHECK(element_to_component[0] == element_to_component[7]);

    // a second call over the same vectors allocates nothing and overwrites both
    auto const representatives_capacity = representatives.capacity();
    auto const mapping_capacity = element_to_component.capacity();
    ds.merge_by_element(1, 2);
    ds.compute_components(representatives, element_to_component);
    CHECK(representatives.size() == 6);
    CHECK(element_to_component.size() == 8);
    CHECK(representatives.capacity() == representatives_capacity);
    CHECK(element_to_component.capacity() == mapping_capacity);
    CHECK(element_to_component[1] == element_to_component[2]);
}

TEST("disjoint_set append")
{
    auto lhs = cc::disjoint_set<i32>::create_singletons(3);
    lhs.merge_by_element(0, 2);

    auto rhs = cc::disjoint_set<i32>::create_singletons(4);
    rhs.merge_by_element(1, 3);
    rhs.merge_by_element(3, 0);

    auto const offset = lhs.append(rhs);
    CHECK(offset == 3);
    CHECK(lhs.element_count() == 7);
    CHECK(lhs.partition_count() == 2 + 2);

    CHECK(lhs.are_in_same_set(0, 2));
    CHECK(lhs.are_in_same_set(offset + 0, offset + 1));
    CHECK(lhs.are_in_same_set(offset + 1, offset + 3));
    CHECK(!lhs.are_in_same_set(offset + 0, offset + 2));
    CHECK(lhs.size_of_set_by_element(offset + 3) == 3);

    // nothing merges across the seam
    for (i32 a = 0; a < 3; ++a)
        for (i32 b = 3; b < 7; ++b)
            CHECK(!lhs.are_in_same_set(a, b));

    // rhs is untouched
    CHECK(rhs.element_count() == 4);
    CHECK(rhs.partition_count() == 2);
}

TEST("disjoint_set self-append")
{
    auto ds = cc::disjoint_set<i32>::create_singletons(3);
    ds.merge_by_element(0, 1);

    auto const offset = ds.append(ds);
    CHECK(offset == 3);
    CHECK(ds.element_count() == 6);
    CHECK(ds.partition_count() == 4);
    CHECK(ds.are_in_same_set(3, 4));
    CHECK(!ds.are_in_same_set(3, 5));
    CHECK(!ds.are_in_same_set(0, 3));
}

TEST("disjoint_set copy and move")
{
    auto ds = cc::disjoint_set<i32>::create_singletons(6);
    ds.merge_by_element(0, 1);
    ds.merge_by_element(2, 3);

    auto copy = ds;
    CHECK(copy.element_count() == 6);
    CHECK(copy.partition_count() == 4);
    CHECK(copy.are_in_same_set(0, 1));
    CHECK(copy.are_in_same_set(2, 3));

    // the copy is independent
    copy.merge_by_element(1, 3);
    CHECK(copy.partition_count() == 3);
    CHECK(ds.partition_count() == 4);
    CHECK(!ds.are_in_same_set(1, 3));

    auto moved = cc::move(copy);
    CHECK(moved.element_count() == 6);
    CHECK(moved.partition_count() == 3);

    auto assigned = cc::disjoint_set<i32>::create_singletons(2);
    assigned = ds;
    CHECK(assigned.element_count() == 6);
    CHECK(assigned.partition_count() == 4);
    CHECK(assigned.are_in_same_set(2, 3));
}

TEST("disjoint_set index types")
{
    auto small = cc::disjoint_set<i16>::create_singletons(300);
    CHECK(small.element_count() == 300);
    small.merge_by_element(i16(299), i16(0));
    CHECK(small.are_in_same_set(i16(0), i16(299)));
    CHECK(small.size_of_set_by_element(i16(0)) == 2);

    auto unsigned_idx = cc::disjoint_set<u32>::create_singletons(10);
    unsigned_idx.merge_by_element(9u, 8u);
    CHECK(unsigned_idx.are_in_same_set(8u, 9u));
    CHECK(unsigned_idx.partition_count() == 9);
}

TEST("disjoint_set against a naive partition")
{
    auto constexpr element_count = 64;
    auto rng = cc::random(1234);

    auto representatives = cc::vector<i32>();
    auto element_to_component = cc::vector<i32>();

    for (auto round = 0; round < 20; ++round)
    {
        auto ds = cc::disjoint_set<i32>::create_singletons(element_count);
        auto naive = naive_partition(element_count);

        for (auto step = 0; step < element_count * 2; ++step)
        {
            auto const a = i32(rng.uniform(0, element_count - 1));
            auto const b = i32(rng.uniform(0, element_count - 1));

            CHECK(ds.merge_by_element(a, b) == !naive.same(a, b));
            naive.merge(a, b);

            for (i32 e = 0; e < element_count; ++e)
                CHECK(ds.are_in_same_set(a, e) == naive.same(a, e));
        }

        // partition count and set sizes agree with the reference
        ds.compute_components(representatives, element_to_component);
        auto distinct_labels = 0;
        for (i32 e = 0; e < element_count; ++e)
        {
            if (naive.label[e] == e)
                ++distinct_labels;

            auto expected_size = 0;
            for (i32 o = 0; o < element_count; ++o)
                if (naive.same(e, o))
                    ++expected_size;
            CHECK(ds.size_of_set_by_element(e) == expected_size);
        }
        CHECK(representatives.size() == distinct_labels);
        CHECK(ds.partition_count() == distinct_labels);
    }
}
