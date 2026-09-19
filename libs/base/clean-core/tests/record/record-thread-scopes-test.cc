#include "record-test-types.hh"

#include <clean-core/record/scope.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/thread_scopes.hh>
#include <clean-core/record/value.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

using namespace cc_rec_test;

// Reading what a thread has open WITHOUT stopping it.
//
// The claim under test is not that the names come back — that much a recording already carries — but that they come
// back to a reader that is not the thread in question, from its published events alone.
// That is what a hang report needs, and on wasm it is the only per-thread answer there is.

namespace
{
/// What this thread's entry said, copied out of the callback.
///
/// Owned rather than a `thread_scope_view`, because a view's `levels` borrows the reader's own scratch and is valid
/// only for the duration of the callback — copying the view out hands back a dangling span.
struct own_scopes
{
    u32 depth = 0;
    bool complete = false;
    cc::vector<cc::string_view> names;
};

/// One thread's entry, or nothing when the registry was busy or that thread has not recorded yet.
/// Looked up by id rather than taken as the first thread, because nexus runs tests on a pool and the registry holds
/// every thread that has ever recorded.
[[nodiscard]] cc::optional<own_scopes> read_scopes_of(cc::thread_id self)
{
    auto found = cc::optional<own_scopes>();

    auto const read = cc::rec::try_read_thread_scopes(
        [&](cc::rec::thread_scope_view const& t)
        {
            if (t.id != self)
                return;

            auto out = own_scopes{.depth = t.depth, .complete = t.is_complete()};
            for (auto const* const d : t.levels)
                out.names.push_back(d != nullptr && d->name != nullptr ? cc::string_view(d->name) : cc::string_view());
            found = cc::move(out);
        });

    if (!read)
        return {};
    return found;
}

[[nodiscard]] cc::optional<own_scopes> read_own_scopes()
{
    return read_scopes_of(cc::current_thread_id());
}
} // namespace

REC_TEST("record/scopes - an open scope is visible to a reader that is not the thread that opened it")
{
    if (!threads_available())
        SKIP("no second thread to open the scopes on");

    rec_fixture const fixture(deterministic_config());

    // The worker opens two scopes and parks inside them; this thread reads them and only then lets it go.
    // That is the hang report's situation exactly: the thread being described is not the one describing it.
    auto opened = cc::atomic<bool>(false);
    auto release = cc::atomic<bool>(false);
    auto worker_id = cc::atomic<cc::thread_id>(cc::thread_id::invalid);

    auto worker = std::thread(
        [&]
        {
            worker_id.store(cc::current_thread_id(), cc::memory_order_release);
            CC_RECORD_SCOPE("outer-scope-under-test");
            {
                CC_RECORD_SCOPE("inner-scope-under-test"); // nested, since one block holds one scope guard
                opened.store(true, cc::memory_order_release);
                while (!release.load(cc::memory_order_acquire))
                    cc::this_thread_sleep_secs(0.001);
            }
        });

    while (!opened.load(cc::memory_order_acquire))
        cc::this_thread_sleep_secs(0.001);

    auto const seen = read_scopes_of(worker_id.load(cc::memory_order_acquire));
    release.store(true, cc::memory_order_release);
    worker.join();

    REQUIRE(seen.has_value());
    REQUIRE(seen.value().depth == 2);
    REQUIRE(seen.value().names.size() == 2);

    // Outermost first, which is the order a reader renders.
    CHECK(seen.value().names[0] == "outer-scope-under-test");
    CHECK(seen.value().names[1] == "inner-scope-under-test");
    CHECK(seen.value().complete);
}

REC_TEST("record/scopes - a scope closing is visible too")
{
    rec_fixture const fixture(deterministic_config());

    CC_RECORD_SCOPE("outer-scope-under-test");
    {
        CC_RECORD_SCOPE("inner-scope-under-test");
        REQUIRE(read_own_scopes().has_value());
        CHECK(read_own_scopes().value().depth == 2);
    }

    // The inner one closed, and the outer one did not.
    auto const after_inner = read_own_scopes();
    REQUIRE(after_inner.has_value());
    CHECK(after_inner.value().depth == 1);
    REQUIRE(after_inner.value().names.size() == 1);
    CHECK(after_inner.value().names[0] == "outer-scope-under-test");
}

REC_TEST("record/scopes - a scope opened before its chunk was drained is still named")
{
    rec_fixture const fixture(deterministic_config());

    // A hung thread stops recording, and the consumer drains what it had; the scope it is stuck in must survive that.
    // Enough marks to fill the chunk the scope began in, so the name can only come back through a later chunk's
    // preamble, which restates the outermost open scopes.
    CC_RECORD_SCOPE("opened-long-ago");
    for (auto i = 0; i < 20'000; ++i)
        CC_RECORD_MARK("filler");
    cc::rec::flush_blocking();

    auto const view = read_own_scopes();
    REQUIRE(view.has_value());
    CHECK(view.value().depth == 1);
    REQUIRE(view.value().names.size() == 1);
    CHECK(view.value().names[0] == "opened-long-ago");
}

REC_TEST("record/scopes - nesting deeper than the preamble names is still named from the window")
{
    rec_fixture const fixture(deterministic_config());

    // Deeper than the preamble can name, which is the case worth pinning: the depth must still be right, and the
    // levels opened inside the window must still be named.
    // Nested rather than sequential, since a scope is a guard and one block holds one.
    CC_RECORD_SCOPE("depth-1");
    {
        CC_RECORD_SCOPE("depth-2");
        {
            CC_RECORD_SCOPE("depth-3");
            {
                CC_RECORD_SCOPE("depth-4");
                {
                    CC_RECORD_SCOPE("depth-5");

                    auto const view = read_own_scopes();
                    REQUIRE(view.has_value());
                    CHECK(view.value().depth == 5);
                    REQUIRE(view.value().names.size() == 5);

                    CHECK(view.value().names[0] == "depth-1");
                    CHECK(view.value().names[4] == "depth-5");
                }
            }
        }
    }
}

REC_TEST("record/scopes - a thread that has recorded is listed")
{
    rec_fixture const fixture(deterministic_config());

    // A thread joins the registry by recording, so this thread is present only once it has.
    CC_RECORD_SCOPE("register-this-thread");

    auto seen = 0;
    auto const read = cc::rec::try_read_thread_scopes([&](cc::rec::thread_scope_view const&) { ++seen; });

    CHECK(read);
    CHECK(seen > 0);
}
