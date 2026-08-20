#include <clean-core/bytes/hash128.hh>
#include <nexus/test.hh>
#include <shaped-graphics/fwd.hh> // sg::epoch
#include <shaped-viewer/resources/impl/lru_pool.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the managers' LRU/budget core.
// No GPU — the record type is a plain int and eviction is driven purely by epochs and byte sizes.

namespace
{
enum class test_id : u32
{
    invalid = u32(-1) // matches the production ids: the pool mints from 0 upward, so 0 is a real id
};

// insert() / find_by_hash() are protected (only a concrete manager mints records); expose them for the test.
struct test_pool : sv::impl::lru_pool<test_id, int>
{
    using sv::impl::lru_pool<test_id, int>::insert;
    using sv::impl::lru_pool<test_id, int>::find_by_hash;
};
} // namespace

TEST("sv - lru_pool basic insert / get / evict")
{
    auto p = test_pool{};
    auto const a = p.insert(/*hash*/ cc::hash128{.low = 0xA1}, /*record*/ 10, /*size*/ 100);
    auto const b = p.insert(/*hash*/ cc::hash128{.low = 0xB2}, /*record*/ 20, /*size*/ 50);

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

TEST("sv - lru_pool content-addressed find_by_hash")
{
    auto p = test_pool{};
    auto const a = p.insert(/*hash*/ cc::hash128{.low = 0x1234}, /*record*/ 42, /*size*/ 8);

    CHECK(p.find_by_hash(cc::hash128{.low = 0x1234}).has_value());
    CHECK(p.find_by_hash(cc::hash128{.low = 0x1234}).value() == a);
    CHECK(!p.find_by_hash(cc::hash128{.low = 0x9999}).has_value()); // never inserted

    CHECK(p.evict(a));
    CHECK(!p.find_by_hash(cc::hash128{.low = 0x1234}).has_value()); // index cleared on eviction
}

TEST("sv - lru_pool keys on the whole 128-bit hash")
{
    // The index hashes a cc::hash128 down to its low limb, so two keys sharing a low limb collide in the bucket.
    // They must still resolve to their own records — the pool compares full keys, never truncated ones.
    auto p = test_pool{};
    auto const a = p.insert(cc::hash128{.low = 0x77, .high = 0x1}, /*record*/ 1, /*size*/ 8);
    auto const b = p.insert(cc::hash128{.low = 0x77, .high = 0x2}, /*record*/ 2, /*size*/ 8);

    CHECK(a != b);
    CHECK(p.count() == 2);
    CHECK(p.find_by_hash(cc::hash128{.low = 0x77, .high = 0x1}).value() == a);
    CHECK(p.find_by_hash(cc::hash128{.low = 0x77, .high = 0x2}).value() == b);
    CHECK(!p.find_by_hash(cc::hash128{.low = 0x77, .high = 0x3}).has_value());
}

TEST("sv - lru_pool evicts least-recently-used over budget")
{
    auto p = test_pool{};
    p.set_limits(/*max_bytes*/ 150, /*max_idle_epochs*/ -1);

    auto const a = p.insert(/*hash*/ cc::hash128{.low = 0xA}, /*record*/ 1, /*size*/ 100);
    auto const b = p.insert(/*hash*/ cc::hash128{.low = 0xB}, /*record*/ 2, /*size*/ 100);
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

    auto const a = p.insert(/*hash*/ cc::hash128{.low = 0xA}, /*record*/ 1, /*size*/ 10); // last used epoch 0

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

    auto const a = p.insert(/*hash*/ cc::hash128{.low = 0xA}, /*record*/ 1, /*size*/ 10);
    for (auto e = 1; e <= 5; ++e)
    {
        p.begin_frame(sg::epoch(e));
        (void)p.get(a); // touched every frame
        CHECK(p.contains(a));
    }
}
