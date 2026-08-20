#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/vector.hh>
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

/// A relation type defined next to the code that records it, which is the normal way.
constexpr cc::rec::relation_type relation_reads_from = {
    .name = "reads_from",
    .inverse_name = "read_by",
};
} // namespace

//
// Ids and scopes
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

REC_TEST("record/trace - a scope mints and names its trace, and restores what was there")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::trace_id outer_id = cc::rec::trace_id::none;
    cc::rec::trace_id inner_id = cc::rec::trace_id::none;

    auto const r = capture(
        [&]
        {
            CC_RECORD_MARK("before");
            {
                // Naming it is the point: a bare id is an opaque number, useless in a viewer.
                CC_TRACE_SCOPE("handle-request");
                outer_id = cc::rec::current_trace_id();
                CHECK(outer_id != cc::rec::trace_id::none);
                CC_RECORD_MARK("in-outer");
                {
                    CC_TRACE_SCOPE("fetch-asset");
                    inner_id = cc::rec::current_trace_id();
                    CC_RECORD_MARK("in-inner");
                }
                CHECK(cc::rec::current_trace_id() == outer_id);
                CC_RECORD_MARK("back-in-outer");
            }
            CC_RECORD_MARK("after");
        });

    CHECK(outer_id != inner_id);
    CHECK(cc::rec::current_trace_id() == cc::rec::trace_id::none);

    auto const of_outer = r.from_trace(outer_id);
    CHECK(of_outer.count("in-outer") == 1);
    CHECK(of_outer.count("back-in-outer") == 1);
    CHECK(of_outer.count("in-inner") == 0); // the inner scope took over
    CHECK(of_outer.count("before") == 0);
    CHECK(of_outer.count("after") == 0);

    CHECK(r.from_trace(inner_id).count("in-inner") == 1);

    // The scope's name reaches the stream, which is how a reader learns what a trace id is called.
    CHECK(r.count("handle-request") == 1);
    CHECK(r.count("fetch-asset") == 1);
}

REC_TEST("record/trace - a trace can carry an id that came from somewhere else")
{
    rec_fixture const fixture(deterministic_config());

    // A request id off the wire, a job id from a queue: minting is the default, not the only way.
    auto const wire_id = cc::rec::trace_id(0x1234'5678'9ABC'DEF0ull);

    auto const r = capture(
        [&]
        {
            CC_TRACE_SCOPE_WITH_ID("inbound-request", wire_id);
            CHECK(cc::rec::current_trace_id() == wire_id);
            CC_RECORD_MARK("handled");
        });

    CHECK(r.from_trace(wire_id).count("handled") == 1);
}

REC_TEST("record/trace - a trace spans threads, since an id is just a value")
{
    rec_fixture const fixture(deterministic_config());

    auto const id = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            CC_TRACE_SCOPE_WITH_ID("job", id);
            CC_RECORD_MARK("on-main");

            // Passing the id by hand is the synchronous way to carry a trace across a thread.
            // Carrying it automatically is what folding trace scopes into cc::async's ambient chain will add.
            std::thread worker(
                [&]
                {
                    CC_TRACE_SCOPE_WITH_ID("job", id);
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

REC_TEST("record/trace - a relation says what it means, not just who it links")
{
    rec_fixture const fixture(deterministic_config());

    auto const request = cc::rec::new_trace_id();
    auto const fetch = cc::rec::new_trace_id();

    auto const r = capture([&] { CC_RECORD_RELATION(cc::rec::relation_parent_of, request, fetch); });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 1);
    REQUIRE(edges[0].type != nullptr);

    // The type is a static object, so the edge carries semantics a consumer can act on rather than an opaque tag.
    CHECK(cc::string_view(edges[0].type->name) == "parent_of");
    CHECK(cc::string_view(edges[0].type->inverse_name) == "child_of");
    CHECK(edges[0].type->is_transitive);
    CHECK(!edges[0].type->is_symmetric);
    CHECK(!edges[0].type->is_equivalence);

    CHECK(edges[0].subject() == request);
    REQUIRE(edges[0].objects().size() == 1);
    CHECK(edges[0].objects()[0] == fetch);
}

REC_TEST("record/trace - a relation is n-ary, and the first member is the subject")
{
    rec_fixture const fixture(deterministic_config());

    auto const join = cc::rec::new_trace_id();
    auto const a = cc::rec::new_trace_id();
    auto const b = cc::rec::new_trace_id();
    auto const c = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            // A fan-in: one result, three causes.
            // Decomposed into pairs this would lose that they were a group.
            CC_RECORD_RELATION(cc::rec::relation_caused_by, join, a, b, c);

            // A symmetric one, where every member is a peer and the order carries nothing.
            CC_RECORD_RELATION(cc::rec::relation_same_key_as, a, b, c);
        });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 2);

    CHECK(edges[0].members.size() == 4);
    CHECK(edges[0].subject() == join);
    CHECK(edges[0].objects().size() == 3);
    CHECK(edges[0].objects()[2] == c);

    CHECK(edges[1].members.size() == 3);
    CHECK(edges[1].type->is_symmetric);

    // An equivalence is the one flag a reconstruction can act on directly: these ids may be merged.
    CHECK(edges[1].type->is_equivalence);
}

