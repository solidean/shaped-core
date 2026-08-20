#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

namespace
{
/// Captures everything recorded by `body`, narrowed to the thread that ran it.
///
/// This is the synchronous form of the assertion pattern: install a recording listener for a scope, run the code,
/// flush, and keep only what this thread wrote.
/// Narrowing by thread is what makes it reliable here; a test running asynchronously needs the ambient context
/// instead, which is what the nexus integration will use.
cc::rec::recording capture(cc::function_ref<void()> body)
{
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        body();
        cc::rec::flush_blocking();
    }
    return rl.take().from_thread(cc::current_thread_id());
}
} // namespace

//
// Filtering
//

REC_TEST("record/query - filtering synthesizes blocks that own their bytes")
{
    rec_fixture const fixture(deterministic_config());

    auto const all = capture(
        []
        {
            CC_RECORD_MARK("kept");
            CC_RECORD_MARK("dropped");
            CC_RECORD_MARK("kept");
        });

    auto const kept = all.filtered([](cc::rec::chunk_view const&, cc::rec::event_view const& e)
                                   { return cc::string_view(e.name()) == "kept"; });

    CHECK(kept.event_count() == 2);
    CHECK(kept.count("dropped") == 0);

    // The kept events are no longer contiguous, so the result owns a buffer instead of pinning the chunk.
    REQUIRE(kept.block_count() >= 1);
    for (auto const& b : kept.blocks())
    {
        CHECK(!b.source);
        CHECK(b.owned.size() > 0);
    }
}

REC_TEST("record/query - a filtered recording outlives the chunks it came from")
{
    auto cfg = deterministic_config();
    cfg.chunk_bytes = 8 * 1024;
    rec_fixture const fixture(cfg);

    auto const kept = capture(
                          []
                          {
                              for (int i = 0; i < 400; ++i)
                                  CC_RECORD("i", i);
                          })
                          .of_kind(cc::rec::event_kind::value);

    CHECK(kept.event_count() == 400);

    // Nothing here still references a chunk, so the pool can recycle everything and the recording stays readable.
    for (auto const& b : kept.blocks())
        CHECK(!b.source);

    CHECK(kept.values("i").size() == 400);
    CHECK(kept.first_value("i").value() == 0.0);
    CHECK(kept.last_value("i").value() == 399.0);
}

REC_TEST("record/query - narrowing by thread keeps only what this thread wrote")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        CC_RECORD_MARK("from-here");

        std::thread worker(
            []
            {
                cc::rec::set_current_thread_record_name("worker");
                CC_RECORD_MARK("from-there");
            });
        worker.join();

        cc::rec::flush_blocking();
    }

    auto const everything = rl.take();
    CHECK(everything.count("from-here") == 1);
    CHECK(everything.count("from-there") == 1);

    auto const mine = everything.from_thread(cc::current_thread_id());
    CHECK(mine.count("from-here") == 1);
    CHECK(mine.count("from-there") == 0);
}

REC_TEST("record/query - filtering by domain and by kind")
{
    rec_fixture const fixture(deterministic_config());

    auto const all = capture(
        []
        {
            CC_LOG_INFO("a message");
            CC_RECORD_MARK("a marker");
            CC_RECORD_STAT("a stat", cc::rec::unit_count, 3);
        });

    CHECK(all.of_kind(cc::rec::event_kind::log).count("a message") == 1);
    CHECK(all.of_kind(cc::rec::event_kind::log).count("a marker") == 0);
    CHECK(all.of_kind(cc::rec::event_kind::marker).event_count() == 1);
    CHECK(all.of_kind(cc::rec::event_kind::stat_snapshot).event_count() == 1);

    // The system's own bookkeeping sits in its own domain, so filtering to the default one drops it.
    auto const mine = all.from_domain(&cc::rec::g_default_domain);
    CHECK(mine.count("a message") == 1);
    CHECK(mine.count("record.chunk_acquired") == 0);
}

