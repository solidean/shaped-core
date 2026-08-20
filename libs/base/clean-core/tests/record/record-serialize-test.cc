#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/crash_dump.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

namespace
{
/// Captures what `body` records, narrowed to the thread that ran it.
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

/// Records a workload that touches every part of the format: a scope, a stat with a unit, a value, text and a log.
void record_everything()
{
    CC_RECORD_SCOPE("outer");
    CC_RECORD("count", 7);
    CC_RECORD("path", "assets/tree.obj");
    CC_RECORD_STAT("queue_depth", cc::rec::unit_count, 3);
    CC_RECORD_ACCUM("bytes", cc::rec::unit_bytes, 4096);
    CC_RECORD_MARK("marked");
    CC_LOG_WARNING("something took {} tries", 2);
}
} // namespace

REC_TEST("record/serialize - a recording survives a round trip through bytes")
{
    rec_fixture const fixture(deterministic_config());

    auto const original = capture(record_everything);
    REQUIRE(original.event_count() > 0);

    auto const bytes = cc::rec::serialize(original);
    REQUIRE(bytes.size() > 0);

    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();
    CHECK(!loaded.value().is_truncated());
    CHECK(r.event_count() == original.event_count());

    // Everything the query API can answer, it answers the same on both sides.
    CHECK(r.count("marked") == original.count("marked"));
    CHECK(r.first_value("count").value() == 7.0);
    CHECK(r.first_text("path").value() == "assets/tree.obj");
    CHECK(r.first_value("queue_depth").value() == 3.0);
    auto const messages = r.messages();
    auto const original_messages = original.messages();
    REQUIRE(messages.size() == original_messages.size());
    for (isize i = 0; i < messages.size(); ++i)
        CHECK(messages[i] == original_messages[i]);
    CHECK(r.scopes("outer").size() == 1);
}

REC_TEST("record/serialize - the descriptor's whole context comes back, not just its name")
{
    rec_fixture const fixture(deterministic_config());

    auto const bytes = cc::rec::serialize(capture(record_everything));
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto found_stat = false;
    auto found_log = false;
    loaded.value().events().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (cc::string_view(e.name()) == "bytes")
            {
                found_stat = true;
                CHECK(e.kind() == cc::rec::event_kind::stat_accumulate);

                // The unit travelled by value, so a reader that never linked against the recording binary still
                // knows how to render the number.
                REQUIRE(e.quantity() != nullptr);
                CHECK(cc::string_view(e.quantity()->symbol) == "B");
                CHECK(e.quantity()->prefix_base == 1024);

                // And so did the field layout.
                REQUIRE(e.fields().size() == 1);
                CHECK(cc::string_view(e.fields()[0].name) == "value");
            }

            if (e.kind() == cc::rec::event_kind::log)
            {
                found_log = true;
                CHECK(e.level() == cc::rec::level::warning);
                CHECK(cc::string_view(e.domain()->name()) == "default");

                // The source location is a rebuildable struct rather than a std::source_location, which is the only
                // reason a loaded recording can still say where it came from.
                CHECK(cc::string_view(e.site().file).contains("record-serialize-test"));
                CHECK(e.site().line > 0);
            }
        });

    CHECK(found_stat);
    CHECK(found_log);
}

REC_TEST("record/serialize - a loaded recording is a recording: it filters, decimates and replays")
{
    rec_fixture const fixture(deterministic_config());

    auto const bytes = cc::rec::serialize(capture(record_everything));
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();

    CHECK(r.of_kind(cc::rec::event_kind::marker).event_count() == 1);

    // By NAME, not by pointer: a loaded recording owns its own domain objects, so identity matches nothing on one.
    CHECK(r.from_domain("default").count("count") == 1);
    CHECK(r.from_domain(&cc::rec::g_default_domain).event_count() == 0);

    collector replayed;
    r.replay(replayed);
    CHECK(replayed.count_named("marked") == 1);
}

