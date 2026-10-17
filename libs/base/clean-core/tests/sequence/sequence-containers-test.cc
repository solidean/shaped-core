#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/strided_span.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::map<int, float> make_weights()
{
    auto m = cc::map<int, float>();
    m[1] = 0.5f;
    m[2] = 1.5f;
    m[7] = 2.0f;
    return m;
}
} // namespace

TEST("sequence - map keys and values")
{
    auto m = make_weights();

    CHECK(m.keys().count() == 3);
    CHECK(m.keys().sum_from(0) == 10);
    CHECK(m.values().sum_from(0.0f) == 4.0f);

    // keys().contains is the map's own lookup, not a scan
    CHECK(m.keys().contains(7));
    CHECK(!m.keys().contains(3));

    // values reference the nodes, so they can be written through
    m.values().each([](float& w) { w *= 2.0f; });
    CHECK(m.get(2) == 3.0f);

    // keys and values of the same map walk in the same order, so they zip into entries
    auto entries = cc::zip(m.keys(), m.values()).to_vector();
    CHECK(entries.size() == 3);
    for (auto const& e : entries)
        CHECK((m.get(e.get<0>()) == e.get<1>()));

    // a temporary map is owned by its sequence
    CHECK(make_weights().values().sum_from(0.0f) == 4.0f);
    CHECK(make_weights().keys().contains(2));
    CHECK((cc::map<int, float>().values().count() == 0));

    // external iteration over the buckets
    int key_sum = 0;
    for (int k : m.keys())
        key_sum += k;
    CHECK(key_sum == 10);
}

TEST("sequence - set values")
{
    auto s = cc::set<int>();
    s.insert(4);
    s.insert(8);
    s.insert(15);

    CHECK(s.values().count() == 3);
    CHECK(s.values().contains(8));
    CHECK(!s.values().contains(9));
    CHECK(s.values().assert_non_empty().max() == 15);
    CHECK(s.values().filter([](int x) { return x % 2 == 0; }).count() == 2);
}

TEST("sequence - fixed_array, small_vector, fixed_vector")
{
    auto const fa = cc::fixed_array<int, 3>{4, 5, 6};
    static_assert(decltype(fa.values())::is_non_empty);
    CHECK(fa.values().sum() == 15);
    CHECK(fa.indices().contains(2));
    static_assert(!decltype(cc::fixed_array<int, 0>().values())::is_non_empty);

    auto sv = cc::small_vector<int, 4>();
    for (int i = 0; i < 10; ++i)
        sv.push_back(i);
    CHECK(sv.values().sum_from(0) == 45);
    CHECK(sv.values().skip(8).sum_from(0) == 17);

    auto fv = cc::fixed_vector<int, 8>();
    fv.push_back(1);
    fv.push_back(2);
    CHECK(fv.values().count() == 2);

    // a temporary fixed_vector keeps its elements inline, so the owning source addresses them by index
    auto make_fv = []
    {
        auto v = cc::fixed_vector<int, 8>();
        v.push_back(7);
        v.push_back(8);
        return v;
    };
    auto owned = make_fv().values();
    auto moved = cc::move(owned);
    CHECK(cc::move(moved).sum_from(0) == 15);
}

TEST("sequence - ringbuffer chains its segments")
{
    auto rb = cc::ringbuffer<int>();
    for (int i = 0; i < 4; ++i)
        rb.push_back(i);
    rb.push_front(-1);
    rb.push_front(-2);

    // the content wraps, so values() runs over both segments, front to back
    auto const all = rb.values().to_vector();
    CHECK(all.size() == 6);
    CHECK(all[0] == -2);
    CHECK(all[5] == 3);
    CHECK(rb.values().sum_from(0) == 3);
    CHECK(rb.indices().count() == 6);

    rb.values().each([](int& x) { x = 1; });
    CHECK(rb.values().sum_from(0) == 6);
}

TEST("sequence - strided_span")
{
    struct vertex
    {
        float x;
        float y;
    };
    auto vs = cc::vector<vertex>{{1, 2}, {3, 4}, {5, 6}};
    auto const ys = cc::strided_span<float>(&vs[0].y, vs.size(), isize(sizeof(vertex)));

    CHECK(ys.values().sum_from(0.0f) == 12.0f);
    CHECK(ys.values().skip(1).take(1).sum_from(0.0f) == 4.0f);
    CHECK(ys.values().find([](float y) { return y == 6.0f; }) == &vs[2].y);
}
