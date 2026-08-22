#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>

// Help output, pinned as goldens.
//
// Width and colour are always passed explicitly, never sniffed, so a golden does not depend on how the test
// binary happened to be invoked.
// Each golden covers ONE feature: a kitchen-sink golden churns on every unrelated change, while a
// feature-sized one fails exactly where the change is.

using namespace cc::primitive_defines;

namespace
{
nx::args_builder make_args()
{
    auto args = nx::args({.name = "t", .description = "a test program"});
    args.no_auto_print();
    return args;
}

/// Trailing spaces on a padded row are invisible in a golden, so neither side carries them.
/// Nothing else is normalized: leading indentation is part of what these tests pin.
cc::string trim_line_ends(cc::string_view text)
{
    auto out = cc::string();
    auto line_start = isize(0);

    for (auto i = isize(0); i <= text.size(); ++i)
    {
        if (i < text.size() && text[i] != '\n')
            continue;

        auto end = i;
        while (end > line_start && (text[end - 1] == ' ' || text[end - 1] == '\r'))
            --end;

        out += text.subview({.start = line_start, .end = end});
        if (i < text.size())
            out += "\n";

        line_start = i + 1;
    }

    return out;
}
} // namespace

TEST("args help - the usage line")
{
    auto jobs = 4;
    auto output = cc::string();
    auto files = cc::vector<cc::string>();
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");
    args.arg({"o", "output"}, output, {.desc = "where", .required = true});
    args.positional("FILES", files, {.desc = "inputs", .min_count = 1});

    // A required option is spelled out rather than hidden inside "[options]".
    CHECK(args.usage_line() == "usage: t [options] --output STRING FILES...");
}

TEST("args help - the usage line distinguishes optional from required positionals")
{
    auto src = cc::string();
    auto files = cc::vector<cc::string>();

    SECTION("an unbounded variadic is bracketed")
    {
        auto args = make_args();
        args.positional("FILES", files, {.desc = "inputs"});
        CHECK(args.usage_line() == "usage: t [FILES...]");
    }

    SECTION("one with a minimum is not")
    {
        auto args = make_args();
        args.positional("FILES", files, {.desc = "inputs", .min_count = 1});
        CHECK(args.usage_line() == "usage: t FILES...");
    }

    SECTION("a fixed positional follows its own required flag")
    {
        auto args = make_args();
        args.positional("SRC", src, {.desc = "from", .required = true});
        CHECK(args.usage_line() == "usage: t SRC");
    }

    SECTION("a rest binding shows the separator")
    {
        auto tail = cc::vector<cc::string_view>();
        auto args = make_args();
        args.rest(tail, "ARGS");
        CHECK(args.usage_line() == "usage: t [-- ARGS]");
    }
}

TEST("args help - a minimal program")
{
    auto jobs = 4;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many jobs to run at once");

    CHECK(trim_line_ends(args.help_text({.width = 90})) == R"(t
a test program

usage: t [options]

options:
  -j, --jobs INT  how many jobs to run at once [default: 4]
  -h              show a short help and exit
  --help          show the full help and exit
)");
}

TEST("args help - defaults, requiredness and env are annotated")
{
    auto jobs = 4;
    auto output = cc::string();
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .env = "T_JOBS"});
    args.arg({"o", "output"}, output, {.desc = "where to write", .metavar = "FILE", .required = true});

    CHECK(trim_line_ends(args.help_text({.width = 90})) == R"(t
a test program

usage: t [options] --output FILE

options:
  -j, --jobs INT     how many [default: 4] [env: T_JOBS]
  -o, --output FILE  where to write [required]
  -h                 show a short help and exit
  --help             show the full help and exit
)");
}

TEST("args help - groups become sections, in declaration order")
{
    auto a = false;
    auto b = false;
    auto args = make_args();
    args.no_auto_help();
    args.arg({"a"}, a, "ungrouped");
    args.group("output");
    args.arg({"b"}, b, "grouped");

    CHECK(trim_line_ends(args.help_text({.width = 90})) == R"(t
a test program

usage: t [options]

options:
  -a  ungrouped

output:
  -b  grouped
)");
}

TEST("args help - long descriptions and extras are --help only")
{
    auto jobs = 4;
    auto args = nx::args({.name = "t", .description = "a test program"});
    args.no_auto_print();
    args.no_auto_help();
    args.arg({"j", "jobs"}, jobs, {.desc = "how many", .help = "The much longer story about jobs."});
    args.section("notes", "Something worth knowing.");
    args.example("t -j8", "eight jobs");
    args.document_env("T_HOME", "where configuration lives");

    SECTION("the short form carries none of it")
    {
        CHECK(trim_line_ends(args.short_help_text({.width = 90})) == R"(t
a test program

usage: t [options]

options:
  -j, --jobs INT  how many [default: 4]
)");
    }

    SECTION("the full form carries all of it")
    {
        CHECK(trim_line_ends(args.help_text({.width = 90})) == R"(t
a test program

usage: t [options]

options:
  -j, --jobs INT  The much longer story about jobs. [default: 4]

environment:
  T_HOME  where configuration lives

notes:
  Something worth knowing.

examples:
  eight jobs
    t -j8
)");
    }
}

TEST("args help - descriptions wrap to the given width and stay in their column")
{
    auto jobs = 4;
    auto args = make_args();
    args.no_auto_help();
    args.arg({"j", "jobs"}, jobs, "how many jobs to run at once, which is a description long enough to need wrapping");

    CHECK(trim_line_ends(args.help_text({.width = 60})) == R"(t
a test program

usage: t [options]

options:
  -j, --jobs INT  how many jobs to run at once, which is a
                  description long enough to need wrapping
                  [default: 4]
)");
}

TEST("args help - a hidden argument is absent but still parses")
{
    auto secret = false;
    auto args = make_args();
    args.no_auto_help();
    args.arg({"s", "secret"}, secret, {.desc = "internal", .hidden = true});

    auto const help = args.help_text({.width = 90});
    CHECK(!help.contains("secret"));
    CHECK(!help.contains("[options]")); // nothing visible to advertise

    CHECK(args.parse({"--secret"}).ok());
    CHECK(secret);
}

TEST("args help - colour is off unless asked for")
{
    auto jobs = 4;
    auto args = make_args();
    args.arg({"j", "jobs"}, jobs, "how many");

    CHECK(!args.help_text({.color = false}).contains("\033["));
    CHECK(args.help_text({.color = true}).contains("\033["));
}
