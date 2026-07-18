#include <clean-core/container/vector.hh>
#include <instruction-tracer/cli/options.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
cc::result<options> parse(cc::vector<char const*> argv)
{
    return parse_options(cc::span<char const* const>(argv.data(), argv.size()));
}
} // namespace

TEST("options - defaults")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo"});
    REQUIRE(r.has_value());

    auto const& o = r.value();
    CHECK(o.exe == "t.exe");
    CHECK(o.skip == 0);
    CHECK(o.traces == 1);
    CHECK(o.instructions == 100);
    CHECK(o.until_return);
    CHECK(o.stop_at_syscall);
    CHECK(o.stack);
    CHECK(o.source);
    CHECK(o.terminate_after_traces);
    CHECK(!o.register_diffs);
    CHECK(o.target_args.empty());

    // No section flag means the trace alone; memory regions default to heap + stack.
    CHECK(o.sections.trace);
    CHECK(!o.sections.stats);
    CHECK(!o.sections.any_memory());
    CHECK(o.regions.heap);
    CHECK(o.regions.stack);
    CHECK(!o.regions.frame);
    CHECK(!o.regions.instructions);
    CHECK(!o.memory_instruction_addresses);
}

TEST("options - collection flags")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo::bar", //
                    "--skip", "100", "--traces", "3", "--instructions", "42"});
    REQUIRE(r.has_value());

    CHECK(r.value().skip == 100);
    CHECK(r.value().traces == 3);
    CHECK(r.value().instructions == 42);
    CHECK(r.value().target.form == target_spec::kind::symbol);
    CHECK(r.value().target.symbol == "foo::bar");
}

TEST("options - every bool flag has a --no- form")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", //
                    "--no-until-return", "--no-stop-at-syscall", "--no-stack", "--no-source",
                    "--no-terminate-after-traces", "--register-diffs", "--no-stats"});
    REQUIRE(r.has_value());

    auto const& o = r.value();
    CHECK(!o.until_return);
    CHECK(!o.stop_at_syscall);
    CHECK(!o.stack);
    CHECK(!o.source);
    CHECK(!o.terminate_after_traces);
    CHECK(o.register_diffs);
    CHECK(!o.sections.stats);
}

TEST("options - --stats is a shortcut for --sections stats and raises the instruction cap")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--stats"});
    REQUIRE(r.has_value());
    CHECK(r.value().sections.stats);
    CHECK(!r.value().sections.trace); // a named section replaces the default trace
    CHECK(r.value().instructions == stats_instruction_default);
}

TEST("options - --sections selects an arbitrary subset, all from one capture")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", //
                    "--sections", "trace,memory,memory-stats"});
    REQUIRE(r.has_value());

    auto const& s = r.value().sections;
    CHECK(s.trace);
    CHECK(s.memory);
    CHECK(s.memory_stats);
    CHECK(!s.stats);
    CHECK(!s.cachelines);
    // A memory section raises the cap just like --stats.
    CHECK(r.value().instructions == stats_instruction_default);
}

TEST("options - --sections trace keeps the default cap")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--sections", "trace"});
    REQUIRE(r.has_value());
    CHECK(r.value().sections.trace);
    CHECK(r.value().instructions == 100);
}

TEST("options - --sections and --stats union")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--sections", "trace", "--stats"});
    REQUIRE(r.has_value());
    CHECK(r.value().sections.trace);
    CHECK(r.value().sections.stats);
}

TEST("options - an unknown section is rejected")
{
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--sections", "bogus"}).has_error());
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--sections", "trace,bogus"}).has_error());
}

TEST("options - --memory-regions replaces the default set")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", //
                    "--memory-regions", "frame,instructions", "--memory-instruction-addresses"});
    REQUIRE(r.has_value());

    auto const& o = r.value();
    CHECK(!o.regions.heap);
    CHECK(!o.regions.stack);
    CHECK(o.regions.frame);
    CHECK(o.regions.instructions);
    CHECK(o.memory_instruction_addresses);
}

TEST("options - an unknown memory region is rejected")
{
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--memory-regions", "cache"}).has_error());
}

TEST("options - an explicit --instructions beats --stats, whichever comes first")
{
    auto after = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--stats", "--instructions", "500"});
    REQUIRE(after.has_value());
    CHECK(after.value().instructions == 500);

    auto before = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--instructions", "500", "--stats"});
    REQUIRE(before.has_value());
    CHECK(before.value().instructions == 500);
}

TEST("options - --no-stats leaves the instruction default alone")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--no-stats"});
    REQUIRE(r.has_value());
    CHECK(r.value().instructions == 100);
}

TEST("options - explicit positive form overrides a default-on flag")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--no-source", "--source"});
    REQUIRE(r.has_value());
    CHECK(r.value().source);
}

TEST("options - everything after -- goes to the debuggee")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", //
                    "--", "--traces", "99", "some-test"});
    REQUIRE(r.has_value());

    // --traces after -- belongs to the debuggee, not to us.
    REQUIRE(r.value().target_args.size() == 3);
    CHECK(r.value().target_args[0] == "--traces");
    CHECK(r.value().target_args[1] == "99");
    CHECK(r.value().target_args[2] == "some-test");
    CHECK(r.value().traces == 1);
}

TEST("options - bare -- yields no debuggee args")
{
    auto r = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--"});
    REQUIRE(r.has_value());
    CHECK(r.value().target_args.empty());
}

TEST("options - target forms")
{
    auto by_address = parse({"instruction-tracer", "--exe", "t.exe", "--address", "0x7ff611203410"});
    REQUIRE(by_address.has_value());
    CHECK(by_address.value().target.form == target_spec::kind::address);
    CHECK(by_address.value().target.address == 0x7ff611203410ull);

    auto by_target = parse({"instruction-tracer", "--exe", "t.exe", "--target", "mod.exe+0x10"});
    REQUIRE(by_target.has_value());
    CHECK(by_target.value().target.form == target_spec::kind::module_offset);

    // --symbol is always a symbol, even when the name looks like an address.
    auto odd = parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "0x10"});
    REQUIRE(odd.has_value());
    CHECK(odd.value().target.form == target_spec::kind::symbol);
    CHECK(odd.value().target.symbol == "0x10");
}

TEST("options - required arguments")
{
    CHECK(parse({"instruction-tracer", "--symbol", "foo"}).has_error()); // no --exe
    CHECK(parse({"instruction-tracer", "--exe", "t.exe"}).has_error());  // no target
    CHECK(parse({"instruction-tracer"}).has_error());
}

TEST("options - target forms are mutually exclusive")
{
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--address", "0x10"}).has_error());
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--target", "bar"}).has_error());
}

TEST("options - rejects bad input")
{
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "foo", "--bogus"}).has_error());
    CHECK(parse({"instruction-tracer", "--exe"}).has_error());                      // missing value
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol"}).has_error()); // missing value
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "f", "--skip", "x"}).has_error());
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "f", "--traces", "0"}).has_error());
    CHECK(parse({"instruction-tracer", "--exe", "t.exe", "--symbol", "f", "--instructions", "0"}).has_error());
}

TEST("options - --help short-circuits the required arguments")
{
    auto r = parse({"instruction-tracer", "--help"});
    REQUIRE(r.has_value());
    CHECK(r.value().help);

    CHECK(parse({"instruction-tracer", "-h"}).value().help);
    CHECK(!usage_text().empty());
}
