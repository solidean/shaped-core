#include <babel-serializer/data/json.hh>
#include <babel-serializer/trace/chrome_trace.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/map.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/trace.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The recording system is a process-wide singleton, so every test here holds one exclusion tag and none overlap.
// These stand the cc::rec singleton up themselves, so they run alone and take the run's recorder over for the
// duration — see libs/base/clean-core/docs/systems/recording.md, "Lifecycle constraints".
#define TRACE_TEST(name_) TEST(name_, nx::config::exclusive(), nx::config::owns_recorder)

namespace
{
struct rec_fixture
{
    rec_fixture()
    {
        auto cfg = cc::rec::config{};
        cfg.threaded = false;
        cfg.chunk_bytes = 64 * 1024;
        cfg.budget_bytes = 4 * 1024 * 1024;
        cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
        cc::rec::initialize(cfg);
    }
    ~rec_fixture() { cc::rec::shutdown(); }

    rec_fixture(rec_fixture const&) = delete;
    rec_fixture& operator=(rec_fixture const&) = delete;
};

/// Captures what `body` records, narrowed to the thread that ran it.
cc::rec::recording capture(cc::function_ref<void()> body)
{
    cc::rec::recording_listener rl;
    auto const h = cc::rec::register_listener(rl);
    body();
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(h);
    return rl.take().from_thread(cc::current_thread_id());
}

/// Captures every thread, which is what a sampled recording needs: the samples live on the SAMPLER's thread until
/// they are spliced, so narrowing to the calling thread throws them away.
cc::rec::recording capture_all(cc::function_ref<void()> body)
{
    cc::rec::recording_listener rl;
    auto const h = cc::rec::register_listener(rl);
    body();
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(h);
    return rl.take();
}

cc::string encode_to_string(cc::rec::recording const& r, babel::chrome_trace::write_options opts = {})
{
    auto encoded = babel::chrome_trace::encode(r, opts);
    REQUIRE(encoded.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(encoded.value().data()), encoded.value().size()));
}

/// How many trace events the JSON holds, by parsing it rather than by counting braces.
isize trace_event_count(cc::string_view json)
{
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());
    return doc.value().root()["traceEvents"].size();
}
} // namespace

TRACE_TEST("chrome_trace - the output parses as JSON and carries the expected envelope")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD_MARK("something-happened"); });
    auto const json = encode_to_string(r);

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto const root = doc.value().root();
    CHECK(root["displayTimeUnit"].as_string() == "ms");
    CHECK(root["traceEvents"].is_array());
    CHECK(root["traceEvents"].size() > 0);
}

TRACE_TEST("chrome_trace - scopes become B/E pairs and stats become counters")
{
    rec_fixture const fixture;

    auto const r = capture(
        []
        {
            {
                CC_RECORD_SCOPE("outer");
                CC_RECORD_STAT("depth", cc::rec::unit_count, 3);
            }
        });

    auto const json = encode_to_string(r);
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    isize begins = 0;
    isize ends = 0;
    isize counters = 0;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
    {
        auto const ph = traceEvents[i]["ph"].as_string();
        begins += ph == "B" ? 1 : 0;
        ends += ph == "E" ? 1 : 0;
        counters += ph == "C" ? 1 : 0;
    }

    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK(counters == 1);
}

TRACE_TEST("chrome_trace - an accumulate becomes a running total, not the raw delta")
{
    rec_fixture const fixture;

    auto const r = capture(
        []
        {
            CC_RECORD_ACCUM("bytes", cc::rec::unit_bytes, 100);
            CC_RECORD_ACCUM("bytes", cc::rec::unit_bytes, 250);
        });

    auto const json = encode_to_string(r);
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    cc::vector<f64> levels;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
        if (traceEvents[i]["ph"].as_string() == "C")
            levels.push_back(traceEvents[i]["args"]["value"].as_double());

    // A counter track shows a level, so the deltas are summed on the way out.
    REQUIRE(levels.size() == 2);
    CHECK(levels[0] == 100.0);
    CHECK(levels[1] == 350.0);
}

TRACE_TEST("chrome_trace - a log message renders its formatted text, and the domain becomes the category")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_LOG_WARNING("fell back after {} tries", 3); });
    auto const json = encode_to_string(r);

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto found = false;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
    {
        auto const e = traceEvents[i];
        if (e["ph"].as_string() != "i" || e["name"].as_string() != "fell back after 3 tries")
            continue;

        found = true;
        CHECK(e["cat"].as_string() == "default");
        CHECK(e["args"]["level"].as_string() == "warning");
    }
    CHECK(found);
}