REC_TEST("record/serialize - several threads' streams survive together")
{
    rec_fixture const fixture(deterministic_config());

    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);

        CC_RECORD_MARK("from-main");
        std::thread worker(
            []
            {
                cc::rec::set_current_thread_record_name("worker");
                CC_RECORD_MARK("from-worker");
            });
        worker.join();

        cc::rec::flush_blocking();
    }

    auto const original = rl.take();
    CHECK(original.count("from-main") == 1);
    CHECK(original.count("from-worker") == 1);

    auto const bytes = cc::rec::serialize(original);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();
    CHECK(r.count("from-main") == 1);
    CHECK(r.count("from-worker") == 1);

    // The thread names come back too, which is what makes a multi-lane trace readable.
    auto const names_the_worker = [](cc::rec::recording const& rec)
    {
        auto found = false;
        rec.for_each_event([&](cc::rec::chunk_view const& v, cc::rec::event_view const& e)
                           { found |= cc::string_view(e.name()) == "from-worker" && v.thread.name == "worker"; });
        return found;
    };

    CHECK(names_the_worker(original)); // the capture named it
    CHECK(names_the_worker(r));        // and the round trip kept the name
}

REC_TEST("record/serialize - a file round-trips")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-serialize", ".ccrec");
    auto const original = capture(record_everything);

    REQUIRE(cc::rec::save_recording(original, path).has_value());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().events().event_count() == original.event_count());
    CHECK(loaded.value().cycles_per_second() > 0);
    CHECK(loaded.value().dumped_at_wall_secs() > 0);

    CHECK(cc::remove_file(path));
}

REC_TEST("record/serialize - garbage is refused rather than misread")
{
    rec_fixture const fixture(deterministic_config());

    // Too short for a header.
    CHECK(cc::rec::deserialize(cc::span<byte const>()).has_error());

    auto const good = cc::rec::serialize(capture(record_everything));
    REQUIRE(good.size() > 200);

    // Wrong magic.
    {
        auto bad = good;
        bad[0] = byte('X');
        CHECK(cc::rec::deserialize(bad).has_error());
    }

    // Wrong version: refusing beats guessing, for a format with no stability guarantee.
    {
        auto bad = good;
        auto const wrong = u32(cc::rec::serialized_version + 1);
        cc::memcpy(bad.data() + 8, &wrong, sizeof(wrong));
        CHECK(cc::rec::deserialize(bad).has_error());
    }

    // Truncated: every table is bounds-checked before an entry is read.
    {
        auto bad = cc::vector<byte>();
        for (isize i = 0; i < good.size() / 2; ++i)
            bad.push_back(good[i]);
        CHECK(cc::rec::deserialize(bad).has_error());
    }
}

//
// The crash path
//

REC_TEST("record/crash - the dump writes the same format, through the constrained writer")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash", ".ccrec");
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});
    CHECK(cc::rec::crash_dump_path() == path);

    record_everything();
    CC_RECORD_MARK("just-before-the-crash");

    // Exactly the path the crash handler takes, which is what makes the constrained writer testable at all rather
    // than something to find out about during a crash.
    REQUIRE(cc::rec::write_crash_dump_now());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();
    CHECK(r.count("just-before-the-crash") == 1);
    CHECK(r.count("marked") == 1);
    CHECK(r.first_value("count").value() == 7.0);
    CHECK(r.scopes("outer").size() == 1);

    CHECK(cc::remove_file(path));
}

REC_TEST("record/crash - the dump sees events no listener ever drained")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash-undrained", ".ccrec");
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});

    // No listener is registered and nothing is flushed, so a recording would have caught none of this.
    // The dump reads the chunks directly, which is the whole point on a path where nothing is going to run again.
    CC_RECORD_MARK("never-drained");

    REQUIRE(cc::rec::write_crash_dump_now());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().events().count("never-drained") == 1);

    CHECK(cc::remove_file(path));
}

REC_TEST("record/crash - an arena too small truncates the dump and says so")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash-small", ".ccrec");

    // Below the builder's floor, so it cannot even lay out its tables.
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 4096});

    CC_RECORD_MARK("will-not-fit");
    REQUIRE(cc::rec::write_crash_dump_now());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());

    // A truncated dump is still a valid file that says what it is, rather than a corrupt one or no file at all.
    CHECK(loaded.value().is_truncated());
    CHECK(loaded.value().events().event_count() == 0);

    CHECK(cc::remove_file(path));

    // Leave a usable arena behind for whatever runs next.
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});
}
