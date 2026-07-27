#include <nexus/test.hh>
#include <shaped-graphics/fwd.hh> // sg::epoch
#include <shaped-viewer/resources/impl/lru_pool.hh>

// CPU-only tests for the managers' LRU/budget core. No GPU — the record type is a plain int and eviction is
// driven purely by epochs and byte sizes.

namespace
{
enum class test_id : cc::u32
{
    invalid = cc::u32(-1) // matches the production ids: the pool mints from 0 upward, so 0 is a real id
};

// insert() is protected (only a concrete manager mints records); expose it for the test.
struct test_pool : sv::impl::lru_pool<test_id, int>
{
    using sv::impl::lru_pool<test_id, int>::insert;
};
} // namespace

TEST("sv - lru_pool basic insert / get / evict")
{
    auto p = test_pool{};
    auto const a = p.insert(10, 100);
    auto const b = p.insert(20, 50);

    CHECK(p.count() == 2);
    CHECK(p.used_bytes() == 150);
    CHECK(p.contains(a));
    CHECK(p.get(a) == 10);
    CHECK(p.get(b) == 20);
    CHECK(p.get_ptr(test_id::invalid) == nullptr);

    CHECK(p.evict(a));
    CHECK(!p.evict(a)); // already gone
    CHECK(!p.contains(a));
    CHECK(p.count() == 1);
    CHECK(p.used_bytes() == 50);
}

TEST("sv - lru_pool evicts least-recently-used over budget")
{
    auto p = test_pool{};
    p.set_limits(/*max_bytes*/ 150, /*max_idle_epochs*/ -1);

    auto const a = p.insert(1, 100);
    auto const b = p.insert(2, 100);
    // Both were touched this (epoch 0) frame, so neither can be evicted yet even though we are over budget.
    CHECK(p.count() == 2);

    p.begin_frame(sg::epoch(1));
    (void)p.get(a); // a is used this frame; b is not

    p.begin_frame(sg::epoch(2)); // now b (least-recently-used, not in this frame's set) is dropped
    CHECK(p.contains(a));
    CHECK(!p.contains(b));
    CHECK(p.used_bytes() == 100);
}

TEST("sv - lru_pool evicts after the idle timeout")
{
    auto p = test_pool{};
    p.set_limits(/*max_bytes*/ 0, /*max_idle_epochs*/ 1); // unbounded bytes; evict after >1 idle epoch

    auto const a = p.insert(1, 10); // last used epoch 0

    p.begin_frame(sg::epoch(1)); // idle 0
    CHECK(p.contains(a));
    p.begin_frame(sg::epoch(2)); // idle 1 — still within the timeout
    CHECK(p.contains(a));
    p.begin_frame(sg::epoch(3)); // idle 2 — past the timeout
    CHECK(!p.contains(a));
}

TEST("sv - lru_pool keeps a resource touched every frame")
{
    auto p = test_pool{};
    p.set_limits(/*max_bytes*/ 0, /*max_idle_epochs*/ 0); // evict as soon as a frame passes unused

    auto const a = p.insert(1, 10);
    for (auto e = 1; e <= 5; ++e)
    {
        p.begin_frame(sg::epoch(e));
        (void)p.get(a); // touched every frame
        CHECK(p.contains(a));
    }
}
