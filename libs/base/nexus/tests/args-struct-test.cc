#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// The by-value path, and that it really is the by-reference path underneath.

using namespace cc::primitive_defines;

namespace
{
/// The tier for a type you own: a static member, discoverable right where the fields are.
struct build_options
{
    int jobs = 4;
    bool verbose = false;
    cc::string output = "a.out";
    cc::vector<cc::string> files;

    static void declare_args(nx::args_builder& args, build_options& self)
    {
        args.info({.name = "build", .description = "build the project", .version = "2.0"});
        args.no_auto_print();
        args.arg({"j", "jobs"}, self.jobs, {.desc = "how many", .validate = nx::arg::in_range(1, 64)});
        args.arg({"v", "verbose"}, self.verbose, "print more");
        args.arg({"o", "output"}, self.output, "where to write");
        args.positional("FILES", self.files, {.desc = "what to build"});
    }
};

/// A type we do not own and cannot add a member to.
struct third_party_options
{
    int level = 0;
};

/// The same type, with a member that must LOSE to the trait below.
struct both_tiers
{
    cc::string chosen = "neither";

    static void declare_args(nx::args_builder& args, both_tiers& self)
    {
        args.no_auto_print();
        self.chosen = "member";
        args.arg({"x"}, self.chosen, "unused");
    }
};
} // namespace

template <>
struct nx::custom::args_trait<third_party_options>
{
    static void declare(nx::args_builder& args, third_party_options& self)
    {
        args.no_auto_print();
        args.arg({"l", "level"}, self.level, "how deep");
    }
};

template <>
struct nx::custom::args_trait<both_tiers>
{
    static void declare(nx::args_builder& args, both_tiers& self)
    {
        args.no_auto_print();
        self.chosen = "trait";
        args.arg({"x"}, self.chosen, "unused");
    }
};

TEST("args struct - the member tier fills in a struct")
{
    auto const parsed = nx::parse_args<build_options>({"-j", "8", "--verbose", "one.cc", "two.cc"});

    REQUIRE(parsed.ok());
    CHECK(!parsed.should_exit());

    auto const& o = parsed.value();
    CHECK(o.jobs == 8);
    CHECK(o.verbose);
    CHECK(o.output == "a.out"); // the member initializer, untouched
    REQUIRE(o.files.size() == 2);
    CHECK(o.files[0] == "one.cc");
}

TEST("args struct - member initializers are the defaults")
{
    auto const parsed = nx::parse_args<build_options>({});

    REQUIRE(parsed.ok());
    CHECK(parsed.value().jobs == 4);
    CHECK(!parsed.value().verbose);
    CHECK(parsed.value().output == "a.out");
}

TEST("args struct - the trait tier adapts a type that has no member")
{
    auto const parsed = nx::parse_args<third_party_options>({"--level", "3"});

    REQUIRE(parsed.ok());
    CHECK(parsed.value().level == 3);
}

TEST("args struct - the trait tier is checked first and beats the member")
{
    // The same precedence cc::hash uses: an override you can apply to a type you do not control.
    auto const parsed = nx::parse_args<both_tiers>({});

    REQUIRE(parsed.ok());
    CHECK(parsed.value().chosen == "trait");
}

TEST("args struct - all three outcomes come through")
{
    SECTION("success")
    {
        auto const parsed = nx::parse_args<build_options>({"-j", "2"});
        CHECK(parsed.ok());
        CHECK(!parsed.should_exit());
    }

    SECTION("help exits zero")
    {
        auto const parsed = nx::parse_args<build_options>({"--help"});
        CHECK(!parsed.ok());
        CHECK(parsed.should_exit());
        CHECK(parsed.exit_code() == 0);
    }

    SECTION("a usage error exits non-zero and carries its diagnostics")
    {
        auto const parsed = nx::parse_args<build_options>({"--nonsense"});
        CHECK(!parsed.ok());
        CHECK(parsed.exit_code() == 1);
        REQUIRE(parsed.result().has_diagnostics());
        CHECK(parsed.result().diagnostics()[0].kind == nx::diagnostic_kind::unknown_option);
    }
}

TEST("args struct - it is the same engine, so everything else still applies")
{
    SECTION("validation")
    {
        auto const parsed = nx::parse_args<build_options>({"-j", "999"});
        CHECK(!parsed.ok());
        REQUIRE(parsed.result().has_diagnostics());
        CHECK(parsed.result().diagnostics()[0].kind == nx::diagnostic_kind::failed_validation);
    }

    SECTION("did-you-mean")
    {
        auto const parsed = nx::parse_args<build_options>({"--verbse"});
        REQUIRE(parsed.result().has_diagnostics());
        CHECK(parsed.result().diagnostics()[0].suggestion == "--verbose");
    }

    SECTION("info() reaches the version flag")
    {
        auto const parsed = nx::parse_args<build_options>({"--version"});
        CHECK(parsed.exit_code() == 0);
        CHECK(parsed.result().outcome() == nx::args_outcome::version_requested);
    }
}

TEST("args struct - the argv overload skips the program path")
{
    char const* argv[] = {"tool.exe", "-j", "3"};
    auto const parsed = nx::parse_args<build_options>(3, argv);

    REQUIRE(parsed.ok());
    CHECK(parsed.value().jobs == 3);
}

TEST("args struct - declare_args works on a builder the caller owns")
{
    // The by-reference path and the by-value path are the same declaration, which is the point.
    auto options = build_options();
    auto args = nx::args({});
    nx::declare_args(args, options);

    CHECK(args.parse({"-j", "6"}).ok());
    CHECK(options.jobs == 6);
}
