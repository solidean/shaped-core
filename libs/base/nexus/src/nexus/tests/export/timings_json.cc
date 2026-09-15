#include "timings_json.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <nexus/tests/config.hh>

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
        if (!exec.nested.empty())
            t.write("children", exec.nested.size());

        auto const& cfg = exec.instance.declaration->test_config;
        if (cfg.exclusive_global)
            t.write("exclusive", true);
        if (cfg.main_thread)
            t.write("main_thread", true);
        if (cfg.exclusion_tag_count > 0)
        {
            auto tags = t.write_array("tags");
            for (auto i = 0; i < cc::min(cfg.exclusion_tag_count, nx::config::max_exclusion_tags); ++i)
                tags.write(cc::string_view(cfg.exclusion_tags[i]));
        }
        if (cfg.scheduler == nx::config::scheduler_mode::own_pool)
            t.write("phase", "own_pool");
        else if (cfg.scheduler == nx::config::scheduler_mode::none)
            t.write("phase",
                    cfg.ambient == nx::config::ambient_mode::single_threaded ? "singlethreaded" : "no_scheduler");
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
