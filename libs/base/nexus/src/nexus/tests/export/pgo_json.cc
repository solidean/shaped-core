#include "pgo_json.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/record/desc.hh>

cc::string nx::write_pgo_json(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    namespace json = babel::json;

    // One metric per line, and a non-finite reading becomes null rather than invalid JSON: a sidecar nothing can
    // parse is worse than a hole in one row.
    auto w = json::string_writer({.indent = 2, .non_finite = json::non_finite_policy::null});

    {
        auto root = w.object();
        root.write("suite", suite_name);

        auto metrics = root.write_array("metrics");
        for (auto const& exec : execution.executions)
        {
            CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
            auto const test = exec.instance.declaration->name;

            for (auto const& metric : exec.metrics)
            {
                auto m = metrics.write_object(json::layout::compact);
                m.write("test", test);
                m.write("name", metric.name);
                m.write("value", metric.value);
                m.write("unit", metric.unit_symbol());
                m.write("higher_is_better", metric.higher_is_better());
            }
        }
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
