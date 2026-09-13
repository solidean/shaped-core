#include "timings_json.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>

namespace
{
void emit_test(babel::json::array_writer& tests,
               nx::test_execution const& exec,
               cc::string const& prefix,
               double steady_to_wall)
{
    namespace json = babel::json;
    CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");

    auto name = prefix;
    if (!exec.invocation_group.empty())
    {
        name += exec.invocation_group;
        name += " / ";
    }
    name += exec.instance.declaration->name;

    if (exec.started_at_steady_s > 0)
    {
        auto t = tests.write_object(json::layout::compact);
        t.write("name", name);
        t.write("start", exec.started_at_steady_s + steady_to_wall);
        t.write("end", exec.finished_at_steady_s + steady_to_wall);
        t.write("thread", exec.thread);
        t.write("failed", exec.is_considered_failing());
    }

    auto const child_prefix = name + " / ";
    for (auto const& child : exec.nested)
        emit_test(tests, child, child_prefix, steady_to_wall);
}
} // namespace

cc::string nx::write_timings_json(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    namespace json = babel::json;

    // Read back to back, so the offset is off by at most the gap between two clock reads.
    auto const steady_to_wall = cc::current_time_wall_secs() - cc::current_time_steady_secs();

    auto w = json::string_writer({.indent = 2, .non_finite = json::non_finite_policy::null});
    {
        auto root = w.object();
        root.write("suite", suite_name);

        auto tests = root.write_array("tests");
        for (auto const& exec : execution.executions)
            emit_test(tests, exec, cc::string(), steady_to_wall);
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
