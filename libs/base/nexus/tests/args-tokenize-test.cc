#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// The quoting grammar, as a table.
// These rules are ours and must mean the same thing on every platform, so a response file written on one
// and read on another cannot change meaning — which is exactly what pinning them here is for.

using namespace cc::primitive_defines;

namespace
{
/// The tokens, joined with '|', so a whole expectation reads as one string.
cc::string joined(cc::string_view text)
{
    auto out = cc::string();
    for (auto const& token : nx::args_tokenize(text))
    {
        if (!out.empty())
            out += "|";

        out += token;
    }

    return out;
}

/// A file that removes itself, so a failing test does not leave one behind.
struct scratch_file
{
    cc::string path;

    explicit scratch_file(cc::string_view contents) : path(cc::temp_file_path("nx-args", ".rsp"))
    {
        auto adapter = cc::file_write_stream_adapter::create(path);
        REQUIRE(adapter.has_value());

        auto stream = adapter.value().stream();
        CHECK(stream.write(cc::as_bytes(contents)).has_value());
        CHECK(stream.flush().has_value());
    }

    ~scratch_file() { cc::remove_file(path); }

    scratch_file(scratch_file const&) = delete;
    scratch_file& operator=(scratch_file const&) = delete;

    [[nodiscard]] cc::string reference() const { return cc::string("@") + path; }
};
} // namespace

TEST("args tokenize - the basics")
{
    CHECK(joined("") == "");
    CHECK(joined("   ") == "");
    CHECK(joined("one") == "one");
    CHECK(joined("one two three") == "one|two|three");

    SECTION("every kind of whitespace separates")
    {
        CHECK(joined("one\ttwo\nthree\r\nfour") == "one|two|three|four");
        CHECK(joined("   leading and trailing   ") == "leading|and|trailing");
    }
}

TEST("args tokenize - quotes group")
{
    CHECK(joined("\"one two\"") == "one two");
    CHECK(joined("'one two'") == "one two");
    CHECK(joined("a \"b c\" d") == "a|b c|d");

    SECTION("a quote may sit inside a token")
    {
        CHECK(joined("--path=\"c:/program files/x\"") == "--path=c:/program files/x");
        CHECK(joined("pre\"in\"post") == "preinpost");
    }

    SECTION("an empty quoted string is a real, empty token")
    {
        // Losing this would make an intentionally empty argument unwritable.
        CHECK(nx::args_tokenize("--name \"\"").size() == 2);
        CHECK(nx::args_tokenize("--name \"\"")[1] == "");
    }

    SECTION("an unterminated quote runs to the end rather than failing")
    {
        CHECK(joined("a \"b c") == "a|b c");
    }
}

TEST("args tokenize - escapes, and where they do not apply")
{
    SECTION("inside double quotes")
    {
        CHECK(joined("\"a\\\"b\"") == "a\"b");
        CHECK(joined("\"a\\\\b\"") == "a\\b");
        CHECK(joined("\"a\\nb\"") == "a\nb");
        CHECK(joined("\"a\\tb\"") == "a\tb");
    }

    SECTION("inside single quotes nothing is an escape")
    {
        // Which is what lets a Windows path be pasted in unchanged.
        CHECK(joined("'c:\\temp\\new'") == "c:\\temp\\new");
        CHECK(joined("'a\\\"b'") == "a\\\"b");
    }
}

TEST("args tokenize - comments start only where a token would")
{
    CHECK(joined("a # b c") == "a");
    CHECK(joined("# whole line\nb") == "b");
    CHECK(joined("a\n# comment\nb") == "a|b");

    SECTION("a '#' inside a token is an ordinary character")
    {
        CHECK(joined("--tag=#1") == "--tag=#1");
        CHECK(joined("\"# not a comment\"") == "# not a comment");
    }
}

TEST("args tokenize - response files splice in place")
{
    auto const file = scratch_file("--jobs 8\n--verbose\n");

    auto jobs = 1;
    auto verbose = false;
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.enable_response_files();
    args.arg({"j", "jobs"}, jobs, "how many");
    args.arg({"v", "verbose"}, verbose, "print more");

    auto const reference = file.reference();
    CHECK(args.parse({cc::string_view(reference)}).ok());
    CHECK(jobs == 8);
    CHECK(verbose);
}

TEST("args tokenize - response files are opt-in")
{
    auto const file = scratch_file("--jobs 8\n");

    auto jobs = 1;
    auto files = cc::vector<cc::string>();
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.arg({"j", "jobs"}, jobs, "how many");
    args.positional("FILES", files, {.desc = "inputs"});

    // Without the opt-in a program taking user filenames would gain a file-read primitive nobody asked for.
    auto const reference = file.reference();
    CHECK(args.parse({cc::string_view(reference)}).ok());
    CHECK(jobs == 1);
    REQUIRE(files.size() == 1);
    CHECK(files[0] == reference);
}

TEST("args tokenize - an unreadable response file is an error, never a silent nothing")
{
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.enable_response_files();

    auto const r = args.parse({"@definitely-not-a-file.rsp"});
    CHECK(!r.ok());
    REQUIRE(r.has_diagnostics());
    CHECK(r.diagnostics()[0].source.origin == nx::arg_origin::response_file);
}

TEST("args tokenize - @@ is a literal @")
{
    auto name = cc::string();
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.enable_response_files();
    args.positional("NAME", name, {.desc = "who"});

    CHECK(args.parse({"@@user"}).ok());
    CHECK(name == "@user");
}

TEST("args tokenize - splicing stops at a bare --")
{
    auto tail = cc::vector<cc::string_view>();
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.enable_response_files();
    args.rest(tail, "ARGS");

    // Past the separator the tokens belong to another program, and rewriting them would change what it
    // is handed — including turning one of its arguments into a file read.
    CHECK(args.parse({"--", "@notafile", "x"}).ok());
    REQUIRE(tail.size() == 2);
    CHECK(tail[0] == "@notafile");
}

TEST("args tokenize - a nesting chain deeper than the cap is an error, not a hang")
{
    // A file that names itself: without a cap this never terminates.
    auto const path = cc::temp_file_path("nx-args-loop", ".rsp");
    {
        auto adapter = cc::file_write_stream_adapter::create(path);
        REQUIRE(adapter.has_value());
        auto stream = adapter.value().stream();
        auto const self = cc::string("@") + path;
        CHECK(stream.write(cc::as_bytes(cc::string_view(self))).has_value());
        CHECK(stream.flush().has_value());
    }

    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    args.enable_response_files(3);

    auto const reference = cc::string("@") + path;
    auto const r = args.parse({cc::string_view(reference)});
    CHECK(!r.ok());
    REQUIRE(r.has_diagnostics());
    CHECK(r.diagnostics()[0].message.contains("nested more than"));

    cc::remove_file(path);
}
