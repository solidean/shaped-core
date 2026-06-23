#include "junit.hh"

#include <nexus/tests/export/xml.hh>

#include <clean-core/assert.hh>

#include <ostream>
#include <string>
#include <vector>

using nx::impl::xml_escape;

namespace
{
// Collects all failed expressions across the (recursive) section tree of a test,
// flattened in execution order. Pointers stay valid for the lifetime of `exec`.
void collect_errors(nx::test_execution::section const& sec, std::vector<nx::test_error const*>& out)
{
    for (auto const& error : sec.errors)
        out.push_back(&error);

    for (auto const& subsec : sec.subsections)
        collect_errors(subsec, out);
}
} // namespace

void nx::write_junit_xml(std::ostream& out, std::string_view suite_name, nx::test_schedule_execution const& execution)
{
    int const total_tests = execution.count_total_tests();
    int const failed_tests = execution.count_failed_tests();

    double total_time = 0.0;
    for (auto const& exec : execution.executions)
        total_time += exec.root.duration_seconds;

    std::string const suite = xml_escape(suite_name);

    auto emit_suite_attrs = [&](std::ostream& os)
    {
        os << "name=\"" << suite << "\" "
           << "tests=\"" << total_tests << "\" "
           << "failures=\"" << failed_tests << "\" "
           << "errors=\"0\" skipped=\"0\" "
           << "time=\"" << total_time << "\"";
    };

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<testsuites ";
    emit_suite_attrs(out);
    out << ">\n";
    out << "  <testsuite ";
    emit_suite_attrs(out);
    out << ">\n";

    for (auto const& exec : execution.executions)
    {
        CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
        auto const& decl = *exec.instance.declaration;

        out << "    <testcase classname=\"" << suite << "\" "
            << "name=\"" << xml_escape(decl.name) << "\" "
            << "time=\"" << exec.root.duration_seconds << "\"";

        if (!exec.is_considered_failing())
        {
            out << "/>\n";
            continue;
        }

        out << ">\n";

        std::vector<test_error const*> errors;
        collect_errors(exec.root, errors);

        // The message attribute points at the first failing location, or the
        // test's own declaration when a test fails without a specific check
        // (e.g. a test that ran no checks at all).
        auto const& msg_loc = errors.empty() ? decl.location : errors.front()->location;
        out << "      <failure message=\"" << xml_escape(msg_loc.file_name()) << ":" << msg_loc.line() << "\">";

        if (errors.empty())
        {
            out << xml_escape("test failed without a reported check");
        }
        else
        {
            std::string body;
            for (auto const* error : errors)
            {
                body += error->expr;
                if (!error->expanded.empty() && error->expanded != error->expr)
                {
                    body += " => ";
                    body += error->expanded;
                }
                body += " at ";
                body += error->location.file_name();
                body += ":";
                body += std::to_string(error->location.line());
                body += "\n";
            }
            out << xml_escape(body);
        }

        out << "</failure>\n";
        out << "    </testcase>\n";
    }

    out << "  </testsuite>\n";
    out << "</testsuites>\n";
}
