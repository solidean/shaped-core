#include "cache_fixture.hh"

#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

namespace
{
/// A compute that counts its own invocations and resolves only when the test says so.
///
/// Manual rather than lazy, so a test can hold every caller inside one compute and check that the SECOND acquire
/// joined rather than started its own — which a compute that finished immediately could never show.
struct gated_compute
{
    cc::shared_async<blob> node = cc::make_async_manual<blob>();
    int calls = 0;

    cc::shared_async<blob> operator()()
    {
        ++calls;
        return node;
    }

    void resolve(cc::string_view text) { node->push_value(make_blob(text)); }
    void fail() { node->push_error(cc::async_error::make_error(cc::any_error(cc::string("compute failed")))); }
};
} // namespace

TEST("bcache acquire computes once and shares the result")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "shared-compute");
    auto compute = gated_compute();

    // Both issued before anything is driven, so both are genuinely concurrent as far as the table is concerned.
    auto const a = f.cache().acquire(key, [&] { return compute(); });
    auto const b = f.cache().acquire(key, [&] { return compute(); });

    // The joiner shares the very same node, which is what makes "one pipeline" observable rather than inferred.
    CHECK(a.get() == b.get());
    CHECK(f.cache().get_stats().singleflight_joins == 1);

    f.idle();
    CHECK(compute.calls <= 1); // never twice, whatever the driving order turned out to be

    compute.resolve("computed once");
    CHECK(blob_text(f.settle(a)) == "computed once");
    CHECK(blob_text(f.settle(b)) == "computed once");
    CHECK(compute.calls == 1);
}

TEST("bcache acquire serves a second caller from storage once the first has finished")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "then-cached");

    auto first = gated_compute();
    auto const a = f.cache().acquire(key, [&] { return first(); });
    f.idle();
    first.resolve("from compute");
    CHECK(blob_text(f.settle(a)) == "from compute");

    // The slot is released by the terminal step, so nothing is left behind for the next caller to join.
    f.idle();
    CHECK(f.cache().get_stats().computes_started == 1);

    auto second = gated_compute();
    auto const b = f.cache().acquire(key, [&] { return second(); });
    CHECK(blob_text(f.settle(b)) == "from compute");
    CHECK(second.calls == 0); // a hit, so the callback is never even asked for
}

TEST("bcache acquire runs one compute per distinct key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto one = gated_compute();
    auto two = gated_compute();

    auto const a = f.cache().acquire(key_of("shader", "one"), [&] { return one(); });
    auto const b = f.cache().acquire(key_of("shader", "two"), [&] { return two(); });
    CHECK(a.get() != b.get());

    f.idle();
    one.resolve("first");
    two.resolve("second");

    CHECK(blob_text(f.settle(a)) == "first");
    CHECK(blob_text(f.settle(b)) == "second");
    CHECK(f.cache().get_stats().singleflight_joins == 0);
}

TEST("bcache acquire lets a later caller retry after a failed compute")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "fails-first");

    auto failing = gated_compute();
    auto const a = f.cache().acquire(key, [&] { return failing(); });
    f.idle();
    failing.fail();

    f.drive_until([&] { return a->is_ready(); });
    CHECK(a->has_error()); // a compute failure is the ONE thing acquire propagates

    // Nothing was stored, and the slot is gone — so a later attempt is a fresh one rather than the same failure.
    auto succeeding = gated_compute();
    auto const b = f.cache().acquire(key, [&] { return succeeding(); });
    CHECK(b.get() != a.get());
    f.idle();
    succeeding.resolve("second attempt");
    CHECK(blob_text(f.settle(b)) == "second attempt");
}

TEST("bcache acquire stores what it computed")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "written-back");

    auto compute = gated_compute();
    auto const a = f.cache().acquire(key, [&] { return compute(); });
    f.idle();
    compute.resolve("persist me");
    f.settle_only(a);

    // The store is fire-and-forget, so it lands a cycle or two after the caller already has its value.
    f.idle();
    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    CHECK(blob_text(hit.value().data) == "persist me");
}

TEST("bcache acquire takes a plain blocking callback too")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "sync-compute");

    auto calls = 0;
    auto const a = f.cache().acquire(key,
                                     [&]
                                     {
                                         ++calls;
                                         return make_blob("synchronously produced");
                                     });

    CHECK(blob_text(f.settle(a)) == "synchronously produced");
    CHECK(calls == 1);

    f.idle();
    auto const b = f.cache().acquire(key, [&] { return make_blob("never runs"); });
    CHECK(blob_text(f.settle(b)) == "synchronously produced");
    CHECK(calls == 1);
}

TEST("bcache acquire releases a slot without disturbing a successor under the same key")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The ABA case.
    // One operation finishes; another under the SAME key starts; the first's release must not erase
    // the second's registration, or a third caller would start a second compute rather than joining.
    auto f = cache_fixture();
    auto const key = key_of("shader", "aba");

    auto first = gated_compute();
    auto const a = f.cache().acquire(key, [&] { return first(); });
    f.idle();
    first.resolve("round one");
    f.settle_only(a);
    f.idle();

    CHECK(f.cache().get_stats().computes_started == 1);

    // A bumped version, so this really is a second pipeline rather than the same one still registered.
    auto const other = key_of("shader", "aba", 2);
    auto second = gated_compute();
    auto const b = f.cache().acquire(other, [&] { return second(); });
    f.idle();

    auto third = gated_compute();
    auto const c = f.cache().acquire(other, [&] { return third(); });
    CHECK(b.get() == c.get()); // joined, not restarted
    CHECK(third.calls == 0);

    second.resolve("round two");
    CHECK(blob_text(f.settle(b)) == "round two");
    CHECK(blob_text(f.settle(c)) == "round two");
}

TEST("bcache acquire forgets an operation nobody is waiting on any more")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The table holds a WEAK reference on purpose: an owning one would keep every blob it ever handed out alive, making the disk cache an unbounded memory cache as a side effect.
    auto f = cache_fixture();
    auto const key = key_of("shader", "dropped");

    {
        auto compute = gated_compute();
        auto const a = f.cache().acquire(key, [&] { return compute(); });
        f.idle();
        compute.resolve("done");
        f.settle_only(a);
    }
    f.idle();

    auto again = gated_compute();
    auto const b = f.cache().acquire(key, [&] { return again(); });
    CHECK(blob_text(f.settle(b)) == "done"); // served from storage, not from a retained value
    CHECK(again.calls == 0);
}