REC_TEST("record/query - slicing by cycle range")
{
    rec_fixture const fixture(deterministic_config());

    auto const all = capture(
        []
        {
            CC_RECORD_MARK("early");
            CC_RECORD_MARK("late");
        });

    auto const events = all.of_kind(cc::rec::event_kind::marker);
    REQUIRE(events.event_count() == 2);

    u64 first = 0;
    u64 second = 0;
    isize seen = 0;
    events.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (seen == 0)
                first = e.cycles;
            else
                second = e.cycles;
            ++seen;
        });

    CHECK(all.in_cycle_range(first, second).count("early") == 1);
    CHECK(all.in_cycle_range(first, second).count("late") == 0);
    CHECK(all.in_cycle_range(first, second + 1).count("late") == 1);
}

//
// Queries
//

REC_TEST("record/query - values, texts and messages read back by name")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD("attempts", 1);
            CC_RECORD("attempts", 2);
            CC_RECORD("attempts", 3);
            CC_RECORD("path", "assets/mesh.obj");
            CC_LOG_INFO("plain");
            CC_LOG_INFO("formatted {}", 42);
        });

    CHECK(r.count("attempts") == 3);
    CHECK(r.contains("attempts"));
    CHECK(!r.contains("nothing-recorded-this"));

    CHECK(r.first_value("attempts").value() == 1.0);
    CHECK(r.last_value("attempts").value() == 3.0);
    CHECK(r.values("attempts").size() == 3);
    CHECK(r.values("attempts")[1] == 2.0);

    // A name nobody recorded is absent rather than zero, which is the distinction a CHECK needs.
    CHECK(!r.first_value("attempts-that-never-happened").has_value());

    CHECK(r.first_text("path").value() == "assets/mesh.obj");

    // A message with no arguments keeps its text in the descriptor; one with arguments carries the formatted payload.
    auto const messages = r.messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0] == "plain");
    CHECK(messages[1] == "formatted 42");
}

REC_TEST("record/query - contains_in_order allows anything between, but not the wrong order")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_MARK("first");
            CC_RECORD_MARK("noise");
            CC_RECORD_MARK("second");
            CC_RECORD_MARK("noise");
            CC_RECORD_MARK("third");
        });

    CHECK(r.contains_in_order({"first", "second", "third"}));
    CHECK(r.contains_in_order({"first", "third"}));
    CHECK(!r.contains_in_order({"third", "first"}));
    CHECK(!r.contains_in_order({"first", "second", "fourth"}));

    // An empty expectation is trivially met.
    CHECK(r.contains_in_order({}));
}

REC_TEST("record/query - scopes come back as matched pairs, nested and timed")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            {
                CC_RECORD_SCOPE("outer");
                {
                    CC_RECORD_SCOPE("inner");
                }
                {
                    CC_RECORD_SCOPE("inner");
                }
            }
        });

    auto const all = r.scopes();
    REQUIRE(all.size() == 3);

    CHECK(all[0].name() == "outer");
    CHECK(all[0].depth == 0);
    CHECK(!all[0].is_open);

    // Two scopes sharing a name still pair correctly, because the depth separates them.
    CHECK(r.scopes("inner").size() == 2);
    for (auto const& s : r.scopes("inner"))
    {
        CHECK(s.depth == 1);
        CHECK(!s.is_open);
        CHECK(s.end_cycles >= s.begin_cycles);
    }

    // The outer scope spans both inner ones.
    auto const outer = r.scopes("outer");
    REQUIRE(outer.size() == 1);
    for (auto const& s : r.scopes("inner"))
    {
        CHECK(s.begin_cycles >= outer[0].begin_cycles);
        CHECK(s.end_cycles <= outer[0].end_cycles);
    }
}

