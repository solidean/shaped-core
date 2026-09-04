#include <babel-data/data/json.hh>
#include <clean-core/container/span.hh>
#include <clean-core/streams/growing_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// JSON, both directions.
//
// Reading and writing are two separate APIs that share no type, which is the point worth seeing here: a parsed
// `document` is read-once and offers no mutation, and the writer never builds one.
// So "load it, change one field, save it" is not what this library does — you read what you need and write what you
// want, and the two never meet in a mutable tree.

EXAMPLE("babel-data/json-read")
{
    auto const text = cc::string_view(R"({
        "name": "shaped-core",
        "version": 3,
        "tags": ["cpp", "graphics"],
        "build": {"debug": true, "jobs": 16}
    })");

    auto const doc = babel::json::read(text).value(); // a real caller checks has_value() instead
    auto const root = doc.root();

    cc::println("name    = {}", root["name"].as_string());
    cc::println("version = {}", root["version"].as_double());
    cc::println("jobs    = {}", root["build"]["jobs"].as_double());

    // arrays are indexed by position, objects by key; both are O(1) and O(members)
    for (auto i = isize(0); i < root["tags"].size(); ++i)
        cc::println("tag[{}]  = {}", i, root["tags"][i].as_string());

    // every accessor is kind-tolerant: a missing key or a wrong kind gives the fallback, never a crash
    cc::println("missing = {}", root["nope"].as_string("<absent>"));
    cc::println("wrong   = {}", root["name"].as_double(-1));
    cc::println("has ci  = {}", root.has("ci"));
}

EXAMPLE("babel-data/json-write")
{
    // string_writer owns the cc::string it writes into, and hands it over with no copy
    auto w = babel::json::string_writer({.indent = 2});

    {
        auto root = w.object();
        root.write("name", "shaped-core");
        root.write("version", 3);
        root.write("released", nullptr);

        {
            auto tags = root.write_array("tags");
            tags.write("cpp");
            tags.write("graphics");
        }

        // a scope closes when its handle dies, so the nesting in the source IS the nesting in the output
        auto build = root.write_object("build");
        build.write("debug", true);
        build.write("jobs", 16);
    }

    cc::println("{}", w.finish().value());
}

EXAMPLE("babel-data/json-imperative")
{
    // The RAII scopes are sugar: begin_* / end_* is the layer underneath, and it is the one to reach for when the
    // structure comes from somewhere a scope handle cannot follow — a visitor, a state machine, a recursive walk.
    auto w = babel::json::string_writer({.indent = 2});
    auto& j = w.underlying();

    struct node
    {
        char const* name;
        cc::span<node const> children;
    };
    node const leaves[] = {{.name = "vec"}, {.name = "mat"}};
    node const tree = {.name = "tg", .children = leaves};

    // a plain recursive function, which is exactly what an RAII handle cannot be threaded through cleanly
    auto const write_node = [&j](auto const& self, node const& n) -> void
    {
        j.begin_object();
        j.write("name", n.name);
        j.begin_array("children");
        for (auto const& c : n.children)
            self(self, c);
        j.end_array();
        j.end_object();
    };
    write_node(write_node, tree);

    cc::println("{}", w.finish().value());
}

EXAMPLE("babel-data/json-write-stream")
{
    // the writer only ever sees a cc::write_stream, so the sink is the caller's choice — here a growing byte buffer,
    // elsewhere a file or a socket, with no change to the writing code
    auto sink = cc::vector_write_stream_adapter();
    cc::write_stream stream = sink;

    {
        auto w = babel::json::writer(stream); // compact by default
        {
            auto root = w.array();
            for (auto i = 0; i < 4; ++i)
            {
                auto entry = root.write_object();
                entry.write("i", i);
                entry.write("square", i * i);
            }
        }

        // one place to check, because a failing write is recorded and every later write becomes a no-op
        if (auto const done = w.finish(); !done.has_value())
            cc::println("write failed: {}", done.error().to_string());
    }

    auto const bytes = sink.take();
    cc::println("{} bytes: {}", bytes.size(), cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
}

EXAMPLE("babel-data/json-write-numbers")
{
    auto const infinity = 1e308 * 10;
    auto const not_a_number = infinity - infinity;

    auto w = babel::json::string_writer({
        .indent = 2,
        // JSON has no NaN or infinity; the default fails the write rather than emitting something invalid
        .non_finite = babel::json::non_finite_policy::string,
    });

    {
        auto o = w.object();
        o.write("shortest", 0.1);                            // the shortest text that reads back as 0.1
        o.write("fixed", 0.1, cc::float_notation::fixed, 4); // one value, rendered its own way
        o.write("huge", u64(18446744073709551615ull));       // exact here, but past 2^53 - see the report
        o.write("nan", not_a_number);                        // -> "NaN", per the policy above
        o.write_raw("precomputed", R"({"already":"json"})"); // verbatim: never parsed, escaped or checked
        o.write_ascii("umlaut", "\xC3\xA4");                 // -> "\\u00e4", for an ASCII-only consumer
    }

    cc::println("{}", w.finish().value());

    // The document is valid and it is NOT what was written: report() is where that goes, since it is not an error.
    auto const report = w.report();
    cc::println("non-finite: {}, past 2^53: {}, clean: {}", report.non_finite, report.large_integers, report.is_clean());
}

EXAMPLE("babel-data/json-newline-delimited")
{
    // one record per line, which is what a log or a stream of events wants
    auto w = babel::json::string_writer({.newline_delimited = true});

    for (auto const* const level : {"info", "warn", "error"})
    {
        auto e = w.object();
        e.write("level", level);
        e.write("msg", "something happened");
    }

    cc::print("{}", w.finish().value());
}

EXAMPLE("babel-data/json-round-trip")
{
    // write with one API, read back with the other — there is no shared document in between
    auto w = babel::json::string_writer();
    {
        auto o = w.object();
        o.write("msg", "hello \"world\"\n");
        auto nums = o.write_array("nums");
        for (auto i = 0; i < 3; ++i)
            nums.write(i * 1.5);
    }
    auto const text = w.finish().value();
    cc::println("wrote: {}", text);

    auto const doc = babel::json::read(text).value();
    auto const root = doc.root();
    cc::println("read msg    = {}", root["msg"].as_string()); // escapes came back as the original bytes
    cc::println("read nums[2] = {}", root["nums"][2].as_double());
}