TRACE_TEST("chrome_trace - a value's declared fields travel into args")
{
    rec_fixture const fixture;

    auto const r = capture(
        []
        {
            CC_RECORD("vertex_count", 1024);
            CC_RECORD("asset_path", "meshes/tree.obj");
        });

    auto const json = encode_to_string(r);
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto saw_number = false;
    auto saw_text = false;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
    {
        auto const e = traceEvents[i];
        if (e["name"].as_string() == "vertex_count")
        {
            saw_number = true;
            CHECK(e["args"]["value"].as_double() == 1024.0);
        }
        if (e["name"].as_string() == "asset_path")
        {
            saw_text = true;
            CHECK(e["args"]["value"].as_string() == "meshes/tree.obj");
        }
    }
    CHECK(saw_number);
    CHECK(saw_text);
}

TRACE_TEST("chrome_trace - the recorder's own bookkeeping is excluded unless asked for")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD_MARK("mine"); });

    // The chunk acquisition that got the recording started is in there, and is noise by default.
    REQUIRE(r.count("record.chunk_acquired") >= 1);

    auto const without = trace_event_count(encode_to_string(r));
    auto const with = trace_event_count(encode_to_string(r, {.include_system_events = true}));
    CHECK(with > without);
}

TRACE_TEST("chrome_trace - what is included is configurable, and an empty recording still parses")
{
    rec_fixture const fixture;

    auto const r = capture(
        []
        {
            CC_RECORD_SCOPE("scoped");
            CC_LOG_INFO("logged");
            CC_RECORD("valued", 1);
            CC_RECORD_STAT("statted", cc::rec::unit_count, 1);
        });

    auto const everything = trace_event_count(encode_to_string(r));
    auto const nothing = trace_event_count(encode_to_string(
        r, {.include_logs = false, .include_values = false, .include_stats = false, .include_scopes = false}));

    CHECK(everything > nothing);

    // Only the process and thread metadata survive, and the result is still valid JSON.
    CHECK(nothing >= 1);

    auto const empty = encode_to_string(cc::rec::recording{});
    CHECK(babel::json::read(empty).has_value());
}

TRACE_TEST("chrome_trace - a relation carries its meaning and its members")
{
    rec_fixture const fixture;

    auto const a = cc::rec::new_trace_id();
    auto const b = cc::rec::new_trace_id();
    auto const c = cc::rec::new_trace_id();

    auto const r = capture([&] { CC_RECORD_RELATION(cc::rec::relation_same_key_as, a, b, c); });
    auto const json = encode_to_string(r);

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto found = false;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
    {
        auto const e = traceEvents[i];
        if (e["name"].as_string() != "same_key_as")
            continue;

        found = true;
        CHECK(e["relation"].as_string() == "same_key_as");

        // The flag a reconstruction can act on directly travels with the edge.
        auto const is_equivalence = e["equivalence"].as_bool();
        CHECK(is_equivalence);

        // Members come out as an array, and a trace id goes out as a STRING rather than a lossy JSON number.
        auto const members = e["args"]["members"];
        REQUIRE(members.is_array());
        CHECK(members.size() == 3);
        // Three ids, each either a plain number or a string when it is too large for a JSON number to hold.
        auto const renderable = members[0].is_string() || members[0].is_number();
        CHECK(renderable);
    }
    CHECK(found);
}

TRACE_TEST("chrome_trace - text that needs escaping survives the round trip")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD("quoted", "a \"quoted\" \\ path\nwith a newline"); });

    auto const json = encode_to_string(r);
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto found = false;
    auto const traceEvents = doc.value().root()["traceEvents"];
    for (isize i = 0; i < traceEvents.size(); ++i)
        if (traceEvents[i]["name"].as_string() == "quoted")
        {
            found = true;
            CHECK(traceEvents[i]["args"]["value"].as_string() == "a \"quoted\" \\ path\nwith a newline");
        }
    CHECK(found);
}

TRACE_TEST("chrome_trace - no samples means no sampled track")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD_MARK("anchor"); });
    auto const json = encode_to_string(r);

    // The reconstruction must not invent a track for a recording that was never sampled.
    CHECK(!json.contains("(sampled)"));
    CHECK(!json.contains(R"("cat":"sampled")"));
}

