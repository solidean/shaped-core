#include "record-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/container/set.hh>
#include <clean-core/record/async_scope.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

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

/// How many ambient-change deltas a recording holds.
isize ambient_changes(cc::rec::recording const& r)
{
    return r.count_of_kind(cc::rec::event_kind::ambient_changed);
}
} // namespace

REC_TEST("record/async-scope - an ambient scope publishes a delta on entry and on exit")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_MARK("outside");
            {
                CC_RECORD_ASYNC_SCOPE("load-level");
                CC_RECORD_MARK("inside");
            }
            CC_RECORD_MARK("outside-again");
        });

    // One on entering, one on restoring.
    // Eager rather than lazy on purpose: a region that records nothing still has to be attributed, because an async
    // scope is about where TIME goes.
    CHECK(ambient_changes(r) == 2);
    CHECK(r.count("inside") == 1);
}

REC_TEST("record/async-scope - a scope that records nothing still publishes its span")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_MARK("before");
            {
                CC_RECORD_ASYNC_SCOPE("silent-work");
                // Deliberately nothing.
                // A lazy delta would never fire here, and everything after would be billed to whatever context
                // preceded the scope.
            }
            CC_RECORD_MARK("after");
        });

    CHECK(ambient_changes(r) == 2);
}

REC_TEST("record/async-scope - the innermost scope is the one in effect, and nesting restores")
{
    rec_fixture const fixture(deterministic_config());

    CHECK(cc::rec::current_async_scope() == nullptr);

    {
        CC_RECORD_ASYNC_SCOPE("outer");
        auto const* const outer = cc::rec::current_async_scope();
        REQUIRE(outer != nullptr);
        CHECK(cc::string_view(outer->name) == "outer");

        {
            CC_RECORD_ASYNC_SCOPE("inner");
            auto const* const inner = cc::rec::current_async_scope();
            REQUIRE(inner != nullptr);
            CHECK(cc::string_view(inner->name) == "inner");
        }

        // The chain is a stack, so leaving the inner one exposes the outer again.
        CHECK(cc::rec::current_async_scope() == outer);
    }

    CHECK(cc::rec::current_async_scope() == nullptr);
}

REC_TEST("record/async-scope - the scope survives a co_await, which a local scope may not")
{
    rec_fixture const fixture(deterministic_config());

    cc::string before;
    cc::string after;

    auto const r = capture(
        [&]
        {
            CC_RECORD_ASYNC_SCOPE("spanning-work");

            auto const co = [](cc::string* b, cc::string* a) -> cc::shared_async<int>
            {
                if (auto const* const s = cc::rec::current_async_scope(); s != nullptr)
                    *b = s->name;

                co_await cc::async_yield();

                // The whole point: the continuation is still under the scope, wherever it resumed.
                if (auto const* const s = cc::rec::current_async_scope(); s != nullptr)
                    *a = s->name;

                co_return 1;
            }(&before, &after);

            CHECK(cc::async_blocking_get(co) == 1);
        });

    CHECK(before == "spanning-work");
    CHECK(after == "spanning-work");
    CHECK(ambient_changes(r) >= 2);
}

REC_TEST("record/async-scope - a repeat restore to the same context costs no delta")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            CC_RECORD_ASYNC_SCOPE("steady");

            // Nothing here changes the ambient, so nothing beyond the scope's own two deltas is written.
            for (int i = 0; i < 50; ++i)
                CC_RECORD_MARK("tick");
        });

    CHECK(r.count("tick") == 50);
    CHECK(ambient_changes(r) == 2);
}

REC_TEST("record/async-scope - attribution outlives the links it came from")
{
    auto cfg = deterministic_config();
    cfg.chunk_bytes = 8 * 1024; // small, so the events rotate through several chunks
    rec_fixture const fixture(cfg);

    cc::rec::recording captured;
    {
        cc::rec::recording_listener rl;
        {
            scoped_listener const reg(rl);
            for (int i = 0; i < 200; ++i)
            {
                CC_RECORD_ASYNC_SCOPE("churn");
                CC_RECORD_MARK("work");
            }
            cc::rec::flush_blocking();
        }
        captured = rl.take();
    }

    // Every scope object and every link is long gone, and the recording still tells the two hundred contexts apart.
    // That is the point of attributing by ID rather than by the ambient ADDRESS: an address is unique only while its
    // link lives, so keeping it meaningful would have meant pinning one per context switch.
    CHECK(captured.count("work") == 200);

    cc::set<u64> traces;
    captured.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::ambient_changed)
                return;
            if (auto const t = e.field_as_u64("trace").value_or(0); t != 0)
                traces.insert(t);
        });

    CHECK(traces.size() == 200);

    // Two per scope, plus one wherever a chunk rotation made the next write unconditional.
    CHECK(ambient_changes(captured) >= 400);
}

REC_TEST("record/async-scope - ambient deltas gate on the profiling category")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture(
        []
        {
            scoped_domain_mask const restore(cc::rec::g_system_domain);
            cc::rec::g_system_domain.set_enabled(cc::rec::category::profiling, false);

            CC_RECORD_ASYNC_SCOPE("silenced");
            CC_RECORD_MARK("still-recorded"); // a different category and domain, so it still lands
        });

    CHECK(ambient_changes(r) == 0);
    CHECK(r.count("still-recorded") == 1);
}

REC_TEST("record/async-scope - a recording's pin is not outstanding work")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        cc::async_ambient_scope const s(cc::rec::impl::async_scope_tag(), nullptr);

        // The recorder pins this very link into a chunk, and a pin takes a reference.
        // It must not read as work in flight: nexus fails a test whose context outlives it, and a recording holding on
        // to that context is not the thing that check is for.
        cc::rec::flush_blocking();
        CHECK(s.outstanding() == 0);
    }
}
