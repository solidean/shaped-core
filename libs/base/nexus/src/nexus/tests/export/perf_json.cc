#include "perf_json.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/to_string.hh>

namespace
{
// Escapes a string for embedding in a JSON string literal. Mirrors xml_escape's role for junit.cc, but for
// JSON: quotes and backslashes are escaped, and control characters below 0x20 become \uXXXX escapes.
cc::string json_escape(cc::string_view str)
{
    cc::string result;
    for (auto const c : str)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char const* const hex = "0123456789abcdef";
                result += "\\u00";
                result += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                result += hex[static_cast<unsigned char>(c) & 0xF];
            }
            else
                result += c;
            break;
        }
    }
    return result;
}
} // namespace

cc::string nx::write_perf_json(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    cc::string out;
    out += "{\n";
    out += "  \"suite\": \"";
    out += json_escape(suite_name);
    out += "\",\n";
    out += "  \"metrics\": [";

    bool first = true;
    for (auto const& exec : execution.executions)
    {
        CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
        cc::string const test = json_escape(exec.instance.declaration->name);

        for (auto const& metric : exec.metrics)
        {
            out += first ? "\n" : ",\n";
            first = false;

            out += "    {\"test\": \"";
            out += test;
            out += "\", \"name\": \"";
            out += json_escape(metric.name);
            out += "\", \"value\": ";
            out += cc::to_string(metric.value);
            out += ", \"unit\": \"";
            out += json_escape(metric.unit);
            out += "\", \"higher_is_better\": ";
            out += metric.higher_is_better ? "true" : "false";
            out += "}";
        }
    }

    out += first ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}
