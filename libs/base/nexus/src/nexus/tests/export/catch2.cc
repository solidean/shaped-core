#include "catch2.hh"

#include <nexus/tests/export/xml.hh>

#include <clean-core/assert.hh>

#include <algorithm>
#include <ostream>
#include <string>

using nx::impl::xml_escape;

namespace
{
void print_section_expressions(std::ostream& out,
                               nx::test_execution::section const& sec,
                               std::string const& indent,
                               int& error_count,
                               int max_errors)
{
    // Print errors/expressions for this section
    for (auto const& error : sec.errors)
    {
        if (error_count >= max_errors)
            return;

        out << indent << "<Expression success=\"false\" ";
        out << "filename=\"" << xml_escape(error.location.file_name()) << "\" ";
        out << "line=\"" << error.location.line() << "\">\n";
        out << indent << "  <Original>" << xml_escape(error.expr) << "</Original>\n";
        out << indent << "  <Expanded>" << xml_escape(error.expanded) << "</Expanded>\n";
        out << indent << "</Expression>\n";

        ++error_count;
    }
}

void print_section_recursive(std::ostream& out,
                             nx::test_execution::section const& sec,
                             std::string const& indent,
                             int& error_count,
                             int max_errors)
{
    // Print expressions for this section (top-level section errors appear before subsections)
    print_section_expressions(out, sec, indent, error_count, max_errors);

    // Print subsections
    for (auto const& subsec : sec.subsections)
    {
        out << indent << "<Section name=\"" << xml_escape(subsec.name) << "\" ";
        out << "filename=\"" << xml_escape(subsec.location.file_name()) << "\" ";
        out << "line=\"" << subsec.location.line() << "\">\n";

        // Recursively print subsection content
        print_section_recursive(out, subsec, indent + "  ", error_count, max_errors);

        // Print section summary
        // If the section is considered failing but has 0 failed checks (e.g., missing CHECK),
        // report at least 1 failure so C++ TestMate interprets it correctly
        auto const failures = subsec.is_considered_failing ? std::max(subsec.failed_checks, 1) : subsec.failed_checks;
        out << indent << "  <OverallResults ";
        out << "successes=\"" << (subsec.executed_checks - subsec.failed_checks) << "\" ";
        out << "failures=\"" << failures << "\" ";
        out << "expectedFailures=\"0\" ";
        out << "durationInSeconds=\"" << subsec.duration_seconds << "\"/>\n";

        out << indent << "</Section>\n";
    }
}
} // namespace

void nx::write_catch2_discovery_xml(std::ostream& out, nx::test_registry const& registry)
{
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<MatchingTests>\n";

    for (auto const& decl : registry.declarations)
    {
        out << "  <TestCase>\n";
        out << "    <Name>" << xml_escape(decl.name) << "</Name>\n";
        out << "    <ClassName/>\n";
        out << "    <Tags></Tags>\n";
        out << "    <SourceInfo>\n";
        out << "      <File>" << xml_escape(decl.location.file_name()) << "</File>\n";
        out << "      <Line>" << decl.location.line() << "</Line>\n";
        out << "    </SourceInfo>\n";
        out << "  </TestCase>\n";
    }

    out << "</MatchingTests>\n";
}

void nx::write_catch2_results_xml(std::ostream& out, nx::test_schedule_execution const& execution)
{
    // TODO(catch2-xml):
    // - Emit captured StdOut / StdErr elements (useful for failure diagnostics and hung tests).
    // - Support INFO/CAPTURE-style contextual messages in XML, not just failed expressions.
    // - Model partial test-case runs (SECTION re-entry / partNumber) instead of only a merged section tree.
    // - Add benchmark result reporting hooks (even if unimplemented for now).
    // - Include run metadata (run name, RNG seed) for reproducibility/debugging.
    // - Track and emit expectedFailures properly instead of hardcoding 0.
    // - Fill discovery <Tags> from declarations (tag filtering is a core Catch2 feature).
    // - Consider emitting explicit “test/section started” progress lines (stderr) for live IDE feedback.

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<TestRun>\n";

    for (auto const& exec : execution.executions)
    {
        CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
        auto const& decl = *exec.instance.declaration;
        bool const success = !exec.is_considered_failing();

        out << "  <TestCase name=\"" << xml_escape(decl.name) << "\" ";
        out << "filename=\"" << xml_escape(decl.location.file_name()) << "\" ";
        out << "line=\"" << decl.location.line() << "\">\n";

        // Print all sections and expressions recursively (capped at max_errors)
        int const max_errors = 50;
        int error_count = 0;
        print_section_recursive(out, exec.root, "    ", error_count, max_errors);

        // Print test case summary
        out << "    <OverallResult success=\"" << (success ? "true" : "false") << "\" ";
        out << "durationInSeconds=\"" << exec.root.duration_seconds << "\"/>\n";
        out << "  </TestCase>\n";
    }

    out << "</TestRun>\n";
}
