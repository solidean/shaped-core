#include "catch2.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/tests/export/xml.hh>

using nx::impl::xml_escape;

namespace
{
void print_section_expressions(cc::string& out,
                               nx::test_execution::section const& sec,
                               cc::string const& indent,
                               int& error_count,
                               int max_errors)
{
    // Print errors/expressions for this section
    for (auto const& error : sec.errors)
    {
        if (error_count >= max_errors)
            return;

        out += indent;
        out += "<Expression success=\"false\" ";
        out += "filename=\"";
        out += xml_escape(error.location.file_name());
        out += "\" ";
        out += "line=\"";
        out += cc::to_string(error.location.line());
        out += "\">\n";
        out += indent;
        out += "  <Original>";
        out += xml_escape(error.expr);
        out += "</Original>\n";
        out += indent;
        out += "  <Expanded>";
        out += xml_escape(error.expanded);
        out += "</Expanded>\n";
        out += indent;
        out += "</Expression>\n";

        ++error_count;
    }
}

void print_section_recursive(cc::string& out,
                             nx::test_execution::section const& sec,
                             cc::string const& indent,
                             int& error_count,
                             int max_errors)
{
    // Print expressions for this section (top-level section errors appear before subsections)
    print_section_expressions(out, sec, indent, error_count, max_errors);

    // Print subsections
    for (auto const& subsec : sec.subsections)
    {
        out += indent;
        out += "<Section name=\"";
        out += xml_escape(subsec.name);
        out += "\" ";
        out += "filename=\"";
        out += xml_escape(subsec.location.file_name());
        out += "\" ";
        out += "line=\"";
        out += cc::to_string(subsec.location.line());
        out += "\">\n";

        // Recursively print subsection content
        print_section_recursive(out, subsec, indent + "  ", error_count, max_errors);

        // Print section summary
        // If the section is considered failing but has 0 failed checks (e.g., missing CHECK),
        // report at least 1 failure so C++ TestMate interprets it correctly
        auto const failures = subsec.is_considered_failing ? cc::max(subsec.failed_checks, 1) : subsec.failed_checks;
        out += indent;
        out += "  <OverallResults ";
        out += "successes=\"";
        out += cc::to_string(subsec.executed_checks - subsec.failed_checks);
        out += "\" ";
        out += "failures=\"";
        out += cc::to_string(failures);
        out += "\" ";
        out += "expectedFailures=\"0\" ";
        out += "durationInSeconds=\"";
        out += cc::to_string(subsec.duration_seconds);
        out += "\"/>\n";

        out += indent;
        out += "</Section>\n";
    }
}
} // namespace

cc::string nx::write_catch2_discovery_xml(nx::test_registry const& registry)
{
    cc::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<MatchingTests>\n";

    for (auto const& decl : registry.declarations)
    {
        out += "  <TestCase>\n";
        out += "    <Name>";
        out += xml_escape(decl.name);
        out += "</Name>\n";
        out += "    <ClassName/>\n";
        out += "    <Tags></Tags>\n";
        out += "    <SourceInfo>\n";
        out += "      <File>";
        out += xml_escape(decl.location.file_name());
        out += "</File>\n";
        out += "      <Line>";
        out += cc::to_string(decl.location.line());
        out += "</Line>\n";
        out += "    </SourceInfo>\n";
        out += "  </TestCase>\n";
    }

    out += "</MatchingTests>\n";
    return out;
}

cc::string nx::write_catch2_results_xml(nx::test_schedule_execution const& execution)
{
    // TODO(catch2-xml):
    // - Emit captured StdOut / StdErr elements (useful for failure diagnostics and hung tests).
    // - Support INFO/CAPTURE-style contextual messages in XML, not just failed expressions.
    // - Model partial test-case runs (SECTION re-entry / partNumber) instead of only a merged section tree.
    // - Add benchmark result reporting hooks (even if unimplemented for now).
    // - Include run metadata (run name, RNG seed) for reproducibility/debugging.
    // - Track and emit expectedFailures properly instead of hardcoding 0.
    // - Fill discovery <Tags> from declarations (tag filtering is a core Catch2 feature).
    // - Consider emitting explicit "test/section started" progress lines (stderr) for live IDE feedback.

    cc::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<TestRun>\n";

    for (auto const& exec : execution.executions)
    {
        CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
        auto const& decl = *exec.instance.declaration;
        bool const success = !exec.is_considered_failing();

        out += "  <TestCase name=\"";
        out += xml_escape(decl.name);
        out += "\" ";
        out += "filename=\"";
        out += xml_escape(decl.location.file_name());
        out += "\" ";
        out += "line=\"";
        out += cc::to_string(decl.location.line());
        out += "\">\n";

        // Print all sections and expressions recursively (capped at max_errors)
        int const max_errors = 50;
        int error_count = 0;
        print_section_recursive(out, exec.root, "    ", error_count, max_errors);

        // Print test case summary
        out += "    <OverallResult success=\"";
        out += (success ? "true" : "false");
        out += "\" ";
        out += "durationInSeconds=\"";
        out += cc::to_string(exec.root.duration_seconds);
        out += "\"/>\n";
        out += "  </TestCase>\n";
    }

    out += "</TestRun>\n";
    return out;
}
