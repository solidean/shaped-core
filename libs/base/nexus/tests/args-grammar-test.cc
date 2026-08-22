#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// The token grammar, rule by rule.
// libs/base/nexus/docs/args.md states these as a spec; this file is what holds the implementation to it.

using namespace cc::primitive_defines;

namespace
{
/// Every test here parses a literal command line and asserts on bound variables, so a builder that never
/// prints and never sniffs the terminal is the only thing needed.
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t", .description = "a test program"});
    args.no_auto_print();
    return args;
}

nx::args_result parse(nx::args_builder& args, cc::span<cc::string_view const> tokens)
{
    return args.parse(tokens);
}
} // namespace

TEST("args grammar - long names, both value forms")
{
    auto jobs = 1;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    SECTION("--jobs 8")
    {
        CHECK(parse(args, {"--jobs", "8"}).ok());
        CHECK(jobs == 8);
    }

    SECTION("--jobs=8")
    {
        CHECK(parse(args, {"--jobs=8"}).ok());
        CHECK(jobs == 8);
    }

    SECTION("absent leaves the variable's own initializer")
    {
        CHECK(parse(args, {}).ok());
        CHECK(jobs == 1);
    }
}

TEST("args grammar - short names, fused and separate")
{
    auto jobs = 1;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    SECTION("-j 8")
    {
        CHECK(parse(args, {"-j", "8"}).ok());
        CHECK(jobs == 8);
    }

    SECTION("-j8 fused")
    {
        CHECK(parse(args, {"-j8"}).ok());
        CHECK(jobs == 8);
    }

    SECTION("-j=8 is rejected, and says what to write instead")
    {
        auto const r = parse(args, {"-j=8"});
        CHECK(!r.ok());
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::invalid_value);
        CHECK(r.diagnostics()[0].message.contains("-j8"));
    }
}

TEST("args grammar - short flags cluster")
{
    auto a = false;
    auto b = false;
    auto c = false;
    auto args = make_args();
    args.arg({"a"}, a, "first");
    args.arg({"b"}, b, "second");
    args.arg({"c"}, c, "third");

    CHECK(parse(args, {"-abc"}).ok());
    CHECK(a);
    CHECK(b);
    CHECK(c);
}

TEST("args grammar - a value taker consumes the rest of its cluster")
{
    auto verbose = false;
    auto jobs = 1;
    auto args = make_args();
    args.arg({"v"}, verbose, "loud");
    args.arg({"j"}, jobs, "how many");

    SECTION("the value is fused to the cluster")
    {
        CHECK(parse(args, {"-vj8"}).ok());
        CHECK(verbose);
        CHECK(jobs == 8);
    }

    SECTION("the value is the next token")
    {
        CHECK(parse(args, {"-vj", "8"}).ok());
        CHECK(verbose);
        CHECK(jobs == 8);
    }
}

TEST("args grammar - bools take no value in the space form")
{
    auto force = false;
    auto rest = cc::vector<cc::string>();
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it anyway");
    args.positional("FILES", rest, {.desc = "inputs"});

    SECTION("the next token stays a positional")
    {
        CHECK(parse(args, {"--force", "somefile"}).ok());
        CHECK(force);
        REQUIRE(rest.size() == 1);
        CHECK(rest[0] == "somefile");
    }

    SECTION("but an explicit = does set it, which is unambiguous")
    {
        CHECK(parse(args, {"--force=false"}).ok());
        CHECK(!force);
    }

    SECTION("the CLI spellings are all accepted")
    {
        CHECK(parse(args, {"--force=yes"}).ok());
        CHECK(force);
    }
}

TEST("args grammar - negatable bools")
{
    auto color = true;
    auto args = make_args();
    args.arg({"color"}, color, {.desc = "colorize", .negatable = true});

    CHECK(parse(args, {"--no-color"}).ok());
    CHECK(!color);

    color = false;
    CHECK(parse(args, {"--color"}).ok());
    CHECK(color);
}

TEST("args grammar - a counting flag counts")
{
    auto verbosity = 0;
    auto args = make_args();
    args.count({"v", "verbose"}, verbosity, "louder each time");

    SECTION("clustered")
    {
        CHECK(parse(args, {"-vvv"}).ok());
        CHECK(verbosity == 3);
    }

    SECTION("repeated separately, short and long mixed")
    {
        CHECK(parse(args, {"-v", "--verbose", "-v"}).ok());
        CHECK(verbosity == 3);
    }
}

TEST("args grammar - a vector accumulates, a scalar takes the last")
{
    auto includes = cc::vector<cc::string>();
    auto output = cc::string("a.out");
    auto args = make_args();
    args.arg({"I", "include"}, includes, "add a search path");
    args.arg({"o", "output"}, output, "where to write");

    CHECK(parse(args, {"-I", "one", "-I", "two", "-o", "x", "-o", "y"}).ok());
    REQUIRE(includes.size() == 2);
    CHECK(includes[0] == "one");
    CHECK(includes[1] == "two");
    CHECK(output == "y"); // last wins, because scripts build command lines by appending overrides
}

