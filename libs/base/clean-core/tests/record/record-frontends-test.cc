#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/record/console_listener.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

namespace
{
enum class widget_kind : u8
{
    round = 3,
    square = 7,
};
} // namespace

//
// Logging
//

REC_TEST("record/log - a message with no arguments costs the stream no payload at all")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        CC_LOG_INFO("plain message");
        cc::rec::flush_blocking();
    }

    auto const* const e = c.first_named("plain message");
    REQUIRE(e != nullptr);
    CHECK(e->kind == cc::rec::event_kind::log);
    CHECK(e->level == cc::rec::level::info);

    // The text lives in the descriptor, so the event carries nothing.
    CHECK(e->text.empty());
}

REC_TEST("record/log - a formatted message is written straight into the chunk")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        CC_LOG_INFO("uploaded {} bytes to {}", 4096, "gpu");
        cc::rec::flush_blocking();
    }

    // The format string is the site's name, so every message from one site groups under one string.
    auto const* const e = c.first_named("uploaded {} bytes to {}");
    REQUIRE(e != nullptr);
    CHECK(e->text == "uploaded 4096 bytes to gpu");
}

REC_TEST("record/log - levels gate independently, and the gate is the domain's")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        scoped_domain_mask const restore(cc::rec::g_default_domain);

        // trace and debug are opt-in, so a default build records neither.
        CC_LOG_TRACE("trace-off");
        CC_LOG_DEBUG("debug-off");
        CC_LOG_INFO("info-on");
        CC_LOG_WARNING("warning-on");
        CC_LOG_ERROR("error-on");
        cc::rec::flush_blocking();

        CHECK(c.count_named("trace-off") == 0);
        CHECK(c.count_named("debug-off") == 0);
        CHECK(c.count_named("info-on") == 1);
        CHECK(c.count_named("warning-on") == 1);
        CHECK(c.count_named("error-on") == 1);

        cc::rec::g_default_domain.set_enabled(cc::rec::level::trace, true);
        cc::rec::g_default_domain.set_enabled(cc::rec::level::info, false);
        CC_LOG_TRACE("trace-now-on");
        CC_LOG_INFO("info-now-off");
        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("trace-now-on") == 1);
    CHECK(c.count_named("info-now-off") == 0);
}

REC_TEST("record/log - a message longer than the chunk can take is truncated, never dropped")
{
    auto cfg = deterministic_config();
    cfg.chunk_bytes = 8 * 1024;
    rec_fixture const fixture(cfg);

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        auto const huge = cc::string::create_filled(16 * 1024, 'x');
        CC_LOG_INFO("{}", huge);
        cc::rec::flush_blocking();
    }

    isize seen = 0;
    rl.result().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::log)
                return;
            ++seen;
            CHECK(e.is_truncated());
            CHECK(e.payload_as_text().size() > 0);
        });
    CHECK(seen == 1);
}

//
// Values and markers
//

REC_TEST("record/value - scalars, enums, pointers and text all read back")
{
    rec_fixture const fixture(deterministic_config());

    int const answer = 42;
    auto const kind = widget_kind::square;
    void const* const address = &answer;

    collector c;
    {
        scoped_listener const reg(c);

        CC_RECORD_MARK("fallback-taken");
        CC_RECORD("answer", answer);
        CC_RECORD("kind", kind);
        CC_RECORD("address", address);
        CC_RECORD("ratio", 0.25);
        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("fallback-taken") == 1);
    CHECK(c.first_named("fallback-taken")->kind == cc::rec::event_kind::marker);

    REQUIRE(c.first_named("answer") != nullptr);
    CHECK(c.first_named("answer")->value.value() == 42.0);

    // An enum collapses onto its underlying type, so a consumer reads a number without knowing the type.
    REQUIRE(c.first_named("kind") != nullptr);
    CHECK(c.first_named("kind")->value.value() == 7.0);

    // A pointer is an opaque address, never dereferenced.
    REQUIRE(c.first_named("address") != nullptr);
    CHECK(c.first_named("address")->kind == cc::rec::event_kind::value);

    REQUIRE(c.first_named("ratio") != nullptr);
    CHECK(c.first_named("ratio")->value.value() == 0.25);
}

