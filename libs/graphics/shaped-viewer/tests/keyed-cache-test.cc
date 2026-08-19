#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/impl/keyed_cache.hh>
#include <shaped-viewer/view/view_id.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the per-view caches' shared reclamation core.
// No GPU — the record is a plain struct and reclamation is driven purely by ticks and declared byte sizes.
// This is where "the same key across frames keeps its state" is pinned: sv::viewer needs a window, a swapchain and a
// ray-tracing device, so the policy is only reachable headless as this type.

namespace
{
struct payload
{
    int value = 0;

    /// Part of the record the hook never touches — a view's camera, in the real thing.
    int identity = 0;
};

using cache = sv::impl::keyed_cache<sv::view_id, payload>;

sv::view_id id_of(char const* name)
{
    return sv::view_id::from_string(name);
}

/// A release hook recording which keys it saw, so a test can check a payload is released exactly once.
/// Clearing what it released is the hook's job, not the cache's — that is what lets a record keep an identity the
/// payload does not own.
struct release_log
{
    cc::vector<sv::view_id> keys;

    void operator()(sv::view_id key, payload& p)
    {
        keys.push_back(key);
        CHECK(p.value != 0); // never called on an already-released record
        p.value = 0;
    }
};
} // namespace

TEST("sv - keyed_cache keeps state across frames under the same key")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_entry = 2});

    auto const a = id_of("main");
    c.begin_frame(1);
    c.get_or_create(a).value = 42;

    // Re-created every frame from the same key: the record is the one from the first frame, never a fresh default.
    for (auto tick = u64(2); tick <= 10; ++tick)
    {
        c.begin_frame(tick);
        CHECK(c.get_or_create(a).value == 42);
    }
    CHECK(c.count() == 1);
}

TEST("sv - keyed_cache drops an entry past the idle threshold")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_entry = 1}); // drop after > 1 idle frame

    auto const a = id_of("main");
    c.begin_frame(1);
    c.get_or_create(a).value = 7;

    c.begin_frame(2); // idle 0
    CHECK(c.contains(a));
    c.begin_frame(3); // idle 1 — still within the threshold
    CHECK(c.contains(a));
    c.begin_frame(4); // idle 2 — past it
    CHECK(!c.contains(a));

    // Coming back mints a fresh record rather than resurrecting the old one.
    CHECK(c.get_or_create(a).value == 0);
}

TEST("sv - keyed_cache demotion releases the payload but keeps the entry")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_payload = 1, .max_idle_frames_entry = 4});

    auto const a = id_of("main");
    auto log = release_log{};

    c.begin_frame(1, log);
    c.get_or_create(a).value = 5;
    c.set_payload_bytes(a, 1024);
    CHECK(c.payload_bytes() == 1024);

    c.begin_frame(2, log); // idle 0
    c.begin_frame(3, log); // idle 1 — within the payload threshold
    CHECK(log.keys.empty());
    CHECK(c.payload_bytes() == 1024);

    c.begin_frame(4, log); // idle 2 — the payload goes, the identity stays
    CHECK(log.keys.size() == 1);
    CHECK(log.keys[0] == a);
    CHECK(c.contains(a));
    CHECK(c.count() == 1);
    CHECK(c.payload_bytes() == 0);
    CHECK(c.peek_ptr(a)->value == 0); // the hook cleared what it released

    // Nothing to release a second time, whether it idles further or is finally evicted.
    c.begin_frame(5, log);
    c.begin_frame(6, log); // idle 4 — the entry threshold is not passed yet
    CHECK(c.contains(a));
    c.begin_frame(7, log); // idle 5 — past it
    CHECK(!c.contains(a));
    CHECK(log.keys.size() == 1);
}

