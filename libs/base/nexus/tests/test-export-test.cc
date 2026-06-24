#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/xml.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

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
