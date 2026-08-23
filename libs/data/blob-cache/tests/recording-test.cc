#include "cache_fixture.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace bcache;
using namespace bcache::test;

// Hit rate is blob-cache's primary health signal — the design's whole premise is that a storage failure IS a miss —
// and until the counters were mirrored into the recording the only way to see it was to hold a live cache and ask.
// These pin that mirroring, because a counter nobody can reach is the same as one that was never written.

namespace
{
/// Brings the recorder up for one test and takes it down again.
/// Declared FIRST in a test so it is destroyed LAST — a recording holds chunk references, and shutting the pool down
/// underneath one is a use-after-free rather than a diagnostic.
struct rec_fixture
{
    rec_fixture()
    {
        auto cfg = cc::rec::config{};
        cfg.threaded = false;
        cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
        cc::rec::initialize(cfg);
    }
    ~rec_fixture() { cc::rec::shutdown(); }

    rec_fixture(rec_fixture const&) = delete;
    rec_fixture& operator=(rec_fixture const&) = delete;
};

f64 accumulated(cc::rec::recording const& r, cc::string_view name)
{
    auto total = 0.0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::stat_accumulate && cc::string_view(e.name()) == name)
            {
                CHECK(cc::string_view(e.domain()->name()) == "bcache");
                total += e.field_as_double("value").value_or(0.0);
            }
    }
    return total;
}
} // namespace

TEST("bcache/recording - hits and misses are recorded as they are counted",
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    rec_fixture const fixture;

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    {
        auto f = cache_fixture();
        auto const key = key_of("shader", "recorded");

        // One miss, then a store, then one hit.
        CHECK(!f.settle(f.cache().get(key)).has_value());
        f.settle_only(f.cache().put(key, make_blob("bytes")));
        CHECK(f.settle(f.cache().get(key)).has_value());
    }

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const r = rl.take();

    // Exactly the counters the in-process cache_stats struct would report, from the one place both are written.
    CHECK(accumulated(r, "bcache.hits") == 1.0);
    CHECK(accumulated(r, "bcache.misses") == 1.0);
}
