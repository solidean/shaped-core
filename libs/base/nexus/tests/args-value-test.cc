#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// What each bindable type accepts, and what it says when it refuses.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t"});
    args.no_auto_print();
    return args;
}

/// A tiny enum with a name table, which is the shape that drives "one of: ..." in help.
enum class mode
{
    fast,
    slow,
};
} // namespace

template <>
struct nx::custom::arg_value_trait<mode>
{
    static bool parse(cc::string_view token, mode& out, cc::string& error)
    {
        if (token == "fast")
            return out = mode::fast, true;
        if (token == "slow")
            return out = mode::slow, true;

        error = "expected fast or slow";
        return false;
    }

    static cc::string_view type_name() { return "MODE"; }

    static void values(cc::vector<cc::string>& out)
    {
        out.push_back("fast");
        out.push_back("slow");
    }
};

TEST("args value - integers respect their own width")
{
    auto small = i8(0);
    auto args = make_args();
    args.arg({"n"}, small, "a small number");

    CHECK(args.parse({"-n", "127"}).ok());
    CHECK(small == 127);

    auto const r = args.parse({"-n", "128"});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::invalid_value);

    // The range named is the DECLARED type's, not i64's — that is the whole reason each width parses itself.
    CHECK(r.diagnostics()[0].message.contains("-128"));
    CHECK(r.diagnostics()[0].message.contains("127"));
}

TEST("args value - an unsigned type refuses a sign rather than wrapping")
{
    auto count = u32(0);
    auto args = make_args();
    args.arg({"n"}, count, "how many");

    auto const r = args.parse({"-n", "-1"});
    CHECK(!r.ok());
    CHECK(count == 0);
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].message.contains("non-negative"));
}

TEST("args value - a malformed number and an out-of-range one say different things")
{
    auto n = 0;
    auto args = make_args();
    args.arg({"n"}, n, "a number");

    auto const bad = args.parse({"-n", "hello"});
    REQUIRE(bad.diagnostics().size() == 1);
    CHECK(bad.diagnostics()[0].message.contains("expected an integer"));

    auto const big = args.parse({"-n", "99999999999999999999"});
    REQUIRE(big.diagnostics().size() == 1);
    CHECK(big.diagnostics()[0].message.contains("out of range"));
}

TEST("args value - floats")
{
    auto scale = 1.0f;
    auto args = make_args();
    args.arg({"s", "scale"}, scale, "how much");

    CHECK(args.parse({"--scale", "2.5"}).ok());
    CHECK(scale == 2.5f);

    CHECK(args.parse({"--scale", "-1.25e2"}).ok());
    CHECK(scale == -125.f);

    CHECK(!args.parse({"--scale", "big"}).ok());
}

TEST("args value - bool takes the CLI spellings, case-insensitively")
{
    auto flag = false;
    auto args = make_args();
    args.arg({"f", "flag"}, flag, "a flag");

    // Only the long `=` form carries a value: a short bool never does, which is what keeps clusters simple.
    for (auto const* const yes : {"--flag=true", "--flag=TRUE", "--flag=yes", "--flag=On", "--flag=1"})
    {
        flag = false;
        CHECK(args.parse({cc::string_view(yes)}).ok());
        CHECK(flag);
    }

    for (auto const* const no : {"--flag=false", "--flag=no", "--flag=off", "--flag=0"})
    {
        flag = true;
        CHECK(args.parse({cc::string_view(no)}).ok());
        CHECK(!flag);
    }

    SECTION("and refuses anything else, rather than guessing")
    {
        auto const r = args.parse({"--flag=perhaps"});
        CHECK(!r.ok());
        REQUIRE(r.diagnostics().size() == 1);
        CHECK(r.diagnostics()[0].kind == nx::diagnostic_kind::invalid_value);
    }
}

TEST("args value - strings take anything, including empty")
{
    auto name = cc::string("default");
    auto args = make_args();
    args.arg({"n", "name"}, name, "who");

    CHECK(args.parse({"--name="}).ok());
    CHECK(name == "");

    CHECK(args.parse({"--name", "--weird-but-fine"}).ok());
    CHECK(name == "--weird-but-fine");
}

TEST("args value - a custom trait plugs in through nx::custom")
{
    auto m = mode::slow;
    auto args = make_args();
    args.arg({"m", "mode"}, m, "how to run");

    CHECK(args.parse({"--mode", "fast"}).ok());
    CHECK(m == mode::fast);

    auto const r = args.parse({"--mode", "sideways"});
    CHECK(!r.ok());
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].message.contains("expected fast or slow"));
}

TEST("args value - a type that publishes its values lists them in help")
{
    auto m = mode::fast;
    auto args = make_args();
    args.arg({"m", "mode"}, m, "how to run");

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("one of: fast, slow"));
    CHECK(help.contains("MODE"));
}

TEST("args value - the metavar defaults to the type and can be overridden")
{
    auto path = cc::string();
    auto n = 0;
    auto args = make_args();
    args.arg({"o", "output"}, path, {.desc = "where to write", .metavar = "FILE"});
    args.arg({"j", "jobs"}, n, "how many");

    auto const help = args.help_text({.width = 100});
    CHECK(help.contains("--output FILE"));
    CHECK(help.contains("--jobs INT"));
}