TEST("args grammar - a negative number is a value, not a cluster")
{
    auto offset = 0;
    auto values = cc::vector<cc::string>();
    auto args = make_args();
    args.arg({"o", "offset"}, offset, "how far");
    args.positional("VALUES", values, {.desc = "inputs"});

    SECTION("as an option's value")
    {
        CHECK(parse(args, {"--offset", "-5"}).ok());
        CHECK(offset == -5);
    }

    SECTION("as a positional")
    {
        CHECK(parse(args, {"-5"}).ok());
        REQUIRE(values.size() == 1);
        CHECK(values[0] == "-5");
    }
}

TEST("args grammar - a declared numeric short name wins over the negative-number rule")
{
    auto five = false;
    auto args = make_args();
    args.arg({"5"}, five, "the fifth thing");

    CHECK(parse(args, {"-5"}).ok());
    CHECK(five);
}

TEST("args grammar - a lone dash is a positional")
{
    auto values = cc::vector<cc::string>();
    auto args = make_args();
    args.positional("VALUES", values, {.desc = "inputs"});

    CHECK(parse(args, {"-"}).ok());
    REQUIRE(values.size() == 1);
    CHECK(values[0] == "-");
}

TEST("args grammar - underscores and dashes are the same long name")
{
    auto value = false;

    SECTION("by default either spelling works")
    {
        auto args = make_args();
        args.arg({"dry-run"}, value, "pretend");

        CHECK(parse(args, {"--dry_run"}).ok());
        CHECK(value);
    }

    SECTION("exact_long_names turns that off")
    {
        auto args = make_args();
        args.exact_long_names();
        args.arg({"dry-run"}, value, "pretend");

        CHECK(!parse(args, {"--dry_run"}).ok());
    }
}

TEST("args grammar - long names are case sensitive")
{
    auto value = false;
    auto args = make_args();
    args.arg({"force"}, value, "do it");

    CHECK(!parse(args, {"--Force"}).ok());
    CHECK(!value);
}

TEST("args grammar - no abbreviation")
{
    auto output = cc::string();
    auto args = make_args();
    args.arg({"output"}, output, "where");

    // A prefix that would be unambiguous today becomes ambiguous the moment a sibling is added, so it is
    // never accepted — but it is suggested.
    auto const r = parse(args, {"--out", "x"});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() >= 1);
    CHECK(r.diagnostics()[0].suggestion == "--output");
}

TEST("args grammar - a one-character long name needs its dashes spelled out")
{
    auto x_short = false;
    auto x_long = false;
    auto args = make_args();
    args.arg({"x", "--x"}, x_short, "both spellings");
    args.arg({"y"}, x_long, "short only");

    CHECK(parse(args, {"--x"}).ok());
    CHECK(x_short);

    x_short = false;
    CHECK(parse(args, {"-x"}).ok());
    CHECK(x_short);
}

TEST("args grammar - positionals, fixed and variadic")
{
    SECTION("fixed only")
    {
        auto src = cc::string();
        auto dst = cc::string();
        auto args = make_args();
        args.positional("SRC", src, {.desc = "from"});
        args.positional("DST", dst, {.desc = "to"});

        CHECK(parse(args, {"a", "b"}).ok());
        CHECK(src == "a");
        CHECK(dst == "b");
    }

    SECTION("a variadic in the middle is back-filled from both ends")
    {
        auto src = cc::string();
        auto middle = cc::vector<cc::string>();
        auto dst = cc::string();
        auto args = make_args();
        args.positional("SRC", src, {.desc = "from"});
        args.positional("MIDDLE", middle, {.desc = "through"});
        args.positional("DST", dst, {.desc = "to"});

        CHECK(parse(args, {"a", "x", "y", "b"}).ok());
        CHECK(src == "a");
        REQUIRE(middle.size() == 2);
        CHECK(middle[0] == "x");
        CHECK(middle[1] == "y");
        CHECK(dst == "b");
    }

    SECTION("an empty variadic is fine when its minimum allows it")
    {
        auto src = cc::string();
        auto middle = cc::vector<cc::string>();
        auto dst = cc::string();
        auto args = make_args();
        args.positional("SRC", src, {.desc = "from"});
        args.positional("MIDDLE", middle, {.desc = "through"});
        args.positional("DST", dst, {.desc = "to"});

        CHECK(parse(args, {"a", "b"}).ok());
        CHECK(src == "a");
        CHECK(middle.empty());
        CHECK(dst == "b");
    }
}

