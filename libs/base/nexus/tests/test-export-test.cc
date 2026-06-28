#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/listing_json.hh>
#include <nexus/tests/export/xml.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

namespace
{
// A registry with one test per interesting axis: a plain normal test, a disabled test, and a manual-bucket
// test. Used to exercise the eligibility predicates and the JSON listing.
nx::test_registry make_axis_registry()
{
    nx::test_registry reg;
    reg.add_declaration("alpha", {}, [] {});
    reg.add_declaration("beta", {.enabled = false}, [] {});
    reg.add_declaration("gamma", {.bucket = nx::config::test_bucket::manual}, [] {});
    return reg;
}

nx::test_schedule_config config_from(cc::span<char const* const> args)
{
    return nx::test_schedule_config::create_from_args(int(args.size()), const_cast<char**>(args.data()));
}
} // namespace

TEST("export - xml_escape replaces the five predefined entities")
{
    CHECK(nx::impl::xml_escape("a<b>c") == "a&lt;b&gt;c");
    CHECK(nx::impl::xml_escape("a&b") == "a&amp;b");
    CHECK(nx::impl::xml_escape("\"q\"") == "&quot;q&quot;");
    CHECK(nx::impl::xml_escape("it's") == "it&apos;s");
    CHECK(nx::impl::xml_escape("plain text 123") == "plain text 123");
}

TEST("export - junit report has correct aggregate attributes and per-test cases")
{
    nx::test_registry reg;
    reg.add_declaration("T_pass", {}, [] { CHECK(1 + 1 == 2); });
    reg.add_declaration("T_fail", {}, [] { CHECK(1 == 2); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    cc::string const xml = nx::write_junit_xml("my-suite", exec);

    // Suite-level aggregates: 2 tests, 1 failure, 2 checks, named after the suite.
    CHECK(xml.contains("<testsuites "));
    CHECK(xml.contains("name=\"my-suite\""));
    CHECK(xml.contains("tests=\"2\""));
    CHECK(xml.contains("failures=\"1\""));
    CHECK(xml.contains("assertions=\"2\""));

    // Both tests appear as cases; the passing one carries no <failure>.
    CHECK(xml.contains("name=\"T_pass\""));
    CHECK(xml.contains("name=\"T_fail\""));
    CHECK(xml.contains("<failure message="));
}

TEST("schedule - would_run honors buckets and disabled, name_matches ignores them")
{
    auto reg = make_axis_registry();
    auto const& alpha = reg.declarations[0];
    auto const& beta = reg.declarations[1];  // disabled
    auto const& gamma = reg.declarations[2]; // manual bucket

    // No filters: only the plain normal test runs; disabled and manual stay out of the sweep.
    {
        char const* const args[] = {"prog"};
        auto const cfg = config_from(args);
        CHECK(cfg.name_matches(alpha));
        CHECK(cfg.name_matches(beta));
        CHECK(cfg.name_matches(gamma));
        CHECK(cfg.would_run(alpha));
        CHECK(!cfg.would_run(beta));
        CHECK(!cfg.would_run(gamma));
    }

    // A non-wildcard filter names a test directly: it pulls in an otherwise-excluded disabled test, and only
    // the name-matching test is selected.
    {
        char const* const args[] = {"prog", "beta"};
        auto const cfg = config_from(args);
        CHECK(!cfg.name_matches(alpha));
        CHECK(cfg.name_matches(beta));
        CHECK(cfg.would_run(beta)); // run_disabled_tests set by the non-wildcard filter
        CHECK(!cfg.would_run(alpha));
    }

    // Explicit bucket mode: a name in another bucket matches by name but is excluded by the bucket gate — the
    // "matched but wrong bucket" case the dev.py diagnostics explain.
    {
        char const* const args[] = {"prog", "--guide-benchmarks", "alpha"};
        auto const cfg = config_from(args);
        CHECK(cfg.name_matches(alpha));
        CHECK(!cfg.would_run(alpha));
    }
}

TEST("export - test listing JSON reports eligibility and metadata for every test")
{
    auto reg = make_axis_registry();

    char const* const args[] = {"prog"};
    auto const cfg = config_from(args);
    cc::string const json = nx::write_test_listing_json("my-suite", cfg, reg);

    // Top-level: suite name, the bucket the sweep selected, and the eligible count (only the normal test).
    CHECK(json.contains("\"suite\": \"my-suite\""));
    CHECK(json.contains("\"selected_bucket\": \"normal\""));
    CHECK(json.contains("\"eligible_count\": 1"));

    // Every registered test is listed regardless of eligibility, with its bucket and enabled flag.
    CHECK(json.contains("\"name\": \"alpha\""));
    CHECK(json.contains("\"name\": \"beta\""));
    CHECK(json.contains("\"name\": \"gamma\""));
    CHECK(json.contains("\"bucket\": \"manual\""));
    CHECK(json.contains("\"enabled\": false"));
}

TEST("export - junit report for an all-pass run has no failure elements")
{
    nx::test_registry reg;
    reg.add_declaration("A", {}, [] { CHECK(true); });
    reg.add_declaration("B", {}, [] { CHECK(true); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    cc::string const xml = nx::write_junit_xml("all-pass", exec);

    CHECK(xml.contains("tests=\"2\""));
    CHECK(xml.contains("failures=\"0\""));
    CHECK(!xml.contains("<failure"));
}