REC_TEST("record/value - text is recorded as bytes, not as an address")
{
    rec_fixture const fixture(deterministic_config());

    auto const owned = cc::string("from-a-string");
    char const* const c_str = "from-a-pointer";

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        CC_RECORD("owned", owned);
        CC_RECORD("c_str", c_str);
        CC_RECORD("literal", "from-a-literal");
        cc::rec::flush_blocking();
    }

    cc::vector<cc::string> texts;
    rl.result().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (auto const t = e.field_as_text("value"); t.has_value())
                texts.push_back(cc::string(t.value()));
        });

    REQUIRE(texts.size() == 3);
    CHECK(texts[0] == "from-a-string");
    CHECK(texts[1] == "from-a-pointer"); // a char const* means its bytes, never its address
    CHECK(texts[2] == "from-a-literal");
}

//
// Stats
//

REC_TEST("record/stat - snapshots and accumulates are distinct kinds and carry their unit")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        CC_RECORD_STAT("queue_depth", cc::rec::unit_count, 12);
        CC_RECORD_ACCUM("bytes_uploaded", cc::rec::unit_bytes, 4096);
        cc::rec::flush_blocking();
    }

    auto const* const depth = c.first_named("queue_depth");
    REQUIRE(depth != nullptr);
    CHECK(depth->kind == cc::rec::event_kind::stat_snapshot);
    CHECK(depth->value.value() == 12.0);
    CHECK(depth->quantity == &cc::rec::unit_count);

    auto const* const uploaded = c.first_named("bytes_uploaded");
    REQUIRE(uploaded != nullptr);
    CHECK(uploaded->kind == cc::rec::event_kind::stat_accumulate);
    CHECK(uploaded->value.value() == 4096.0);
    REQUIRE(uploaded->quantity != nullptr);

    // The unit is what lets a listener graph a quantity it has never heard of.
    CHECK(uploaded->quantity->prefix_base == 1024);
    CHECK(cc::string_view(uploaded->quantity->symbol) == "B");
}

//
// Scopes
//

REC_TEST("record/scope - a scope opens and closes at the depth it was entered")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        {
            CC_RECORD_SCOPE("outer");
            {
                CC_RECORD_SCOPE("inner");
            }
        }
        cc::rec::flush_blocking();
    }

    auto const opens = c.of_kind(cc::rec::event_kind::scope_begin);
    auto const closes = c.of_kind(cc::rec::event_kind::scope_end);
    REQUIRE(opens.size() == 2);
    REQUIRE(closes.size() == 2);

    CHECK(cc::string_view(opens[0]->name) == "outer");
    CHECK(opens[0]->depth.value() == 0);
    CHECK(cc::string_view(opens[1]->name) == "inner");
    CHECK(opens[1]->depth.value() == 1);

    // Closes come back innermost first, at the depth they opened at.
    CHECK(cc::string_view(closes[0]->name) == "inner");
    CHECK(closes[0]->depth.value() == 1);
    CHECK(cc::string_view(closes[1]->name) == "outer");
    CHECK(closes[1]->depth.value() == 0);
}

REC_TEST("record/scope - a scope with no name takes the enclosing function's")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        {
            CC_RECORD_SCOPE();
        }
        cc::rec::flush_blocking();
    }

    auto const opens = c.of_kind(cc::rec::event_kind::scope_begin);
    REQUIRE(opens.size() == 1);
    CHECK(!opens[0]->name.empty());
}

REC_TEST("record/scope - the explicit begin/end pair nests like the block form")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        CC_RECORD_SCOPE_BEGIN("manual");
        CC_RECORD_MARK("inside");
        CC_RECORD_SCOPE_END("manual");
        cc::rec::flush_blocking();
    }

    REQUIRE(c.of_kind(cc::rec::event_kind::scope_begin).size() == 1);
    REQUIRE(c.of_kind(cc::rec::event_kind::scope_end).size() == 1);
    CHECK(c.of_kind(cc::rec::event_kind::scope_begin)[0]->depth.value() == 0);
    CHECK(c.of_kind(cc::rec::event_kind::scope_end)[0]->depth.value() == 0);
    CHECK(c.count_named("inside") == 1);
}

REC_TEST("record/scope - an END with nothing open is refused rather than wrapping the depth")
{
    // The failure this bounds: a BEGIN whose END is never reached — an early return, a throw — leaves the thread one
    // level deep forever, and every later END pops one too many.
    // scope_depth is UNSIGNED, so the first over-pop would wrap it to four billion and misplace every scope on the
    // thread for the rest of its life, long after the frame that dropped its END is gone.
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        // A leak, then a close that has nothing of its own to close.
        CC_RECORD_SCOPE_BEGIN("leaked");
        CC_RECORD_SCOPE_END("leaked");
        CC_RECORD_SCOPE_END("nothing-is-open");

        // Whatever the damage above, the depth is still sane here.
        // In its own block, so its END is recorded before the flush rather than after it.
        {
            CC_RECORD_SCOPE("after");
        }

        cc::rec::flush_blocking();
    }

    auto const ends = c.of_kind(cc::rec::event_kind::scope_end);

    // Two ends, not three: the unmatched one recorded nothing at all.
    REQUIRE(ends.size() == 2);

    // And the scope opened afterwards still sits at depth 0, which is what would have been lost to a wrap.
    CHECK(c.of_kind(cc::rec::event_kind::scope_begin).size() == 2);
    CHECK(ends[1]->depth.value() == 0);
}

