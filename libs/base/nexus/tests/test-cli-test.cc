#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/schedule.hh>

// The scheduler's own CLI, pinned.
//
// Written against the hand-rolled parser BEFORE it moved onto nx::args, so this file is the contract the
// migration had to satisfy rather than a description of what it happens to do now.
//
// It matters more than an ordinary suite: C++ TestMate drives this surface, and a regression here is an
// IDE that silently stops discovering tests rather than a red test.
// libs/base/nexus/docs/catch2-runner-compat.md is the prose half.

using namespace cc::primitive_defines;

namespace
{
/// A config parsed from a literal command line, argv[0] included as a real invocation has it.
nx::test_schedule_config parse(cc::span<char const* const> args)
{
    auto argv = cc::vector<char const*>();
    argv.push_back("nexus-test.exe");
    for (auto const* const a : args)
        argv.push_back(a);

    return nx::test_schedule_config::create_from_args(int(argv.size()), const_cast<char**>(argv.data()));
}
} // namespace

TEST("test cli - the defaults a real invocation starts from")
{
    auto const config = parse({});

    // Not the struct's own default of 1: a real run uses every core, and 0 means hardware concurrency.
    CHECK(config.jobs == 0);
    CHECK(!config.verbose);
    CHECK(config.filters.empty());
    CHECK(config.selected_bucket == nx::config::test_bucket::normal);
    CHECK(config.allow_cross_bucket_naming);
}

TEST("test cli - the bucket flags")
{
    CHECK(parse({"--manual"}).selected_bucket == nx::config::test_bucket::manual);
    CHECK(parse({"--pgo-benchmarks"}).selected_bucket == nx::config::test_bucket::pgo_benchmark);
    CHECK(parse({"--examples"}).selected_bucket == nx::config::test_bucket::example);

    // A bucket flag pins the sweep, so an exact name may no longer cross out of it.
    CHECK(!parse({"--manual"}).allow_cross_bucket_naming);
    CHECK(parse({}).allow_cross_bucket_naming);
}

TEST("test cli - jobs, in all three spellings")
{
    CHECK(parse({"--jobs", "4"}).jobs == 4);
    CHECK(parse({"-j", "4"}).jobs == 4);
    CHECK(parse({"-j4"}).jobs == 4);

    SECTION("zero is kept as-is, meaning hardware concurrency")
    {
        CHECK(parse({"-j0"}).jobs == 0);
    }

    SECTION("a bad count fails the parse rather than running anyway")
    {
        // The old parser warned and kept the default, which meant a mistyped -j ran the whole suite and
        // reported green.
        // Running "most of" what was asked for is the one outcome a typo must not produce.
        CHECK(parse({"-jabc"}).parse_failed);
        CHECK(parse({"--jobs", "abc"}).parse_failed);
        CHECK(parse({"--jobs", "-1"}).parse_failed);
    }

    SECTION("a good one does not")
    {
        CHECK(!parse({"-j4"}).parse_failed);
        CHECK(!parse({}).parse_failed);
    }
}

TEST("test cli - verbose and the recording flags")
{
    CHECK(parse({"-v"}).verbose);
    CHECK(parse({"--record"}).record_all);
    CHECK(parse({"--no-recording"}).no_recording);
}

TEST("test cli - section filters")
{
    auto const config = parse({"-c", "one", "-c", "two"});
    REQUIRE(config.section_filters.size() == 2);
    CHECK(config.section_filters[0] == "one");
    CHECK(config.section_filters[1] == "two");
}

TEST("test cli - the report file flags each consume their path")
{
    CHECK(parse({"--junit-xml", "out.xml"}).junit_xml_file == "out.xml");
    CHECK(parse({"--pgo-json", "out.json"}).pgo_json_file == "out.json");
    CHECK(parse({"--list-tests-json", "-"}).list_tests_json_file == "-");

    SECTION("the path is not also read as a filter")
    {
        // The whole reason these are consumed here rather than falling through.
        CHECK(parse({"--junit-xml", "out.xml"}).filters.empty());
    }
}

TEST("test cli - filter mode can be pinned either way")
{
    CHECK(parse({"--match-files"}).mode == nx::filter_mode::file);
    CHECK(parse({"--match-names"}).mode == nx::filter_mode::name);
}

TEST("test cli - positionals become filters, comma-split")
{
    SECTION("one at a time")
    {
        auto const config = parse({"alpha", "beta"});
        REQUIRE(config.filters.size() == 2);
        CHECK(config.filters[0] == "alpha");
        CHECK(config.filters[1] == "beta");
    }

    SECTION("or several in one argument, the Catch2 convention")
    {
        auto const config = parse({"alpha,beta,gamma"});
        REQUIRE(config.filters.size() == 3);
        CHECK(config.filters[2] == "gamma");
    }

    SECTION("empty pieces are dropped rather than becoming a filter that matches everything")
    {
        auto const config = parse({"alpha,,beta,"});
        REQUIRE(config.filters.size() == 2);
        CHECK(config.filters[0] == "alpha");
        CHECK(config.filters[1] == "beta");
    }
}

