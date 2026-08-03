#include <nexus/test.hh>
#include <shaped-linter/cli/options.hh>

namespace
{
/// Parse a fixed argv array (argv[0] is a dummy program name, as in a real launch).
cc::result<scl::options> parse(cc::span<char const* const> argv)
{
    return scl::parse_options(argv);
}
} // namespace

TEST("shaped-linter - options - files and flags")
{
    char const* const argv[] = {"shaped-linter", "a.cc", "--fix", "b.hh"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());

    auto const& o = r.value();
    CHECK(o.apply_fixes);
    CHECK(!o.help);
    CHECK(o.files.size() == 2);
    CHECK(o.files[0] == "a.cc");
    CHECK(o.files[1] == "b.hh");
}

TEST("shaped-linter - options - help short-circuits")
{
    char const* const argv[] = {"shaped-linter", "--help"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());
    CHECK(r.value().help);
}

TEST("shaped-linter - options - unknown flag errors")
{
    char const* const argv[] = {"shaped-linter", "--nope"};
    CHECK(parse(argv).has_error());
}

TEST("shaped-linter - options - no files errors")
{
    char const* const argv[] = {"shaped-linter"};
    CHECK(parse(argv).has_error());
}

TEST("shaped-linter - options - color mode defaults to auto")
{
    char const* const argv[] = {"shaped-linter", "a.cc"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());
    CHECK(r.value().color == cc::console::color_mode::automatic);
}

TEST("shaped-linter - options - color takes its mode as a value or after an equals sign")
{
    char const* const separate[] = {"shaped-linter", "--color", "always", "a.cc"};
    REQUIRE(parse(separate).has_value());
    CHECK(parse(separate).value().color == cc::console::color_mode::always);

    char const* const joined[] = {"shaped-linter", "--color=never", "a.cc"};
    REQUIRE(parse(joined).has_value());
    CHECK(parse(joined).value().color == cc::console::color_mode::never);
}

TEST("shaped-linter - options - no-color is the old spelling of never")
{
    char const* const argv[] = {"shaped-linter", "--no-color", "a.cc"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());
    CHECK(r.value().color == cc::console::color_mode::never);
}

TEST("shaped-linter - options - a bad or missing color mode errors")
{
    char const* const unknown[] = {"shaped-linter", "--color", "rainbow", "a.cc"};
    CHECK(parse(unknown).has_error());

    char const* const missing[] = {"shaped-linter", "--color"};
    CHECK(parse(missing).has_error());
}

TEST("shaped-linter - options - double dash forces positionals")
{
    char const* const argv[] = {"shaped-linter", "--", "--weird-name.cc"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());
    CHECK(r.value().files.size() == 1);
    CHECK(r.value().files[0] == "--weird-name.cc");
}

TEST("shaped-linter - options - prose apply takes one plan and its flags")
{
    // The span starts AFTER the `prose apply` verb, which is what main hands the parser.
    SECTION("a bare plan defaults to writing, without stats")
    {
        char const* const argv[] = {"p.plan"};
        auto const r = scl::parse_prose_apply_options(argv);
        REQUIRE(r.has_value());
        CHECK(r.value().plan_path == "p.plan");
        CHECK(!r.value().dry_run);
        CHECK(!r.value().stats);
    }
    SECTION("dry-run and stats are independent")
    {
        char const* const argv[] = {"--stats", "--dry-run", "p.plan"};
        auto const r = scl::parse_prose_apply_options(argv);
        REQUIRE(r.has_value());
        CHECK(r.value().dry_run);
        CHECK(r.value().stats);
    }
    SECTION("no plan, two plans, or an unknown flag all error")
    {
        char const* const none[] = {"--stats"};
        CHECK(scl::parse_prose_apply_options(none).has_error());

        char const* const two[] = {"a.plan", "b.plan"};
        CHECK(scl::parse_prose_apply_options(two).has_error());

        char const* const unknown[] = {"--nope", "a.plan"};
        CHECK(scl::parse_prose_apply_options(unknown).has_error());
    }
}