REC_TEST("record/trace - a relation type defined next to the code that uses it works the same")
{
    rec_fixture const fixture(deterministic_config());

    auto const shader = cc::rec::new_trace_id();
    auto const source = cc::rec::new_trace_id();

    auto const r = capture([&] { CC_RECORD_RELATION(relation_reads_from, shader, source); });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 1);
    CHECK(cc::string_view(edges[0].type->name) == "reads_from");
    CHECK(cc::string_view(edges[0].type->inverse_name) == "read_by");
}

REC_TEST("record/trace - a member list only known at runtime records the same edge")
{
    rec_fixture const fixture(deterministic_config());

    cc::vector<cc::rec::trace_id> members;
    for (int i = 0; i < 5; ++i)
        members.push_back(cc::rec::new_trace_id());

    auto const r = capture([&] { CC_RECORD_RELATION_MANY(cc::rec::relation_same_key_as, members); });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 1);
    REQUIRE(edges[0].members.size() == 5);
    CHECK(edges[0].members[4] == members[4]);
}

REC_TEST("record/trace - a late relation is the same fact")
{
    rec_fixture const fixture(deterministic_config());

    auto const first = cc::rec::new_trace_id();
    auto const second = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            CC_RECORD_MARK("work-happens");

            // Discovered only afterwards: both resolved to one cache key.
            // Nothing has to be revisited, because nothing holds a graph.
            CC_RECORD_RELATION(cc::rec::relation_same_key_as, first, second);
        });

    auto const edges = r.trace_relations();
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].type->is_equivalence);
}

REC_TEST("record/trace - ids, names and relations survive a round trip through bytes")
{
    rec_fixture const fixture(deterministic_config());

    auto const parent = cc::rec::new_trace_id();
    cc::rec::trace_id child = cc::rec::trace_id::none;

    auto const r = capture(
        [&]
        {
            CC_TRACE_SCOPE("child-work");
            child = cc::rec::current_trace_id();
            CC_RECORD_RELATION(cc::rec::relation_parent_of, parent, child);
            CC_RECORD_MARK("inside-child");
        });

    auto loaded = cc::rec::deserialize(cc::rec::serialize(r));
    REQUIRE(loaded.has_value());

    auto const edges = loaded.value().events().trace_relations();
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].subject() == parent);
    CHECK(edges[0].objects()[0] == child);

    // The relation TYPE travelled by value, so a reader that never linked against this binary still knows what the
    // edge means and which way it reads.
    REQUIRE(edges[0].type != nullptr);
    CHECK(cc::string_view(edges[0].type->name) == "parent_of");
    CHECK(cc::string_view(edges[0].type->inverse_name) == "child_of");
    CHECK(edges[0].type->is_transitive);

    CHECK(loaded.value().events().from_trace(child).count("inside-child") == 1);
    CHECK(loaded.value().events().count("child-work") == 1);
}

REC_TEST("record/trace - tracing gates on its own category")
{
    rec_fixture const fixture(deterministic_config());

    auto const a = cc::rec::new_trace_id();
    auto const b = cc::rec::new_trace_id();

    auto const r = capture(
        [&]
        {
            scoped_domain_mask const restore(cc::rec::g_default_domain);
            cc::rec::g_default_domain.set_enabled(cc::rec::category::tracing, false);

            CC_TRACE_SCOPE("silenced");
            CC_RECORD_RELATION(cc::rec::relation_follows, a, b);
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
    REQUIRE(r.event_count() >= 100);
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