REC_TEST("record/query - a scope whose close is missing is reported as open, not dropped")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_SCOPE_BEGIN("never-closed");
            CC_RECORD_MARK("inside");
            // No CC_RECORD_SCOPE_END: this is what a truncated stream looks like.
        });

    auto const all = r.scopes("never-closed");
    REQUIRE(all.size() == 1);
    CHECK(all[0].is_open);
    CHECK(all[0].duration_cycles() == 0);

    // Leave the thread's depth where the rest of the suite expects it.
    CC_RECORD_SCOPE_END("never-closed");
}

//
// Decimation
//

REC_TEST("record/query - decimating says what it no longer knows")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_MARK("old");
            CC_RECORD_MARK("old");
            CC_RECORD_MARK("recent");
        });

    // Cut just before the last marker.
    u64 cutoff = 0;
    auto const markers = r.of_kind(cc::rec::event_kind::marker);
    markers.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (cc::string_view(e.name()) == "recent")
                cutoff = e.cycles;
        });
    REQUIRE(cutoff > 0);

    auto const thinned = markers.decimated({.keep_from_cycles = cutoff});
    CHECK(thinned.count("old") == 0);
    CHECK(thinned.count("recent") == 1);

    // The whole point: the result reports the span it threw away rather than pretending nothing happened there.
    CHECK(thinned.count_of_kind(cc::rec::event_kind::dropped_span) == 1);

    isize reported = 0;
    thinned.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::dropped_span)
                return;
            reported = isize(e.field_as_int("events").value_or(-1));
            CHECK(e.field_as_int("begin_cycles").value() <= e.field_as_int("end_cycles").value());
        });
    CHECK(reported == 2); // both "old" markers, named as lost rather than silently absent
}

REC_TEST("record/query - decimating keeps the scope you are sitting inside")
{
    rec_fixture const fixture(deterministic_config());

    u64 cutoff = 0;
    auto const r = capture(
        [&]
        {
            CC_RECORD_SCOPE_BEGIN("surrounding");
            CC_RECORD_MARK("ancient");
            cutoff = cc::current_cycles();
            CC_RECORD_MARK("recent");
            CC_RECORD_SCOPE_END("surrounding");
        });

    auto const thinned = r.decimated({.keep_from_cycles = cutoff, .keep_open_scopes = true});

    // The old marker goes; the scope that spans the cutoff stays, or the trace loses the frame you wanted.
    CHECK(thinned.count("ancient") == 0);
    CHECK(thinned.count("recent") == 1);
    CHECK(thinned.scopes("surrounding").size() == 1);
}

//
// The assertion pattern
//

REC_TEST("record/query - the synchronous assertion pattern over code under test")
{
    rec_fixture const fixture(deterministic_config());

    // Stands in for the code under test: it records what it did instead of exposing a debug getter for it.
    auto const load_mesh = [](bool warm_cache)
    {
        CC_RECORD_SCOPE("load_mesh");
        CC_RECORD("vertex_count", 1024);

        if (warm_cache)
            CC_RECORD_MARK("cache-hit");
        else
        {
            CC_RECORD_MARK("cache-miss");
            CC_RECORD_ACCUM("bytes_read", cc::rec::unit_bytes, 65536);
        }
        CC_LOG_INFO("loaded {} vertices", 1024);
    };

    auto const cold = capture([&] { load_mesh(false); });

    CHECK(cold.contains("cache-miss"));
    CHECK(!cold.contains("cache-hit"));
    CHECK(cold.first_value("vertex_count").value() == 1024.0);
    CHECK(cold.first_value("bytes_read").value() == 65536.0);
    CHECK(cold.contains_in_order({"vertex_count", "cache-miss", "bytes_read"}));
    CHECK(cold.scopes("load_mesh").size() == 1);
    CHECK(cold.messages()[0] == "loaded 1024 vertices");

    auto const warm = capture([&] { load_mesh(true); });

    CHECK(warm.contains("cache-hit"));
    CHECK(!warm.contains("cache-miss"));
    CHECK(!warm.first_value("bytes_read").has_value());
}
