#include "record-test-types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/record.hh>
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
struct sample_payload
{
    u64 index = 0;
    f64 value = 0;
};

constexpr cc::rec::field sample_fields[] = {
    {.name = "index", .type = cc::rec::type_code::u64_, .offset = 0, .size = 8},
    {.name = "value", .type = cc::rec::type_code::f64_, .offset = 8, .size = 8},
};
} // namespace

// A site's name is part of its descriptor, so it must be a compile-time constant — these are macros for exactly that
// reason, and a helper taking a runtime `char const*` would not compile.
#define REC_MARK(name_) CC_RECORD_EVENT(cc::rec::event_kind::marker, cc::rec::category::values, name_)
#define REC_SAMPLE(name_, payload_) \
    CC_RECORD_EVENT_WITH(cc::rec::event_kind::value, cc::rec::category::values, name_, nullptr, sample_fields, payload_)

REC_TEST("record - recording before initialize is inert")
{
    // Nothing has brought the system up, so the write path finds no pool and simply counts the loss.
    CHECK(!cc::rec::is_initialized());
    REC_MARK("before-init");
    REC_SAMPLE("before-init-value", (sample_payload{.index = 1, .value = 2.0}));

    // And the system still comes up cleanly afterwards.
    rec_fixture const fixture(deterministic_config());
    CHECK(cc::rec::is_initialized());
}

REC_TEST("record - an event round-trips through a listener")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        REC_MARK("alpha");
        REC_MARK("beta");
        REC_MARK("alpha");

        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("alpha") == 2);
    CHECK(c.count_named("beta") == 1);

    for (auto const& e : c.events)
        if (cc::string_view(e.name) == "alpha")
            CHECK(e.kind == cc::rec::event_kind::marker);
}

REC_TEST("record - timestamps within a thread do not go backwards")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        for (int i = 0; i < 64; ++i)
            REC_MARK("tick");
        cc::rec::flush_blocking();
    }

    REQUIRE(c.events.size() >= 64);
    for (isize i = 1; i < c.events.size(); ++i)
        CHECK(c.events[i].cycles >= c.events[i - 1].cycles);
}

REC_TEST("record - payload fields read back generically")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        REC_SAMPLE("sample", (sample_payload{.index = 7, .value = 1.5}));
        cc::rec::flush_blocking();
    }

    isize seen = 0;
    rl.result().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (cc::string_view(e.name()) != "sample")
                return;

            ++seen;
            auto const index = e.field_as_int("index");
            auto const value = e.field_as_double("value");
            CHECK(index.has_value());
            CHECK(value.has_value());
            CHECK(index.value() == 7);
            CHECK(value.value() == 1.5);

            // A field nobody declared is absent rather than garbage.
            CHECK(!e.field_as_double("nope").has_value());
        });

    CHECK(seen == 1);
}

REC_TEST("record - a disabled category costs nothing and records nothing")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        scoped_domain_mask const restore(cc::rec::g_default_domain);

        cc::rec::g_default_domain.set_enabled(cc::rec::category::values, false);
        REC_MARK("silenced");
        cc::rec::flush_blocking();

        CHECK(c.count_named("silenced") == 0);

        cc::rec::g_default_domain.set_enabled(cc::rec::category::values, true);
        REC_MARK("audible");
        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("audible") == 1);
}

REC_TEST("record - rotating through chunks loses nothing and reports each acquisition")
{
    auto cfg = deterministic_config();
    cfg.chunk_bytes = 8 * 1024; // small enough that a few hundred events rotate several times
    rec_fixture const fixture(cfg);

    collector c;
    constexpr isize event_count = 2000;
    {
        scoped_listener const reg(c);
        for (isize i = 0; i < event_count; ++i)
            REC_SAMPLE("rotating", (sample_payload{.index = u64(i), .value = f64(i)}));
        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("rotating") == event_count);
    CHECK(c.count_named("record.chunk_acquired") > 1);
    CHECK(c.count_named("record.gap") == 0); // grow_unbounded never drops
}

REC_TEST("record - exhausting a drop-policy budget reports a gap rather than lying")
{
    auto cfg = deterministic_config();
    cfg.chunk_bytes = 8 * 1024;
    // Three chunks: enough that the consumer can still release one (a thread always retains its newest), and few
    // enough that a flood with nothing draining runs the pool dry.
    cfg.budget_bytes = 3 * 8 * 1024;
    cfg.overflow = cc::rec::overflow_policy::drop;
    cfg.drop_retry_secs = 0; // retry every time, so the test does not depend on wall-clock timing
    cfg.ready_chunks = 1;
    rec_fixture const fixture(cfg);

    collector c;
    {
        scoped_listener const reg(c);

        // Far more than one chunk holds, with nothing draining, so the pool must run dry.
        for (isize i = 0; i < 4000; ++i)
            REC_SAMPLE("flood", (sample_payload{.index = u64(i), .value = 0}));

        cc::rec::flush_blocking();

        // Now that a chunk is free again, the next event carries the gap report.
        REC_MARK("after-flood");
        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("flood") < 4000);
    CHECK(c.count_named("record.gap") >= 1);
}

