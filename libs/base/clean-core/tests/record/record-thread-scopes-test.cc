#include "record-test-types.hh"

#include <clean-core/record/scope.hh>
#include <clean-core/record/thread_scopes.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

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

/// This thread's entry, or nothing when the registry was busy or this thread has not recorded yet.
/// Looked up by id rather than taken as the first thread, because nexus runs tests on a pool and the registry holds
/// every thread that has ever recorded.
[[nodiscard]] cc::optional<own_scopes> read_own_scopes()
{
    auto const self = cc::current_thread_id();
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
} // namespace

REC_TEST("record/scopes - an open scope is visible to a reader that is not the thread that opened it")
{
    rec_fixture const fixture(deterministic_config());

    // A thread is in the registry only once it has recorded, so this joins it and then leaves nothing open.
    {
        CC_RECORD_SCOPE("join-the-registry");
    }
    {
        auto const before = read_own_scopes();
        REQUIRE(before.has_value());
        CHECK(before.value().depth == 0);
        CHECK(before.value().names.empty());
    }

    CC_RECORD_SCOPE("outer-scope-under-test");
    {
        CC_RECORD_SCOPE("inner-scope-under-test");

        auto const during = read_own_scopes();
        REQUIRE(during.has_value());
        REQUIRE(during.value().depth == 2);
        REQUIRE(during.value().names.size() == 2);

        // Outermost first, which is the order a reader renders.
        CHECK(during.value().names[0] == "outer-scope-under-test");
        CHECK(during.value().names[1] == "inner-scope-under-test");
        CHECK(during.value().complete);
    }

    // The inner one closed, and the outer one did not.
    auto const after_inner = read_own_scopes();
    REQUIRE(after_inner.has_value());
    CHECK(after_inner.value().depth == 1);
    REQUIRE(after_inner.value().names.size() == 1);
    CHECK(after_inner.value().names[0] == "outer-scope-under-test");
}

REC_TEST("record/scopes - the thread is never stopped, so the reader sees whatever is committed")
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

REC_TEST("record/scopes - a thread that has recorded nothing reports nothing rather than being absent")
{
    rec_fixture const fixture(deterministic_config());

    // A thread joins the registry by recording, so this thread is present only once it has.
    CC_RECORD_SCOPE("register-this-thread");

    auto seen = 0;
    auto const read = cc::rec::try_read_thread_scopes([&](cc::rec::thread_scope_view const&) { ++seen; });

    CHECK(read);
    CHECK(seen > 0);
}
