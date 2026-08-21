#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/record/crash_dump.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/pinned_value.hh>
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
///
/// Passed to `capture` as `&record_everything` rather than by name: a cc::function_ref stores its callable as a
/// `void*`, and a function NAME binds as a function lvalue whose address is a function pointer — which clang refuses
/// to put in a `void*` and MSVC accepts as an extension.
/// A pointer VARIABLE has an object address, so taking it explicitly is what makes the call portable.
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

    auto const original = capture(&record_everything);
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

    auto const bytes = cc::rec::serialize(capture(&record_everything));
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

    auto const bytes = cc::rec::serialize(capture(&record_everything));
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
    auto const original = capture(&record_everything);

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

    auto const good = cc::rec::serialize(capture(&record_everything));
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

REC_TEST("record/serialize - a recording carries the modules its addresses mean")
{
    if (!cc::module_enumeration_available())
        SKIP("no module enumeration on this platform");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { CC_LOG_ERROR("something went wrong"); });
    REQUIRE(r.event_count() > 0);

    // A live recording carries none: its addresses mean this process, which is still here to ask.
    CHECK(r.modules().empty());

    auto const bytes = cc::rec::serialize(r);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    // Read back, it carries the table — because by then the process that gave those addresses meaning may be gone,
    // which for a crash dump it always is.
    auto const modules = loaded.value().events().modules();
    REQUIRE(!modules.empty());

    auto const self = reinterpret_cast<u64>(&cc::rec::serialize);
    auto found = false;
    for (auto const& m : modules)
        if (m.contains(self))
        {
            found = true;
            CHECK(!m.path.empty());
            CHECK(!m.identity.empty());
        }

    CHECK(found);
}

REC_TEST("record/serialize - a loaded recording symbolizes its own stacks")
{
    if (!cc::module_enumeration_available() || !cc::symbolizer::is_available())
        SKIP("no module enumeration or no symbolization on this platform");

    rec_fixture const fixture(deterministic_config());

    // An error log captures a stack, which is the payload that is worthless without a module table.
    auto const r = capture([] { CC_LOG_ERROR("a failure worth a stack"); });
    auto const bytes = cc::rec::serialize(r);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    auto const& events = loaded.value().events();
    cc::symbolizer sym(events.modules());
    CHECK(sym.is_foreign());

    isize frames = 0;
    isize with_module = 0;
    events.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (cc::string_view(e.desc->name) != "record.stacktrace")
                return;

            for (auto const address : e.field_as_u64_array("frames"))
            {
                ++frames;
                if (!sym.resolve(reinterpret_cast<void const*>(address)).module.empty())
                    ++with_module;
            }
        });

    if (frames == 0)
        SKIP("this build captured no frames");

    // Every frame resolves at least to a module and an offset, which comes from the TABLE rather than from anything
    // this process has loaded — the whole point of carrying it.
    CHECK(with_module == frames);
}

// A chunk's preamble is the one payload holding a POINTER into this binary — a `desc_ref` naming an open scope's site.
// Both writers have to rewrite it into a table index and the loader has to rewrite it back, exactly as an event's own
// descriptor is, or a loaded preamble would name whatever happens to live at that address.
// These are what pin that, one per writer, since serialize() patches in place and the crash dump streams.

namespace
{
/// The deepest scope name any chunk preamble in `r` claims was already open, or empty.
[[nodiscard]] cc::string outermost_named_scope(cc::rec::recording const& r)
{
    cc::string found;
    r.for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.kind() != cc::rec::event_kind::stream_state)
                return;
            if (e.field_as_u64("named_scopes").value_or(0) == 0)
                return;

            auto const* const outer = e.field_as_desc("scope0");
            if (outer != nullptr)
                found = cc::string(cc::string_view(outer->name));
        });
    return found;
}

/// Rotates several chunks with a scope open, so at least one preamble names it.
void fill_chunks_inside_a_scope()
{
    CC_RECORD_SCOPE("long-lived-frame");
    for (isize i = 0; i < 8000; ++i)
        CC_RECORD_MARK("filler");
}
} // namespace

REC_TEST("record/serialize - a preamble's open scope survives the round trip")
{
    rec_fixture const fixture(deterministic_config());

    auto const live = capture([] { fill_chunks_inside_a_scope(); });
    REQUIRE(outermost_named_scope(live) == "long-lived-frame");

    auto const bytes = cc::rec::serialize(live);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    // Resolved against the LOADED recording's own descriptor table, since the process that gave the pointer meaning
    // is exactly what a round trip is meant to survive without.
    CHECK(outermost_named_scope(loaded.value().events()) == "long-lived-frame");
}

