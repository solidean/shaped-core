#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/overhead.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
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
                CC_RECORD_ASYNC_SCOPE("handle-request");
                outer_id = cc::rec::current_trace_id();
                CHECK(outer_id != cc::rec::trace_id::none);
                CC_RECORD_MARK("in-outer");
                {
                    CC_RECORD_ASYNC_SCOPE("fetch-asset");
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
    // Twice per scope: a begin and an end share the site's name, and both carry the id.
    CHECK(r.count("handle-request") == 2);
    CHECK(r.count("fetch-asset") == 2);
    CHECK(r.count_of_kind(cc::rec::event_kind::async_scope_begin) == 2);
    CHECK(r.count_of_kind(cc::rec::event_kind::async_scope_end) == 2);
}

REC_TEST("record/trace - a trace can carry an id that came from somewhere else")
{
    rec_fixture const fixture(deterministic_config());

    // A request id off the wire, a job id from a queue: minting is the default, not the only way.
    auto const wire_id = cc::rec::trace_id(0x1234'5678'9ABC'DEF0ull);

    auto const r = capture(
        [&]
        {
            CC_RECORD_ASYNC_SCOPE_WITH_ID("inbound-request", wire_id);
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
            CC_RECORD_ASYNC_SCOPE_WITH_ID("job", id);
            CC_RECORD_MARK("on-main");

            // Passing the id by hand is the synchronous way to carry a trace across a thread.
            // Carrying it automatically is what folding trace scopes into cc::async's ambient chain will add.
            std::thread worker(
                [&]
                {
                    CC_RECORD_ASYNC_SCOPE_WITH_ID("job", id);
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
            CC_RECORD_ASYNC_SCOPE("child-work");
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
    CHECK(loaded.value().events().count("child-work") == 2); // the scope's begin and end
}

REC_TEST("record/trace - relations gate on tracing, and a trace scope on profiling")
{
    rec_fixture const fixture(deterministic_config());

    auto const a = cc::rec::new_trace_id();
    auto const b = cc::rec::new_trace_id();

    // The fold split what used to be one gate: a trace scope IS an async scope, so it answers to profiling, and only
    // the relation edges are tracing's.
    auto const no_tracing = capture(
        [&]
        {
            scoped_domain_mask const restore(cc::rec::g_default_domain);
            cc::rec::g_default_domain.set_enabled(cc::rec::category::tracing, false);

            CC_RECORD_ASYNC_SCOPE("still-scoped");
            CC_RECORD_RELATION(cc::rec::relation_follows, a, b);
            CC_RECORD_MARK("still-recorded"); // a different category again, so it lands either way
        });

    CHECK(no_tracing.trace_relations().empty());
    CHECK(no_tracing.count_of_kind(cc::rec::event_kind::async_scope_begin) == 1);
    CHECK(no_tracing.count("still-recorded") == 1);

    auto const no_profiling = capture(
        [&]
        {
            scoped_domain_mask const restore(cc::rec::g_default_domain);
            cc::rec::g_default_domain.set_enabled(cc::rec::category::profiling, false);

            CC_RECORD_ASYNC_SCOPE("silenced");
            CC_RECORD_RELATION(cc::rec::relation_follows, a, b);
        });

    CHECK(no_profiling.count_of_kind(cc::rec::event_kind::async_scope_begin) == 0);
    CHECK(no_profiling.trace_relations().size() == 1);
}

REC_TEST("record/trace - a trace follows the work across a co_await")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::trace_id opened = cc::rec::trace_id::none;
    cc::rec::trace_id before = cc::rec::trace_id::none;
    cc::rec::trace_id after = cc::rec::trace_id::none;

    auto const r = capture(
        [&]
        {
            CC_RECORD_ASYNC_SCOPE("spanning-request");
            opened = cc::rec::current_trace_id();

            auto const co = [](cc::rec::trace_id* b, cc::rec::trace_id* a) -> cc::shared_async<int>
            {
                *b = cc::rec::current_trace_id();
                CC_RECORD_MARK("before-suspend");

                co_await cc::async_yield();

                // The whole reason the fold happened: the thread-local version lost the trace here.
                *a = cc::rec::current_trace_id();
                CC_RECORD_MARK("after-suspend");

                co_return 1;
            }(&before, &after);

            CHECK(cc::async_blocking_get(co) == 1);
        });

    CHECK(opened != cc::rec::trace_id::none);
    CHECK(before == opened);
    CHECK(after == opened);

    // And the attribution reaches the recording, on whichever worker the continuation ran.
    auto const of_trace = r.from_trace(opened);
    CHECK(of_trace.count("before-suspend") == 1);
    CHECK(of_trace.count("after-suspend") == 1);
}
