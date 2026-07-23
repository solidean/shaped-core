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

TEST("shaped-linter - options - double dash forces positionals")
{
    char const* const argv[] = {"shaped-linter", "--", "--weird-name.cc"};
    auto const r = parse(argv);
    REQUIRE(r.has_value());
    CHECK(r.value().files.size() == 1);
    CHECK(r.value().files[0] == "--weird-name.cc");
}
