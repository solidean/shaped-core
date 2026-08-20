#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/record/overhead.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

namespace
{
cc::rec::recording capture(cc::function_ref<void()> body)
{
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        body();
        cc::rec::flush_blocking();
    }
    return rl.take();
}
} // namespace

//
// Ids
//

REC_TEST("record/trace - ids are unique, cheap and need no registry")
{
    rec_fixture const fixture(deterministic_config());

    auto const a = cc::rec::new_trace_id();
    auto const b = cc::rec::new_trace_id();
    CHECK(a != b);
    CHECK(a != cc::rec::trace_id::none);

    // Two threads mint from disjoint ranges, because the thread's identity is in the high bits.
    cc::rec::trace_id from_worker = cc::rec::trace_id::none;
    std::thread worker([&] { from_worker = cc::rec::new_trace_id(); });
    worker.join();

    CHECK(from_worker != a);
    CHECK(from_worker != b);

    // Nothing was registered, so nothing has to be released.
    CHECK(cc::rec::current_trace_id() == cc::rec::trace_id::none);
}

REC_TEST("record/trace - a scope attributes what is recorded under it, and restores what was there")
{
    rec_fixture const fixture(deterministic_config());

    auto const outer = cc::rec::new_trace_id();
    auto const inner = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            CC_RECORD_MARK("before");
            {
                CC_TRACE_SCOPE(outer);
                CHECK(cc::rec::current_trace_id() == outer);
                CC_RECORD_MARK("in-outer");
                {
                    CC_TRACE_SCOPE(inner);
                    CHECK(cc::rec::current_trace_id() == inner);
                    CC_RECORD_MARK("in-inner");
                }
                CHECK(cc::rec::current_trace_id() == outer);
                CC_RECORD_MARK("back-in-outer");
            }
            CC_RECORD_MARK("after");
        });

    CHECK(cc::rec::current_trace_id() == cc::rec::trace_id::none);

    auto const of_outer = r.from_trace(outer);
    CHECK(of_outer.count("in-outer") == 1);
    CHECK(of_outer.count("back-in-outer") == 1);
    CHECK(of_outer.count("in-inner") == 0); // the inner scope took over
    CHECK(of_outer.count("before") == 0);
    CHECK(of_outer.count("after") == 0);

    auto const of_inner = r.from_trace(inner);
    CHECK(of_inner.count("in-inner") == 1);
    CHECK(of_inner.count("in-outer") == 0);
}

REC_TEST("record/trace - a trace spans threads, since an id is just a value")
{
    rec_fixture const fixture(deterministic_config());

    auto const id = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            CC_TRACE_SCOPE(id);
            CC_RECORD_MARK("on-main");

            // Passing the id by hand is the synchronous way to carry a trace across a thread.
            // Carrying it automatically is what the ambient deltas will add.
            std::thread worker(
                [&]
                {
                    CC_TRACE_SCOPE(id);
                    CC_RECORD_MARK("on-worker");
                });
            worker.join();
        });

    auto const of_id = r.from_trace(id);
    CHECK(of_id.count("on-main") == 1);
    CHECK(of_id.count("on-worker") == 1);
}

//
// Relations
//

REC_TEST("record/trace - relations are facts, and a late one is the same fact")
{
    rec_fixture const fixture(deterministic_config());

    auto const request = cc::rec::new_trace_id();
    auto const fetch = cc::rec::new_trace_id();
    auto const other = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            cc::rec::record_relation(request, cc::rec::relation_kind::parent_of, fetch);

            // Discovered only after the work: both requests resolved to one cache key.
            // Nothing has to be revisited, because nothing holds a graph.
            cc::rec::record_relation(fetch, cc::rec::relation_kind::same_key_as, other);
        });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 2);

    CHECK(edges[0].from == request);
    CHECK(edges[0].to == fetch);
    CHECK(edges[0].kind == cc::rec::relation_kind::parent_of);

    CHECK(edges[1].from == fetch);
    CHECK(edges[1].to == other);
    CHECK(edges[1].kind == cc::rec::relation_kind::same_key_as);
    CHECK(edges[1].cycles >= edges[0].cycles);
}

