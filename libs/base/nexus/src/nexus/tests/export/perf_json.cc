#include "perf_json.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <nexus/tests/export/json.hh>

cc::string nx::write_perf_json(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    cc::string out;
    out.appendf("{{\n  \"suite\": \"{}\",\n  \"metrics\": [", json_escape(suite_name));

    bool first = true;
    for (auto const& exec : execution.executions)
    {
        CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
        cc::string const test = json_escape(exec.instance.declaration->name);

        for (auto const& metric : exec.metrics)
        {
            out += first ? "\n" : ",\n";
            first = false;

            out.appendf("    {{\"test\": \"{}\", \"name\": \"{}\", \"value\": {}, \"unit\": \"{}\", "
                        "\"higher_is_better\": {}}}",
                        test, json_escape(metric.name), metric.value, json_escape(metric.unit), metric.higher_is_better);
        }
    }

    out += first ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}