TRACE_TEST("chrome_trace - sampled stacks become spans inside the scopes that were open")
{
    if (!cc::stack_capture_from_context_available() || !CC_HAS_THREADS)
        SKIP("this build has no sampler — no foreign-thread walk, or no threads at all");

    rec_fixture const fixture;

    auto const raw = capture_all(
        [&]
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 2000.0});

            CC_RECORD_MARK("work-begins"); // joins this thread to the set the sampler knows

            auto const start = cc::current_time_steady_secs();
            u64 volatile sink = 0;
            while (cc::current_time_steady_secs() - start < 0.2)
            {
                CC_RECORD_SCOPE("sampled-region");
                for (int i = 0; i < 4096; ++i)
                    sink = sink + u64(i);
            }
        });

    REQUIRE(raw.count_of_kind(cc::rec::event_kind::sample) > 0);

    // Splicing first is the realistic path: it moves each sample onto the thread it caught, so the exporter can nest
    // it inside that thread's scopes rather than beside them.
    auto const r = raw.spliced_samples();
    auto const json = encode_to_string(r);

    CHECK(json.contains(R"("cat":"sampled")"));

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    // Every B is closed, on every track.
    // An unbalanced reconstruction is one a viewer renders as nonsense rather than rejecting, which is why this is
    // worth asserting rather than eyeballing.
    cc::map<f64, int> depth_by_tid;
    auto sampled_spans = 0;
    auto const events = doc.value().root()["traceEvents"];
    for (isize i = 0; i < events.size(); ++i)
    {
        auto const e = events[i];
        auto const phase = e["ph"].as_string();
        if (phase != "B" && phase != "E")
            continue;

        auto& depth = depth_by_tid[e["tid"].as_double()];
        if (phase == "B")
        {
            ++depth;
            if (e["cat"].as_string() == "sampled")
                ++sampled_spans;
        }
        else
            --depth;

        CHECK(depth >= 0); // an E with nothing open means the nesting is wrong
    }

    for (auto const& [tid, depth] : depth_by_tid)
        CHECK(depth == 0);

    CHECK(sampled_spans > 0);
}

TRACE_TEST("chrome_trace - sampled frames carry names and source locations")
{
    if (!cc::stack_capture_from_context_available() || !CC_HAS_THREADS || !cc::symbolizer::is_available())
        SKIP("this build cannot sample or cannot symbolize");

    rec_fixture const fixture;

    auto const raw = capture_all(
        [&]
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 2000.0});
            CC_RECORD_MARK("work-begins");

            auto const start = cc::current_time_steady_secs();
            u64 volatile sink = 0;
            while (cc::current_time_steady_secs() - start < 0.2)
                for (int i = 0; i < 4096; ++i)
                    sink = sink + u64(i);
        });

    REQUIRE(raw.count_of_kind(cc::rec::event_kind::sample) > 0);
    auto const json = encode_to_string(raw.spliced_samples());

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    // A resolved frame is named and carries where it came from; an unresolved one keeps its address rather than
    // acquiring a confident wrong name, so both shapes are legitimate and only "no address at all" is a bug.
    auto sampled = 0;
    auto named = 0;
    auto with_address = 0;
    auto const events = doc.value().root()["traceEvents"];
    for (isize i = 0; i < events.size(); ++i)
    {
        auto const e = events[i];
        if (e["ph"].as_string() != "B" || e["cat"].as_string() != "sampled")
            continue;

        ++sampled;
        if (!e["name"].as_string().starts_with("0x"))
            ++named;
        if (!e["args"]["address"].as_string().empty())
            ++with_address;
    }

    REQUIRE(sampled > 0);

    // The address survives symbolization, always: a name can then be checked, and an unresolved frame still points
    // somewhere.
    // `named` is asserted only where the build has symbols — a release preset ships without debug info, and resolving
    // nothing is the correct answer there rather than a failure.
    CHECK(with_address == sampled);
    if (cc::symbolizer().resolve(reinterpret_cast<void const*>(&babel::chrome_trace::encode)).has_function())
        CHECK(named > 0);

    // Turning it off gets the addresses back.
    // Checked per SPAN rather than over the whole document, because scopes carry a source of their own.
    auto const bare_doc = babel::json::read(encode_to_string(raw.spliced_samples(), {.symbolize_samples = false}));
    REQUIRE(bare_doc.has_value());

    auto bare_sampled = 0;
    auto const bare_events = bare_doc.value().root()["traceEvents"];
    for (isize i = 0; i < bare_events.size(); ++i)
    {
        auto const e = bare_events[i];
        if (e["ph"].as_string() != "B" || e["cat"].as_string() != "sampled")
            continue;

        ++bare_sampled;
        CHECK(e["name"].as_string().starts_with("0x"));
        CHECK(!e["args"]["source"].is_valid());
    }

    CHECK(bare_sampled > 0);
}

