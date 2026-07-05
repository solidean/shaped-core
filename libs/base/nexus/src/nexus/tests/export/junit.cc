#include "junit.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/tests/export/xml.hh>

using nx::impl::xml_escape;

namespace
{
// Collects all failed expressions across the (recursive) section tree of a test,
// flattened in execution order. Pointers stay valid for the lifetime of `exec`.
void collect_errors(nx::test_execution::section const& sec, cc::vector<nx::test_error const*>& out)
{
    for (auto const& error : sec.errors)
        out.push_back(&error);

    for (auto const& subsec : sec.subsections)
        collect_errors(subsec, out);
}

// Emits one <testcase> for `exec` (failing only on its *own* tree; dispatched children are their own
// testcases), then recurses into dispatched children. `prefix` accumulates the addressable path.
void emit_testcase(cc::string& out, cc::string const& suite, nx::test_execution const& exec, cc::string const& prefix)
{
    CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
    auto const& decl = *exec.instance.declaration;

    cc::string name = prefix;
    if (!exec.invocation_group.empty())
    {
        name += exec.invocation_group;
        name += " / ";
    }
    name += decl.name;

    out.appendf("    <testcase classname=\"{}\" name=\"{}\" time=\"{}\"", suite, xml_escape(name),
                exec.root.duration_seconds);

    if (!exec.root.is_considered_failing)
    {
        out += "/>\n";
    }
    else
    {
        out += ">\n";

        cc::vector<nx::test_error const*> errors;
        collect_errors(exec.root, errors);

        // message points at the first failing location, or the test's declaration when it failed with no
        // specific check (e.g. a test that ran no checks at all).
        auto const& msg_loc = errors.empty() ? decl.location : errors.front()->location;
        out.appendf("      <failure message=\"{}:{}\">", xml_escape(msg_loc.file_name()), msg_loc.line());

        if (errors.empty())
        {
            out += xml_escape("test failed without a reported check");
        }
        else
        {
            cc::string body;
            for (auto const* error : errors)
            {
                body += error->expr;
                if (!error->expanded.empty() && error->expanded != error->expr)
                {
                    body += " => ";
                    body += error->expanded;
                }
                body.appendf(" at {}:{}\n", error->location.file_name(), error->location.line());
            }
            out += xml_escape(body);
        }

        out += "</failure>\n";
        out += "    </testcase>\n";
    }

    cc::string const child_prefix = name + " / ";
    for (auto const& child : exec.nested)
        emit_testcase(out, suite, child, child_prefix);
}
} // namespace

cc::string nx::write_junit_xml(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    int const total_tests = execution.count_total_tests();
    int const failed_tests = execution.count_failed_tests();
    int const total_checks = execution.count_total_checks();

    double total_time = 0.0;
    for (auto const& exec : execution.executions)
        total_time += exec.root.duration_seconds;

    cc::string const suite = xml_escape(suite_name);

    // `assertions` (total checks evaluated) is not part of the base JUnit schema
    // but is widely understood; the dev.py runner reads it to report check counts.
    auto emit_suite_attrs = [&](cc::string& os)
    {
        os.appendf("name=\"{}\" tests=\"{}\" failures=\"{}\" errors=\"0\" skipped=\"0\" assertions=\"{}\" time=\"{}\"",
                   suite, total_tests, failed_tests, total_checks, total_time);
    };

    cc::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<testsuites ";
    emit_suite_attrs(out);
    out += ">\n";
    out += "  <testsuite ";
    emit_suite_attrs(out);
    out += ">\n";

    for (auto const& exec : execution.executions)
        emit_testcase(out, suite, exec, cc::string());

    out += "  </testsuite>\n";
    out += "</testsuites>\n";
    return out;
}
