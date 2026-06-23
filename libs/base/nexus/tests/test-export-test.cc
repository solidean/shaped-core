#include <nexus/test.hh>

#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <nexus/tests/export/junit.hh>
#include <nexus/tests/export/xml.hh>

#include <sstream>
#include <string>
#include <string_view>

namespace
{
bool contains(std::string const& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
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

    std::ostringstream out;
    nx::write_junit_xml(out, "my-suite", exec);
    auto const xml = out.str();

    // Suite-level aggregates: 2 tests, 1 failure, 2 checks, named after the suite.
    CHECK(contains(xml, "<testsuites "));
    CHECK(contains(xml, "name=\"my-suite\""));
    CHECK(contains(xml, "tests=\"2\""));
    CHECK(contains(xml, "failures=\"1\""));
    CHECK(contains(xml, "assertions=\"2\""));

    // Both tests appear as cases; the passing one carries no <failure>.
    CHECK(contains(xml, "name=\"T_pass\""));
    CHECK(contains(xml, "name=\"T_fail\""));
    CHECK(contains(xml, "<failure message="));
}

TEST("export - junit report for an all-pass run has no failure elements")
{
    nx::test_registry reg;
    reg.add_declaration("A", {}, [] { CHECK(true); });
    reg.add_declaration("B", {}, [] { CHECK(true); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    std::ostringstream out;
    nx::write_junit_xml(out, "all-pass", exec);
    auto const xml = out.str();

    CHECK(contains(xml, "tests=\"2\""));
    CHECK(contains(xml, "failures=\"0\""));
    CHECK(!contains(xml, "<failure"));
}