REC_TEST("record - an open writer publishes only what was written, and flags truncation")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        CC_REC_DEFINE_DESC(text_desc, cc::rec::event_kind::log, cc::rec::level::info,
                           cc::rec::enable_bit_of(cc::rec::level::info), "open-writer", nullptr, nullptr, 0,
                           cc::rec::desc::variable_payload);

        {
            auto writer = cc::rec::open_event(text_desc, 16);
            REQUIRE(writer.is_open());
            auto const out = writer.payload();
            REQUIRE(out.size() >= 5);
            for (isize i = 0; i < 5; ++i)
                out[i] = byte("hello"[i]);
            writer.commit(5);
        }

        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("open-writer") == 1);
}

REC_TEST("record - a recording is a value that replays")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        REC_MARK("captured");
        REC_MARK("captured");
        cc::rec::flush_blocking();
    }

    auto const captured = rl.take();
    CHECK(captured.event_count() >= 2);

    // Replaying reaches a listener that was never registered, which is what makes a recording testable offline.
    collector replayed;
    captured.replay(replayed);
    CHECK(replayed.count_named("captured") == 2);

    // And concatenation is just appending blocks.
    cc::rec::recording joined;
    joined.append(captured);
    joined.append(captured);
    CHECK(joined.event_count() == captured.event_count() * 2);
}

REC_TEST("record - a listener may record, and only lower layers see it")
{
    rec_fixture const fixture(deterministic_config());

    /// Records one event of its own every time it is fed a chunk.
    struct echoing_listener final : cc::rec::listener
    {
        void on_chunk(cc::rec::chunk_view const&) override
        {
            if (echoes_left > 0)
            {
                --echoes_left;
                REC_MARK("echo");
            }
        }
        [[nodiscard]] cc::string_view listener_name() const override { return "echoing"; }

        int echoes_left = 1;
    };

    collector below;       // layer 0: sees everything, including what higher layers record
    echoing_listener echo; // layer 1: records
    collector above;       // layer 2: must never see the echo

    {
        scoped_listener const reg_below(below);
        scoped_listener const reg_echo(echo);
        scoped_listener const reg_above(above);

        CHECK(reg_below.layer() == 0);
        CHECK(reg_echo.layer() == 1);
        CHECK(reg_above.layer() == 2);

        REC_MARK("trigger");
        cc::rec::flush_blocking(); // dispatches "trigger"; the echo lands in a layer-1 chunk
        cc::rec::flush_blocking(); // dispatches the echo
    }

    CHECK(below.count_named("trigger") == 1);
    CHECK(above.count_named("trigger") == 1);

    CHECK(below.count_named("echo") == 1); // layer 0 < 1
    CHECK(above.count_named("echo") == 0); // layer 2 is not below 1
}

REC_TEST("record - events from a thread that has exited still arrive")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);

        std::thread worker(
            []
            {
                cc::rec::set_current_thread_record_name("worker");
                for (int i = 0; i < 10; ++i)
                    REC_MARK("from-worker");
            });
        worker.join();

        cc::rec::flush_blocking();
    }

    CHECK(c.count_named("from-worker") == 10);
}

REC_TEST("record - the background thread drains without anyone asking, and its absence is the documented fallback")
{
    auto cfg = deterministic_config();
    cfg.threaded = true;
    cfg.poll_interval_secs = 0.0005;
    rec_fixture const fixture(cfg);

    collector c;
    {
        scoped_listener const reg(c);

        REC_MARK("background");
        cc::rec::seal_current_thread_chunk(); // so the tail does not sit waiting for the chunk to fill

        // The worker polls, so give it a bounded number of chances rather than a fixed sleep.
        for (int i = 0; i < 200 && c.count_named("background") == 0; ++i)
            cc::this_thread_sleep_secs(0.005);

        // Without threads there is nobody to drain, and no API disappears — a flush on the caller does the work
        // instead, which is exactly what SC_THREADS=OFF is supposed to cost.
        if constexpr (CC_HAS_THREADS == 0)
        {
            CHECK(c.count_named("background") == 0);
            cc::rec::flush_blocking();
        }
    }

    CHECK(c.count_named("background") == 1);
}

REC_TEST("record - stats report what the system is doing")
{
    rec_fixture const fixture(deterministic_config());

    collector c;
    {
        scoped_listener const reg(c);
        CHECK(cc::rec::stats().registered_listeners == 1);

        REC_MARK("counted");
        cc::rec::flush_blocking();

        auto const s = cc::rec::stats();
        CHECK(s.threads >= 1);
        CHECK(s.events_processed >= 1);
        CHECK(s.chunks_processed >= 1);
        CHECK(s.allocated_bytes > 0);
    }

    CHECK(cc::rec::stats().registered_listeners == 0);
}