// The invariant that lets one record hold both halves of a view: releasing the expensive part must not take the cheap one.
// A camera surviving its own accumulator by ~180 frames is the whole reason the two idle tiers exist, so a demotion that
// reset the record would quietly undo it — the view would come back framed at the origin instead of where it was left.
TEST("sv - keyed_cache demotion leaves what the hook did not clear")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_payload = 1, .max_idle_frames_entry = 100});

    auto const a = id_of("main");
    auto log = release_log{};

    c.begin_frame(1, log);
    c.get_or_create(a) = {.value = 5, .identity = 99};
    c.set_payload_bytes(a, 1024);

    c.begin_frame(2, log);
    c.begin_frame(3, log);
    c.begin_frame(4, log); // past the payload threshold
    REQUIRE(log.keys.size() == 1);

    CHECK(c.peek_ptr(a)->value == 0);     // the payload went
    CHECK(c.peek_ptr(a)->identity == 99); // the identity stayed
}

TEST("sv - keyed_cache never reclaims this tick's working set")
{
    auto c = cache{};
    c.set_limits({.max_entries = 1});

    auto const a = id_of("a");
    auto const b = id_of("b");
    c.begin_frame(1);
    c.get_or_create(a).value = 1;
    c.get_or_create(b).value = 2;

    // Both were used on tick 1, so neither may be dropped even though the cache is over its entry budget.
    c.begin_frame(2);
    CHECK(c.count() == 2);

    (void)c.get_or_create(a); // a is used on tick 2, b is not

    c.begin_frame(3); // now b is the least-recently-used candidate outside the working set
    CHECK(c.contains(a));
    CHECK(!c.contains(b));
}

TEST("sv - keyed_cache releases payloads least-recently-used first over budget")
{
    auto c = cache{};
    c.set_limits({.max_payload_bytes = 150});

    auto const a = id_of("a");
    auto const b = id_of("b");
    auto log = release_log{};

    c.begin_frame(1, log);
    c.get_or_create(a).value = 1;
    c.set_payload_bytes(a, 100);
    c.get_or_create(b).value = 2;
    c.set_payload_bytes(b, 100);

    c.begin_frame(2, log); // over budget, but both are this tick's working set
    CHECK(c.payload_bytes() == 200);
    CHECK(log.keys.empty());

    (void)c.get_or_create(a);

    c.begin_frame(3, log);
    CHECK(log.keys.size() == 1);
    CHECK(log.keys[0] == b);
    CHECK(c.payload_bytes() == 100);
    CHECK(c.count() == 2); // b keeps its identity — only the payload went
}

TEST("sv - keyed_cache peek does not keep a view alive")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_entry = 1});

    auto const a = id_of("main");
    c.begin_frame(1);
    c.get_or_create(a).value = 3;

    for (auto tick = u64(2); tick <= 4; ++tick)
    {
        c.begin_frame(tick);
        (void)c.peek_ptr(a); // a hit-test must not count as use
    }
    CHECK(!c.contains(a));
}

TEST("sv - keyed_cache get and peek are the checked forms of get_ptr and peek_ptr")
{
    auto c = cache{};
    c.set_limits({.max_idle_frames_entry = 3});

    auto const a = id_of("main");
    c.begin_frame(1);
    c.get_or_create(a).value = 11;

    CHECK(&c.get(a) == c.get_ptr(a));
    CHECK(&c.peek(a) == c.peek_ptr(a));
    CHECK(c.get(a).value == 11);

    // peek reads without counting as use, exactly as peek_ptr does, so a checked read cannot keep a record alive.
    for (auto tick = u64(2); tick <= 5; ++tick)
    {
        c.begin_frame(tick);
        CHECK(c.peek(a).value == 11);
    }
    c.begin_frame(6);
    CHECK(!c.contains(a));
}

TEST("sv - keyed_cache clear releases every payload exactly once")
{
    auto c = cache{};
    auto log = release_log{};

    c.begin_frame(1, log);
    for (auto const* name : {"a", "b", "c"})
    {
        auto const key = id_of(name);
        c.get_or_create(key).value = 1;
        c.set_payload_bytes(key, 10);
    }
    CHECK(c.count() == 3);
    CHECK(c.payload_bytes() == 30);

    c.clear(log);
    CHECK(log.keys.size() == 3);
    CHECK(c.count() == 0);
    CHECK(c.payload_bytes() == 0);
}