REC_TEST("record/scope - a disabled category leaves the pair balanced")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        scoped_domain_mask const restore(cc::rec::g_default_domain);

        cc::rec::g_default_domain.set_enabled(cc::rec::category::profiling, false);
        {
            CC_RECORD_SCOPE("silenced");
            CC_RECORD_MARK("still-recorded"); // a different category, so it still lands
        }
        cc::rec::flush_blocking();
    }

    CHECK(c.of_kind(cc::rec::event_kind::scope_begin).empty());
    CHECK(c.of_kind(cc::rec::event_kind::scope_end).empty());
    CHECK(c.count_named("still-recorded") == 1);
}

REC_TEST("record/scope - a scope that opens and closes between two suspends is fine")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        auto const work = []() -> cc::shared_async<int>
        {
            {
                CC_RECORD_SCOPE("before-await");
            }
            co_await cc::async_yield();
            {
                CC_RECORD_SCOPE("after-await");
            }
            co_return 7;
        }();

        CHECK(cc::async_blocking_get(work) == 7);
        cc::rec::flush_blocking();
    }

    // The frame driver asserts only when a scope is still open across a suspend, which neither of these is.
    CHECK(c.count_named("before-await") == 2); // one begin, one end
    CHECK(c.count_named("after-await") == 2);
}

//
// The console listener
//

REC_TEST("record/console - only log events reach the terminal, in timestamp order")
{
    rec_fixture const fixture(deterministic_config());

    auto console = cc::rec::console_listener({.min_level = cc::rec::level::warning, .time = cc::rec::console_time::none});
    {
        scoped_listener const reg(console);

        CC_LOG_INFO("below the console's threshold");
        CC_RECORD_MARK("not-a-log");
        CC_LOG_WARNING("printed once");
        CC_LOG_ERROR("printed twice? no, once");
        cc::rec::flush_blocking();
    }

    // Two messages at or above warning; the info line and the marker are not the console's business.
    CHECK(console.printed_count() == 2);
}

REC_TEST("record/log - an error carries the stack it was logged from")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        CC_LOG_ERROR("something went wrong");
        cc::rec::flush_blocking();
    }
    auto const r = rl.take();

    // The domain captures a stack for errors, and cc::capture_stack is what fills it.
    // A frame count of zero was the honest answer while that was a stub; it is a regression now.
    isize frame_count = -1;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (cc::string_view(e.desc->name) == "record.stacktrace")
                frame_count = e.field_as_u64_array("frames").size();
        });

    REQUIRE(frame_count >= 0); // the event itself must be there either way
    CHECK((frame_count > 0) == cc::stack_capture_available());
}

REC_TEST("record/scope - CC_RECORD_SCOPE_IF opens only when its condition holds")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        {
            CC_RECORD_SCOPE_IF(true, "taken");
        }
        {
            CC_RECORD_SCOPE_IF(false, "skipped");
        }

        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("taken") == 2); // one begin, one end
    CHECK(c.count_named("skipped") == 0);
}

REC_TEST("record/scope - a scope that opened still closes, whatever the condition would say later")
{
    // The condition is read once, at entry.
    // A guard that re-read it on the way out could close a scope it never opened, or leave one open forever — and
    // an unbalanced pair is a wrong flame graph rather than a diagnostic.
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        auto still_true = true;
        {
            CC_RECORD_SCOPE_IF(still_true, "closes-anyway");
            still_true = false;
        }

        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("closes-anyway") == 2);
}

REC_TEST("record/scope - a conditional scope nests inside an unconditional one")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        {
            CC_RECORD_SCOPE("outer");
            {
                CC_RECORD_SCOPE_IF(false, "inner-skipped");
                {
                    CC_RECORD_SCOPE("inner-kept");
                }
            }
        }

        cc::rec::flush_blocking();
    }

    // A skipped scope must not consume a depth level, or everything under it is re-nested one step too deep.
    CHECK(c.count_named("outer") == 2);
    CHECK(c.count_named("inner-skipped") == 0);
    CHECK(c.count_named("inner-kept") == 2);
}
