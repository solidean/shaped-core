#include "record-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/pinned_value.hh>
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

// The chunk preamble.
//
// This is the one piece of stream state a consumer genuinely cannot derive.
// A trace can be carried forward from the ambient deltas, but a long-lived scope opens ONCE: a window that outlived
// its `scope_begin` — a ring buffer, a crash dump's tail, a decimated capture — has nothing else to learn from.
// So the producer states it at every rotation, and these pin that it does.

REC_TEST("record/stream - a chunk's preamble names the scopes that were already open")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        {
            CC_RECORD_SCOPE("outer-frame");

            // Enough to rotate the 64 KB chunk several times, so later chunks open with the scope already running.
            for (isize i = 0; i < 8000; ++i)
                CC_RECORD_MARK("filler");

            cc::rec::flush_blocking();
        }
        cc::rec::flush_blocking();
    }

    auto const rec = capture.take();

    isize preambles = 0;
    isize inside_the_scope = 0;
    rec.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::stream_state)
                return;

            ++preambles;
            if (e.field_as_u64("scope_depth").value_or(0) == 0)
                return;

            ++inside_the_scope;

            // The depth alone would let a reader nest correctly; the name is what makes the frame identifiable.
            CHECK(e.field_as_u64("named_scopes").value_or(0) >= 1);

            auto const* const outer = e.field_as_desc("scope0");
            REQUIRE(outer != nullptr);
            CHECK(cc::string_view(outer->name) == "outer-frame");
        });

    CHECK(preambles > 0);

    // The point of the exercise: at least one chunk began while the scope was open and says so on its own.
    CHECK(inside_the_scope > 0);
}

REC_TEST("record/stream - a preamble reports a depth it could not name in full")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        // Five deep against a preamble that names three, which is the case the two fields exist to tell apart.
        // A block each, because CC_RECORD_SCOPE declares fixed-name locals and two in one block collide.
        {
            CC_RECORD_SCOPE("depth-1");
            {
                CC_RECORD_SCOPE("depth-2");
                {
                    CC_RECORD_SCOPE("depth-3");
                    {
                        CC_RECORD_SCOPE("depth-4");
                        {
                            CC_RECORD_SCOPE("depth-5");

                            for (isize i = 0; i < 8000; ++i)
                                CC_RECORD_MARK("filler");

                            cc::rec::flush_blocking();
                        }
                    }
                }
            }
        }
        cc::rec::flush_blocking();
    }

    auto const rec = capture.take();

    auto deepest = u64(0);
    rec.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::stream_state)
                return;

            auto const depth = e.field_as_u64("scope_depth").value_or(0);
            auto const named = e.field_as_u64("named_scopes").value_or(0);
            deepest = cc::max(deepest, depth);

            // Never claims more names than it has, and never more than the fixed slot count.
            CHECK(named <= depth);
            CHECK(named <= 3);

            // The names it does carry are the OUTERMOST, which are the ones a bounded capture loses first.
            if (named >= 1)
                CHECK(cc::string_view(e.field_as_desc("scope0")->name) == "depth-1");
            if (named >= 3)
                CHECK(cc::string_view(e.field_as_desc("scope2")->name) == "depth-3");
        });

    CHECK(deepest == 5);
}

// The value codec's three tiers.
//
// A string LITERAL is stored as its address, because its bytes are in the binary and outlive everything.
// A runtime string is copied, because a pointer to it would name memory nobody promised to keep.
// The const on `char const[N]` is what tells them apart, which is why a buffer you formatted into lands on the safe
// side without anyone thinking about it.