TRACE_TEST("chrome_trace - a scope says where it was recorded")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD_SCOPE("located-scope"); });
    auto const json = encode_to_string(r);

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    // Every descriptor carries its site, and a scope that does not report it makes a reader hunt for the code by name.
    auto found = false;
    auto const events = doc.value().root()["traceEvents"];
    for (isize i = 0; i < events.size(); ++i)
    {
        auto const e = events[i];
        if (e["ph"].as_string() != "B" || e["name"].as_string() != "located-scope")
            continue;

        auto const source = e["args"]["source"].as_string();
        CHECK(source.contains("chrome_trace-test.cc"));
        CHECK(source.contains(":")); // path and line, not just a path

        // The whole path, so it can be opened rather than guessed at — two same-named files in different directories
        // are otherwise indistinguishable.
        CHECK(source.size() > cc::string_view("chrome_trace-test.cc").size() + 4);
        found = true;
    }

    CHECK(found);
}

TRACE_TEST("chrome_trace - a counter's args stay numeric")
{
    rec_fixture const fixture;

    auto const r = capture([] { CC_RECORD_STAT("queue_depth", cc::rec::unit_count, 7); });
    auto const json = encode_to_string(r);

    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    // A counter's args ARE its data series, so a source string among them would corrupt the plot rather than
    // annotate it.
    // That is why the source goes on scopes and instants and deliberately not here.
    auto found = false;
    auto const events = doc.value().root()["traceEvents"];
    for (isize i = 0; i < events.size(); ++i)
    {
        auto const e = events[i];
        if (e["ph"].as_string() != "C")
            continue;

        found = true;

        // A MISSING key is an invalid ref rather than a null one, which is the distinction that matters here: the
        // source must not be there at all, not be there holding null.
        CHECK(!e["args"]["source"].is_valid());
        CHECK(e["args"]["value"].is_number());
    }

    CHECK(found);
}

TRACE_TEST("chrome_trace - a recording read back from bytes still resolves its own frames")
{
    if (!cc::stack_capture_from_context_available() || !CC_HAS_THREADS || !cc::symbolizer::is_available()
        || !cc::module_enumeration_available())
        SKIP("this build cannot sample, symbolize, or enumerate modules");

    rec_fixture const fixture;

    auto const raw = capture_all(
        [&]
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 2000.0});
            CC_RECORD_MARK("work-begins");

            auto const start = cc::current_time_steady_secs();
            u64 volatile sink = 0;
            while (cc::current_time_steady_secs() - start < 0.2)
                for (int i = 0; i < 4096; ++i)
                    sink = sink + u64(i);
        });

    REQUIRE(raw.count_of_kind(cc::rec::event_kind::sample) > 0);

    // Through bytes and back, which is the path a crash dump takes and the one where the addresses stop meaning
    // anything on their own.
    auto const bytes = cc::rec::serialize(raw.spliced_samples());
    auto loaded = cc::rec::deserialize(bytes);
    REQUIRE(loaded.has_value());
    REQUIRE(!loaded.value().events().modules().empty());

    auto const json = encode_to_string(loaded.value().events());
    auto const doc = babel::json::read(json);
    REQUIRE(doc.has_value());

    auto sampled = 0;
    auto located = 0;
    auto const events = doc.value().root()["traceEvents"];
    for (isize i = 0; i < events.size(); ++i)
    {
        auto const e = events[i];
        if (e["ph"].as_string() != "B" || e["cat"].as_string() != "sampled")
            continue;

        ++sampled;
        if (!e["args"]["module"].as_string().empty())
            ++located;
    }

    REQUIRE(sampled > 0);

    // A module for every frame, from the recording's own table rather than from whatever this process has loaded.
    // That is what a dump from a machine you do not have needs, and all it needs.
    CHECK(located == sampled);
}