REC_TEST("record/trace - ids and relations survive a round trip through bytes")
{
    rec_fixture const fixture(deterministic_config());

    auto const parent = cc::rec::new_trace_id();
    auto const child = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            cc::rec::record_relation(parent, cc::rec::relation_kind::parent_of, child);
            CC_TRACE_SCOPE(child);
            CC_RECORD_MARK("inside-child");
        });

    auto loaded = cc::rec::deserialize(cc::rec::serialize(r));
    REQUIRE(loaded.has_value());

    auto const edges = loaded.value().events().trace_relations();
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].from == parent);
    CHECK(edges[0].to == child);

    CHECK(loaded.value().events().from_trace(child).count("inside-child") == 1);
}

REC_TEST("record/trace - tracing gates on its own category")
{
    rec_fixture const fixture(deterministic_config());

    auto const id = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            scoped_domain_mask const restore(cc::rec::g_default_domain);
            cc::rec::g_default_domain.set_enabled(cc::rec::category::tracing, false);

            CC_TRACE_SCOPE(id);
            cc::rec::record_relation(id, cc::rec::relation_kind::follows, id);
            CC_RECORD_MARK("still-recorded"); // a different category, so it still lands
        });

    CHECK(r.trace_relations().empty());
    CHECK(r.count_of_kind(cc::rec::event_kind::trace_scope) == 0);
    CHECK(r.count("still-recorded") == 1);
}

//
// What the recorder costs
//

REC_TEST("record/overhead - the model is measured, not guessed")
{
    rec_fixture const fixture(deterministic_config());

    // A stand-in model says so, which is the distinction a caller needs; measuring is what flips it.
    scoped_overhead const restore_model({.fixed_cycles = 40, .cycles_per_byte = 0.5, .disabled_cycles = 2});
    CHECK(!cc::rec::overhead().is_measured);

    auto const measured = cc::rec::measure_overhead();
    CHECK(measured.is_measured);
    CHECK(cc::rec::overhead().is_measured);

    // A recording site costs something, and rather less than a microsecond.
    CHECK(measured.fixed_cycles > 0);
    CHECK(measured.fixed_cycles < 5000);

    // A disabled site is a load and a test, so it is far cheaper than an enabled one.
    CHECK(measured.disabled_cycles >= 0);
    CHECK(measured.disabled_cycles < measured.fixed_cycles);

    // Bytes cost something, and never negative — a fit that came out below zero is reported as zero.
    CHECK(measured.cycles_per_byte >= 0);
}

REC_TEST("record/overhead - a recording estimates what it cost to make")
{
    rec_fixture const fixture(deterministic_config());

    scoped_overhead const model({.fixed_cycles = 100, .cycles_per_byte = 1, .disabled_cycles = 2, .is_measured = true});

    auto const r = capture(
        []
        {
            for (int i = 0; i < 100; ++i)
                CC_RECORD_MARK("tick");
        });

    // Every marker is a zero-payload event, so the estimate is the fixed cost times the count.
    auto const events = r.event_count();
    REQUIRE(events >= 100);
    CHECK(r.estimated_overhead_cycles() >= 100 * 100.0);

    // A ratio is only meaningful once time has passed, and it is a fraction rather than a percentage.
    auto const ratio = r.estimated_overhead_ratio();
    CHECK(ratio > 0);
    CHECK(ratio < 1000); // sane rather than exact; a tight loop of nothing but markers is mostly overhead

    // An empty recording spans no time, and says zero rather than dividing by it.
    CHECK(cc::rec::recording{}.estimated_overhead_ratio() == 0.0);
    CHECK(cc::rec::recording{}.estimated_overhead_cycles() == 0.0);
}

REC_TEST("record/overhead - an event that measured itself beats the model")
{
    rec_fixture const fixture(deterministic_config());

    scoped_overhead const model({.fixed_cycles = 1, .cycles_per_byte = 0, .disabled_cycles = 0, .is_measured = true});

    // A stacktrace-enriched event carries its own end timestamp, because capture costs orders of magnitude more than
    // the model would predict.
    scoped_domain_mask const restore(cc::rec::g_default_domain);
    cc::rec::g_default_domain.set_captures_stacktrace(cc::rec::level::info, true);

    auto const r = capture([] { CC_LOG_INFO("with a stack"); });

    // Two events at a modelled cost of 1 each; the stacktrace one reports its real cost instead.
    CHECK(r.count_of_kind(cc::rec::event_kind::value) >= 1);
    CHECK(r.estimated_overhead_cycles() > 2.0);
}