TEST("args grammar - positionals may be interspersed with options")
{
    auto force = false;
    auto files = cc::vector<cc::string>();
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");
    args.positional("FILES", files, {.desc = "inputs"});

    CHECK(parse(args, {"one", "--force", "two"}).ok());
    CHECK(force);
    REQUIRE(files.size() == 2);
    CHECK(files[0] == "one");
    CHECK(files[1] == "two");
}

TEST("args grammar - stop_at_first_positional leaves the tail alone")
{
    auto force = false;
    auto files = cc::vector<cc::string>();
    auto args = make_args();
    args.stop_at_first_positional();
    args.arg({"f", "force"}, force, "do it");
    args.positional("FILES", files, {.desc = "inputs"});

    CHECK(parse(args, {"one", "--force", "two"}).ok());
    CHECK(!force); // --force came after the first positional, so it is just another value
    REQUIRE(files.size() == 3);
    CHECK(files[1] == "--force");
}

TEST("args grammar - the -- separator")
{
    SECTION("its tail goes to the declared rest binding")
    {
        auto force = false;
        auto tail = cc::vector<cc::string_view>();
        auto args = make_args();
        args.arg({"f", "force"}, force, "do it");
        args.rest(tail, "ARGS", "passed onward");

        CHECK(parse(args, {"--force", "--", "--not-mine", "-x"}).ok());
        CHECK(force);
        REQUIRE(tail.size() == 2);
        CHECK(tail[0] == "--not-mine");
        CHECK(tail[1] == "-x");
    }

    SECTION("without one it is an error, never a silent drop")
    {
        auto args = make_args();
        auto const r = parse(args, {"--", "whatever"});
        CHECK(!r.ok());
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unexpected_separator);
    }
}

TEST("args grammar - raw() carries the tail whether or not it was bound")
{
    auto tail = cc::vector<cc::string_view>();
    auto args = make_args();
    args.rest(tail, "ARGS");

    CHECK(parse(args, {"--", "a", "b"}).ok());
    REQUIRE(args.raw().size() == 2);
    CHECK(args.raw()[0] == "a");
}

TEST("args grammar - bound views outlive the caller's tokens")
{
    auto value = cc::string_view();
    auto args = make_args();
    args.arg({"n", "name"}, value, "who");

    {
        // The caller's own storage goes away at the end of this scope; the parse must have copied it.
        auto owned = cc::string("temporary");
        auto tokens = cc::vector<cc::string_view>();
        tokens.push_back("--name");
        tokens.push_back(owned);
        CHECK(args.parse(tokens).ok());
    }

    CHECK(value == "temporary");
}

TEST("args grammar - actions run in token order")
{
    auto log = cc::string();
    auto args = make_args();
    args.action({"a"}, [&] { log += "a"; }, "first");
    args.action({"b"}, [&] { log += "b"; }, "second");

    CHECK(parse(args, {"-b", "-a", "-b"}).ok());
    CHECK(log == "bab");
}

TEST("args grammar - a value action sees every occurrence in order")
{
    auto seen = cc::vector<cc::string>();
    auto args = make_args();
    args.value_action({"I", "include"}, [&](cc::string_view v) { seen.push_back(cc::string(v)); });

    CHECK(parse(args, {"-I", "one", "--include=two"}).ok());
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "one");
    CHECK(seen[1] == "two");
}

TEST("args grammar - allow_unknown captures instead of failing")
{
    auto force = false;
    auto unknown = cc::vector<cc::string_view>();
    auto args = make_args();
    args.arg({"f", "force"}, force, "do it");
    args.allow_unknown(unknown);

    CHECK(parse(args, {"--force", "--mystery", "-z"}).ok());
    CHECK(force);
    REQUIRE(unknown.size() == 2);
    CHECK(unknown[0] == "--mystery");
    CHECK(unknown[1] == "-z");
}

TEST("args grammar - parse can be run more than once")
{
    auto jobs = 1;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    CHECK(parse(args, {"-j", "4"}).ok());
    CHECK(jobs == 4);

    // Occurrence state resets, so the second run is not confused by the first.
    CHECK(parse(args, {"-j", "9"}).ok());
    CHECK(jobs == 9);
}

TEST("args grammar - the argv overload skips the program path")
{
    auto jobs = 1;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    char const* argv[] = {"C:/somewhere/tool.exe", "-j", "3"};
    CHECK(args.parse(3, argv).ok());
    CHECK(jobs == 3);
}

TEST("args grammar - the span overload skips nothing")
{
    auto values = cc::vector<cc::string>();
    auto args = make_args();
    args.positional("VALUES", values, {.desc = "inputs"});

    // The one asymmetry with the argv form, and the easiest thing to get backwards.
    CHECK(parse(args, {"first", "second"}).ok());
    REQUIRE(values.size() == 2);
    CHECK(values[0] == "first");
}
