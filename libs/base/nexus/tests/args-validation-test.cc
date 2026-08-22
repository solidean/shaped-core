#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Value rules and cross-argument rules.
// The point of a validator being an object is that the thing which rejects a value is also the thing that
// prints the rule, so both halves are checked here together.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    return args;
}
} // namespace

TEST("args validation - in_range accepts the ends and rejects past them")
{
    auto jobs = 4;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .validate = nx::arg::in_range(1, 256)});

    CHECK(args.parse({"-j", "1"}).ok());
    CHECK(jobs == 1);
    CHECK(args.parse({"-j", "256"}).ok());
    CHECK(jobs == 256);

    auto const r = args.parse({"-j", "0"});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::failed_validation);
    CHECK(r.diagnostics()[0].arg_name == "--jobs");
    CHECK(r.diagnostics()[0].message.contains("must be in [1, 256]"));
}

TEST("args validation - the rule describes itself in help, with no prose from the caller")
{
    auto jobs = 4;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .validate = nx::arg::in_range(1, 256)});

    CHECK(args.help_text({.width = 100}).contains("(must be in [1, 256])"));
}

TEST("args validation - at_least, at_most and non_empty")
{
    SECTION("at_least")
    {
        auto n = 0;
        auto args = make_args();
        args.arg({"n"}, n, {.desc = "count", .validate = nx::arg::at_least(1)});

        CHECK(args.parse({"-n", "1"}).ok());
        CHECK(!args.parse({"-n", "0"}).ok());
    }

    SECTION("at_most")
    {
        auto n = 0;
        auto args = make_args();
        args.arg({"n"}, n, {.desc = "count", .validate = nx::arg::at_most(10)});

        CHECK(args.parse({"-n", "10"}).ok());
        CHECK(!args.parse({"-n", "11"}).ok());
    }

    SECTION("non_empty")
    {
        auto name = cc::string();
        auto args = make_args();
        args.arg({"n", "name"}, name, {.desc = "who", .validate = nx::arg::non_empty()});

        CHECK(args.parse({"--name", "x"}).ok());
        CHECK(!args.parse({"--name="}).ok());
    }
}

TEST("args validation - one_of, listed in the message and in help")
{
    auto mode = cc::string("fast");
    auto args = make_args();
    args.arg({"m", "mode"}, mode, {.desc = "how", .validate = nx::arg::one_of({"fast", "slow"})});

    CHECK(args.parse({"--mode", "slow"}).ok());
    CHECK(mode == "slow");

    auto const r = args.parse({"--mode", "sideways"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].message.contains("must be one of: fast, slow"));
    CHECK(args.help_text({.width = 100}).contains("must be one of: fast, slow"));
}

TEST("args validation - rules compose with &&, and read as one rule")
{
    auto n = 0;
    auto args = make_args();
    args.arg({"n"}, n, {.desc = "count", .validate = nx::arg::at_least(1) && nx::arg::at_most(9)});

    CHECK(args.parse({"-n", "5"}).ok());
    CHECK(!args.parse({"-n", "0"}).ok());
    CHECK(!args.parse({"-n", "10"}).ok());

    CHECK(args.help_text({.width = 120}).contains("must be at least 1 and must be at most 9"));
}

TEST("args validation - satisfies takes any predicate, with the caller's own wording")
{
    auto name = cc::string();
    auto args = make_args();
    args.arg({"n", "name"}, name,
             {.desc = "who",
              .validate = nx::arg::satisfies("must start with a letter", [](cc::string const& v)
                                             { return !v.empty() && cc::is_alphanumeric(v[0]); })});

    CHECK(args.parse({"--name", "abc"}).ok());

    auto const r = args.parse({"--name", "-x"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].message.contains("must start with a letter"));
}

TEST("args validation - a rule fires per occurrence and names the token that broke it")
{
    auto ports = cc::vector<int>();
    auto args = make_args();
    args.arg({"p", "port"}, ports, {.desc = "a port", .validate = nx::arg::in_range(1, 65535)});

    auto const r = args.parse({"-p", "80", "-p", "70000", "-p", "443"});
    CHECK(!r.ok());

    // The one that failed, quoted — not the variable's final value long after the fact.
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].token == "70000");
}

TEST("args validation - a value rule runs only after the conversion succeeded")
{
    auto n = 0;
    auto args = make_args();
    args.arg({"n"}, n, {.desc = "count", .validate = nx::arg::in_range(1, 9)});

    // One complaint, about the thing that is actually wrong.
    auto const r = args.parse({"-n", "abc"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::invalid_value);
}

// --- cross-argument rules ----------------------------------------------------------------------------

TEST("args validation - at_least_one_of")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.arg({"a"}, a, "first");
    args.arg({"b"}, b, "second");
    args.at_least_one_of({"-a", "-b"});

    CHECK(args.parse({"-a"}).ok());
    CHECK(args.parse({"-b"}).ok());

    auto const r = args.parse({});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::failed_validation);
    CHECK(r.diagnostics()[0].message.contains("at least one of"));
}