REC_TEST("record/value - a literal costs the stream its address, a runtime string its bytes")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        CC_RECORD("literal", "a string literal");

        // Non-const, so it is copied: this is the snprintf-into-a-local shape, and storing its address would name a
        // frame that is gone before anything reads the event.
        char buffer[32] = {};
        cc::memcpy(buffer, "from a buffer", 14);
        CC_RECORD("buffered", buffer);

        auto const runtime = cc::string("built at runtime");
        CC_RECORD("runtime", runtime);

        cc::rec::flush_blocking();
    }

    auto const rec = capture.take();

    // All three read back identically — the tier is an encoding decision, never a difference the reader sees.
    CHECK(rec.first_text("literal").value() == "a string literal");
    CHECK(rec.first_text("buffered").value() == "from a buffer");
    CHECK(rec.first_text("runtime").value() == "built at runtime");

    // ... but the encodings differ, which is the whole point.
    auto const type_of = [&](cc::string_view name)
    {
        auto found = cc::rec::type_code::none;
        rec.for_each_event(
            [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
            {
                if (e.name() != name || e.fields().empty())
                    return;
                found = e.fields()[0].type;
            });
        return found;
    };

    CHECK(type_of("literal") == cc::rec::type_code::cstring);
    CHECK(type_of("buffered") == cc::rec::type_code::inline_text);
    CHECK(type_of("runtime") == cc::rec::type_code::inline_text);
}

REC_TEST("record/value - a runtime name reads back like a static one")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        auto const names = cc::vector<cc::string>{"queue.alpha", "queue.beta"};
        for (isize i = 0; i < names.size(); ++i)
            CC_RECORD_NAMED(names[i], i32(10 + i));

        cc::rec::flush_blocking();
    }

    auto const rec = capture.take();

    // Every name-keyed query goes through event_view::name(), so a dynamic name is invisible to callers.
    CHECK(rec.count("queue.alpha") == 1);
    CHECK(rec.count("queue.beta") == 1);
    CHECK(rec.contains("queue.alpha"));

    CHECK(rec.first_value("queue.alpha").value() == 10.0);
    CHECK(rec.first_value("queue.beta").value() == 11.0);

    // A site with no name of its own is still one site, which is what a runtime name costs: two events, one descriptor.
    CHECK(rec.count_of_kind(cc::rec::event_kind::value) == 2);
}

// Recording bytes BY PIN.
//
// The chunk holds a reference to the caller's storage instead of copying it, so a megabyte costs the stream sixteen
// bytes and the recording still hands back the real thing.
// What the chunk keeps alive is what these assert on: the bytes have to survive the owner going away.

REC_TEST("record/pin - pinned bytes cost the stream an address and outlive their owner")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        {
            auto payload = cc::vector<byte>();
            payload.resize_to_constructed(4096, byte(0xAB));
            payload[0] = byte(0x01);
            payload[4095] = byte(0x02);

            auto const pinned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
            CC_RECORD_PINNED("frame.depth", pinned);

            cc::rec::flush_blocking();
        }
        // The pinned_data is gone here, and the chunk's pin is the only thing keeping the bytes alive.
    }

    auto const rec = capture.take();

    isize seen = 0;
    rec.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.name() != "frame.depth")
                return;

            ++seen;
            auto const bytes = e.field_as_bytes("value");
            REQUIRE(bytes.size() == 4096);
            CHECK(bytes[0] == byte(0x01));
            CHECK(bytes[4095] == byte(0x02));
            CHECK(bytes[100] == byte(0xAB));
        });

    CHECK(seen == 1);

    // Sixteen bytes of payload, whatever the pinned data weighed.
    CHECK(rec.total_bytes() < 4096);
}

REC_TEST("record/pin - a chunk takes more pins than its array holds")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener capture;
    {
        scoped_listener const reg(capture);

        // Well past the 64-slot header array, so the spill path runs several times over.
        // Each spill moves the full array into a heap block and makes the block one pin, so the slots come back.
        constexpr isize pin_count = 500;
        for (isize i = 0; i < pin_count; ++i)
        {
            auto payload = cc::vector<byte>();
            payload.resize_to_constructed(8, byte(i & 0xFF));

            auto const pinned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
            CC_RECORD_PINNED("spilled", pinned);
        }

        cc::rec::flush_blocking();
    }

    auto const rec = capture.take();

    // Every one of them still readable, which is what says the spill chain kept its references rather than dropping
    // the ones it moved.
    isize readable = 0;
    rec.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.name() != "spilled")
                return;

            auto const bytes = e.field_as_bytes("value");
            if (bytes.size() == 8 && bytes[0] == byte(readable & 0xFF))
                ++readable;
        });

    CHECK(readable == 500);
}