// --- the Catch2 compatibility surface, which C++ TestMate drives ---------------------------------------

TEST("test cli - discovery mode needs both --list-tests and --reporter")
{
    CHECK(parse({"--list-tests", "--reporter", "xml"}).is_catch2_xml_discovery);

    CHECK(!parse({"--list-tests"}).is_catch2_xml_discovery);
    CHECK(!parse({"--reporter", "xml"}).is_catch2_xml_discovery);
}

TEST("test cli - results mode is the reporter without the listing")
{
    CHECK(parse({"--reporter", "xml"}).report_catch2_xml_results);
    CHECK(!parse({"--list-tests", "--reporter", "xml"}).report_catch2_xml_results);
    CHECK(!parse({}).report_catch2_xml_results);
}

TEST("test cli - the flags accepted only so an invocation does not error")
{
    // --verbosity and --durations are consumed with their value and otherwise ignored.
    CHECK(parse({"--verbosity", "high"}).filters.empty());
    CHECK(parse({"--durations", "yes"}).filters.empty());

    // The reporter's own value is consumed too, rather than becoming a filter.
    CHECK(parse({"--reporter", "xml"}).filters.empty());
}

TEST("test cli - a Catch2-escaped bracket is unescaped in the compat modes")
{
    // A backslash makes the NEXT character literal, whatever it is — Catch2's own rule, rather than a
    // special case for brackets.
    // A filter that round-trips through an IDE has to mean the same thing on both sides.
    auto const compat = parse({"--reporter", "xml", "\\[tag\\]"});
    REQUIRE(compat.filters.size() == 1);
    CHECK(compat.filters[0] == "[tag]");

    SECTION("any escaped character, not only a bracket")
    {
        auto const other = parse({"--reporter", "xml", "a\\\\b"});
        REQUIRE(other.filters.size() == 1);
        CHECK(other.filters[0] == "a\\b");
    }

    SECTION("but a comma still separates, escaped or not")
    {
        // Splitting happens before unescaping, so `\,` is two filters rather than one literal comma.
        // Catch2 would read it as one; nexus has always split first, and changing that is its own change.
        auto const split = parse({"--reporter", "xml", "a\\,b"});
        CHECK(split.filters.size() == 2);
    }

    SECTION("and nothing is unescaped in an ordinary run")
    {
        auto const plain = parse({"\\[tag\\]"});
        REQUIRE(plain.filters.size() == 1);
        CHECK(plain.filters[0] == "\\[tag\\]");
    }
}

TEST("test cli - a filter that looks like a flag still filters")
{
    // The current parser treats anything it does not recognize as a filter, which is what lets a test
    // whose name begins with a dash be selected at all.
    auto const config = parse({"--not-a-real-flag"});
    REQUIRE(config.filters.size() == 1);
    CHECK(config.filters[0] == "--not-a-real-flag");
}

// --- what the migration onto nx::args added ------------------------------------------------------------

TEST("test cli - a command line for the test itself")
{
    SECTION("as one string, tokenized by the shared rules")
    {
        auto const config = parse({"--test-args", "--iterations 10 --name \"two words\""});
        REQUIRE(config.test_args.size() == 4);
        CHECK(config.test_args[0] == "--iterations");
        CHECK(config.test_args[1] == "10");
        CHECK(config.test_args[3] == "two words");
    }

    SECTION("or as everything after a bare --, already split")
    {
        auto const config = parse({"my-test", "--", "--iterations", "10"});
        REQUIRE(config.test_args.size() == 2);
        CHECK(config.test_args[0] == "--iterations");
        CHECK(config.test_args[1] == "10");

        // The separator does not swallow the filter that preceded it.
        REQUIRE(config.filters.size() == 1);
        CHECK(config.filters[0] == "my-test");
    }

    SECTION("and neither form leaks into the filters")
    {
        CHECK(parse({"--test-args", "--verbose"}).filters.empty());
        CHECK(parse({"--", "--verbose"}).filters.empty());
    }
}

TEST("test cli - the generated help still identifies the binary to C++ TestMate")
{
    auto const help = nx::test_schedule_config::cli_help_text();

    // Load-bearing: the extension scans --help output for this string, and without it an IDE silently
    // discovers no tests at all.
    // libs/base/nexus/docs/catch2-runner-compat.md is the contract.
    CHECK(help.contains("Compatible with Catch2 v3.11.0"));
}

TEST("test cli - help describes the flags the parser actually has")
{
    // The whole point of generating it: the block this replaced listed no flags whatsoever.
    auto const help = nx::test_schedule_config::cli_help_text();

    for (auto const* const flag :
         {"--manual", "--pgo-benchmarks", "--examples", "--jobs", "--junit-xml", "--pgo-json", "--list-tests-json",
          "--match-files", "--match-names", "--record", "--no-recording", "--list-tests", "--reporter", "--test-args"})
        CHECK(help.contains(flag));
}
