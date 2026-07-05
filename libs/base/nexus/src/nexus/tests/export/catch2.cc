#include "catch2.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
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

        out.appendf("{}<Expression success=\"false\" filename=\"{}\" line=\"{}\">\n", indent,
                    xml_escape(error.location.file_name()), error.location.line());
        out.appendf("{}  <Original>{}</Original>\n", indent, xml_escape(error.expr));
        out.appendf("{}  <Expanded>{}</Expanded>\n", indent, xml_escape(error.expanded));
        out.appendf("{}</Expression>\n", indent);

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
        out.appendf("{}<Section name=\"{}\" filename=\"{}\" line=\"{}\">\n", indent, xml_escape(subsec.name),
                    xml_escape(subsec.location.file_name()), subsec.location.line());

        // Recursively print subsection content
        print_section_recursive(out, subsec, indent + "  ", error_count, max_errors);

        // Print section summary
        // If the section is considered failing but has 0 failed checks (e.g., missing CHECK),
        // report at least 1 failure so C++ TestMate interprets it correctly
        auto const failures = subsec.is_considered_failing ? cc::max(subsec.failed_checks, 1) : subsec.failed_checks;
        out.appendf("{}  <OverallResults successes=\"{}\" failures=\"{}\" expectedFailures=\"0\" "
                    "durationInSeconds=\"{}\"/>\n",
                    indent, subsec.executed_checks - subsec.failed_checks, failures, subsec.duration_seconds);
        out.appendf("{}</Section>\n", indent);
    }
}

// Renders dispatched (nested) executions as sections: one <Section> per dispatch group, containing one
// <Section> per instance (named by the parametrized test), whose own sections/children nest below. This
// mirrors the executor's section path (group / instance / ...) so IDE "run this section" (-c) works.
void print_nested_executions(cc::string& out,
                             nx::test_execution const& parent,
                             cc::string const& indent,
                             int& error_count,
                             int max_errors)
{
    cc::isize i = 0;
    while (i < parent.nested.size())
    {
        auto const group = parent.nested[i].invocation_group;
        out.appendf("{}<Section name=\"{}\">\n", indent, xml_escape(group));

        double group_duration = 0;
        bool group_failing = false;
        while (i < parent.nested.size() && parent.nested[i].invocation_group == group)
        {
            auto const& child = parent.nested[i];
            CC_ASSERT(child.instance.declaration != nullptr, "nested instance is invalid");
            auto const& decl = *child.instance.declaration;

            out.appendf("{}  <Section name=\"{}\" filename=\"{}\" line=\"{}\">\n", indent, xml_escape(decl.name),
                        xml_escape(decl.location.file_name()), decl.location.line());
            print_section_recursive(out, child.root, indent + "    ", error_count, max_errors);
            print_nested_executions(out, child, indent + "    ", error_count, max_errors);

            bool const child_failing = child.is_considered_failing();
            auto const failures = child_failing ? cc::max(child.root.failed_checks, 1) : child.root.failed_checks;
            out.appendf("{}    <OverallResults successes=\"{}\" failures=\"{}\" expectedFailures=\"0\" "
                        "durationInSeconds=\"{}\"/>\n",
                        indent, child.root.executed_checks - child.root.failed_checks, failures,
                        child.root.duration_seconds);
            out.appendf("{}  </Section>\n", indent);

            group_duration += child.root.duration_seconds;
            group_failing |= child_failing;
            ++i;
        }

        out.appendf("{}  <OverallResults successes=\"{}\" failures=\"{}\" expectedFailures=\"0\" "
                    "durationInSeconds=\"{}\"/>\n",
                    indent, group_failing ? 0 : 1, group_failing ? 1 : 0, group_duration);
        out.appendf("{}</Section>\n", indent);
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
        // Parametrized tests are inert (run only under their driver via nx::invoke_tests); don't advertise them
        // as standalone runnable cases or the IDE would select nothing when one is clicked.
        if (decl.is_invocable())
            continue;

        out += "  <TestCase>\n";
        out.appendf("    <Name>{}</Name>\n", xml_escape(decl.name));
        out += "    <ClassName/>\n";
        out += "    <Tags></Tags>\n";
        out += "    <SourceInfo>\n";
        out.appendf("      <File>{}</File>\n", xml_escape(decl.location.file_name()));
        out.appendf("      <Line>{}</Line>\n", decl.location.line());
        out += "    </SourceInfo>\n";
        out += "  </TestCase>\n";
    }

    // Aliases are runnable (unlike bare invocable decls): clicking one runs its name as a filter, which the
    // scheduler expands into the fragment runs. Advertise them so IDEs can offer them directly.
    for (auto const& alias : registry.aliases)
    {
        out += "  <TestCase>\n";
        out.appendf("    <Name>{}</Name>\n", xml_escape(alias.name));
        out += "    <ClassName/>\n";
        out += "    <Tags></Tags>\n";
        out += "    <SourceInfo>\n";
        out.appendf("      <File>{}</File>\n", xml_escape(alias.location.file_name()));
        out.appendf("      <Line>{}</Line>\n", alias.location.line());
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

        out.appendf("  <TestCase name=\"{}\" filename=\"{}\" line=\"{}\">\n", xml_escape(decl.name),
                    xml_escape(decl.location.file_name()), decl.location.line());

        // Print all sections and expressions recursively (capped at max_errors)
        int const max_errors = 50;
        int error_count = 0;
        print_section_recursive(out, exec.root, "    ", error_count, max_errors);
        print_nested_executions(out, exec, "    ", error_count, max_errors);

        // Print test case summary
        out.appendf("    <OverallResult success=\"{}\" durationInSeconds=\"{}\"/>\n", success,
                    exec.root.duration_seconds);
        out += "  </TestCase>\n";
    }

    out += "</TestRun>\n";
    return out;
}