TEST("args validation - mutually_exclusive")
{
    auto quiet = false;
    auto verbose = false;
    auto args = make_args();
    args.arg({"q", "quiet"}, quiet, "say less");
    args.arg({"v", "verbose"}, verbose, "say more");
    args.mutually_exclusive({"--quiet", "--verbose"});

    CHECK(args.parse({"-q"}).ok());
    CHECK(args.parse({}).ok());
    CHECK(!args.parse({"-q", "-v"}).ok());
}

TEST("args validation - requires_all")
{
    auto sign = false;
    auto key = cc::string();
    auto args = make_args();
    args.arg({"s", "sign"}, sign, "sign the output");
    args.arg({"k", "key"}, key, "the signing key");
    args.requires_all("--sign", {"--key"});

    CHECK(args.parse({"--sign", "--key", "k.pem"}).ok());
    CHECK(args.parse({"--key", "k.pem"}).ok()); // the trigger is absent, so the rule does not apply
    CHECK(!args.parse({"--sign"}).ok());
}

TEST("args validation - require takes an arbitrary predicate over the bound variables")
{
    auto width = 0;
    auto height = 0;
    auto args = make_args();
    args.arg({"w", "width"}, width, "across");
    args.arg({"h", "height"}, height, "down");
    args.require("width and height must describe a landscape", [&] { return width >= height; });

    CHECK(args.parse({"-w", "10", "-h", "5"}).ok());
    CHECK(!args.parse({"-w", "5", "-h", "10"}).ok());
}

TEST("args validation - cross-argument rules are listed in help")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.arg({"a"}, a, "first");
    args.arg({"b"}, b, "second");
    args.mutually_exclusive({"-a", "-b"});

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("constraints:"));
    CHECK(help.contains("at most one of -a, -b may be given"));
}

TEST("args validation - a cross-argument rule is skipped when something already failed")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.arg({"a"}, a, "first");
    args.arg({"b"}, b, "second");
    args.at_least_one_of({"-a", "-b"});

    // Neither flag was given, so the rule WOULD fail — but reporting it on top of the real error would be
    // reporting something that is not the problem.
    auto const r = args.parse({"--mystery"});
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::unknown_option);
}

// --- the environment as a value source -----------------------------------------------------------------
//
// Writing an environment variable is process-wide, so everything below shares one exclusion tag.

using cc::scoped_environment_variable;

TEST("args validation - a value from the environment is held to the same rule", exclusive("nx-args-env"))
{
    auto jobs = 4;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs,
             {.desc = "how many", .env = "NX_ARGS_TEST_JOBS", .validate = nx::arg::in_range(1, 256)});

    SECTION("a value the rule accepts")
    {
        scoped_environment_variable const set("NX_ARGS_TEST_JOBS", "8");
        CHECK(args.parse({}).ok());
        CHECK(jobs == 8);
    }

    SECTION("a value the rule rejects")
    {
        // Skipping the rule here would let the variable through at 0 while --jobs 0 is rejected, leaving it
        // holding something the declaration says is impossible.
        scoped_environment_variable const set("NX_ARGS_TEST_JOBS", "0");

        auto const r = args.parse({});
        CHECK(!r.ok());
        REQUIRE(r.has_diagnostics());
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::failed_validation);
        CHECK(r.diagnostics()[0].source.origin == nx::arg_origin::environment);
        CHECK(r.diagnostics()[0].source.name == "NX_ARGS_TEST_JOBS");
        CHECK(r.diagnostics()[0].message.contains("must be in [1, 256]"));
    }

    SECTION("a value the type itself refuses still reports as an invalid value")
    {
        scoped_environment_variable const set("NX_ARGS_TEST_JOBS", "lots");

        auto const r = args.parse({});
        CHECK(!r.ok());
        REQUIRE(r.has_diagnostics());
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::invalid_value);
    }

    SECTION("the command line still wins over both")
    {
        scoped_environment_variable const set("NX_ARGS_TEST_JOBS", "0");
        CHECK(args.parse({"-j", "12"}).ok());
        CHECK(jobs == 12);
    }
}

TEST("args validation - a cross-argument rule reads the builder it is handed")
{
    auto a = false;
    auto b = false;

    auto const build = [&]
    {
        auto args = make_args();
        args.arg({"a"}, a, "first");
        args.arg({"b"}, b, "second");
        args.mutually_exclusive({"-a", "-b"});
        return args;
    };

    // The rule used to capture `this`, which a move left pointing at a dead builder — and a factory that
    // returns a declaration is the pattern this library asks for everywhere.
    auto moved = build();

    CHECK(moved.parse({"-a"}).ok());
    CHECK(!moved.parse({"-a", "-b"}).ok());
}