REC_TEST("record/crash - a preamble's open scope survives the dump's streaming writer")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash-preamble", ".ccrec");
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});

    fill_chunks_inside_a_scope();
    REQUIRE(cc::rec::write_crash_dump_now());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());

    // The dump writes each event's bytes straight out rather than copying the block, so its patching is a separate
    // path from serialize()'s and needs its own assertion.
    CHECK(outermost_named_scope(loaded.value().events()) == "long-lived-frame");

    CHECK(cc::remove_file(path));
}

REC_TEST("record/serialize - a literal value survives the round trip as text")
{
    rec_fixture const fixture(deterministic_config());

    auto const live = capture(
        []
        {
            CC_RECORD("asset", "meshes/tree.obj");
            CC_RECORD("runtime", cc::string("built at runtime"));
        });

    auto const bytes = cc::rec::serialize(live);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    // The literal was written as an ADDRESS, which means nothing in the loading process — so the writer had to route
    // it through the string table and the loader had to give it an arena of its own.
    // Reading the pointer back verbatim would `strlen` a dead address, which is why this asserts on the text.
    auto const& r = loaded.value().events();
    CHECK(r.first_text("asset").value() == "meshes/tree.obj");
    CHECK(r.first_text("runtime").value() == "built at runtime");
}

REC_TEST("record/crash - a dump carrying a relation is not shifted by its own table")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash-relation", ".ccrec");
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});

    // The relation table sits between the units and the fields in the layout finish() computes.
    // The dump writer used to skip it, so every section after `units` landed short by however many bytes it held —
    // invisible while no dump contained a relation, and a corrupt file the moment one did.
    auto const parent = cc::rec::new_trace_id();
    auto const child = cc::rec::new_trace_id();
    CC_RECORD_RELATION(cc::rec::relation_parent_of, parent, child);
    CC_RECORD_MARK("after-the-relation");

    REQUIRE(cc::rec::write_crash_dump_now());

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());

    auto const& r = loaded.value().events();
    CHECK(r.count("after-the-relation") == 1);

    auto const relations = r.trace_relations();
    REQUIRE(relations.size() == 1);
    CHECK(cc::string_view(relations[0].type->name) == "parent_of");
    CHECK(relations[0].subject() == parent);
    REQUIRE(relations[0].objects().size() == 1);
    CHECK(relations[0].objects()[0] == child);

    CHECK(cc::remove_file(path));
}

REC_TEST("record/serialize - pinned bytes travel in the file's blob section")
{
    rec_fixture const fixture(deterministic_config());

    auto const live = capture(
        []
        {
            auto payload = cc::vector<byte>();
            payload.resize_to_constructed(2048, byte(0x5A));
            payload[0] = byte(0x11);
            payload[2047] = byte(0x22);

            auto const pinned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
            CC_RECORD_PINNED("frame.depth", pinned);
        });

    auto const bytes = cc::rec::serialize(live);
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());

    // A live recording's payload holds the caller's ADDRESS, which means nothing here — so the writer copied the bytes
    // into a section of its own and the loader pointed the slot at its copy.
    isize seen = 0;
    loaded.value().events().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.name() != "frame.depth")
                return;

            ++seen;
            auto const got = e.field_as_bytes("value");
            REQUIRE(got.size() == 2048);
            CHECK(got[0] == byte(0x11));
            CHECK(got[2047] == byte(0x22));
            CHECK(got[1000] == byte(0x5A));
        });

    CHECK(seen == 1);

    // The file has to be at least the blob it carries, which is what says the bytes really travelled.
    CHECK(bytes.size() > 2048);
}

REC_TEST("record/crash - pinned bytes survive the dump's streaming writer")
{
    rec_fixture const fixture(deterministic_config());

    auto const path = cc::temp_file_path("cc-record-crash-pinned", ".ccrec");
    cc::rec::install_crash_dump({.path = path, .arena_bytes = 2 << 20});

    {
        auto payload = cc::vector<byte>();
        payload.resize_to_constructed(512, byte(0x7E));

        auto const pinned = cc::make_pinned_data(cc::move(payload)).reinterpret_as<byte const>();
        CC_RECORD_PINNED("dumped.buffer", pinned);

        // Still pinned when the dump runs, which is what it reads the bytes through.
        REQUIRE(cc::rec::write_crash_dump_now());
    }

    auto loaded = cc::rec::load_recording(path);
    REQUIRE(loaded.has_value());

    isize seen = 0;
    loaded.value().events().for_each_event(
        [&](cc::rec::chunk_view const&, cc::rec::event_view const& e)
        {
            if (e.name() != "dumped.buffer")
                return;

            ++seen;
            auto const got = e.field_as_bytes("value");
            REQUIRE(got.size() == 512);
            CHECK(got[0] == byte(0x7E));
            CHECK(got[511] == byte(0x7E));
        });

    CHECK(seen == 1);
    CHECK(cc::remove_file(path));
}
