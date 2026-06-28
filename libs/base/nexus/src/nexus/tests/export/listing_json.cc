#include "listing_json.hh"

#include <clean-core/string/to_string.hh>
#include <nexus/tests/export/json.hh>

namespace
{
cc::string_view bucket_name(nx::config::test_bucket bucket)
{
    switch (bucket)
    {
    case nx::config::test_bucket::normal:
        return "normal";
    case nx::config::test_bucket::manual:
        return "manual";
    case nx::config::test_bucket::guide_benchmark:
        return "guide_benchmark";
    }
    return "unknown";
}
} // namespace

cc::string nx::write_test_listing_json(cc::string_view suite_name,
                                       test_schedule_config const& config,
                                       test_registry const& registry)
{
    int eligible_count = 0;
    for (auto const& decl : registry.declarations)
        if (config.would_run(decl))
            ++eligible_count;

    cc::string out;
    out += "{\n";
    out += "  \"suite\": \"";
    out += json_escape(suite_name);
    out += "\",\n";

    out += "  \"filters\": [";
    for (cc::isize i = 0; i < config.filters.size(); ++i)
    {
        out += i == 0 ? "\"" : ", \"";
        out += json_escape(config.filters[i]);
        out += "\"";
    }
    out += "],\n";

    out += "  \"selected_bucket\": \"";
    out += bucket_name(config.selected_bucket);
    out += "\",\n";
    out += "  \"match_any_bucket\": ";
    out += config.match_any_bucket ? "true" : "false";
    out += ",\n";
    out += "  \"run_disabled_tests\": ";
    out += config.run_disabled_tests ? "true" : "false";
    out += ",\n";
    out += "  \"eligible_count\": ";
    out += cc::to_string(eligible_count);
    out += ",\n";

    out += "  \"tests\": [";
    bool first = true;
    for (auto const& decl : registry.declarations)
    {
        auto const& tc = decl.test_config;

        out += first ? "\n" : ",\n";
        first = false;

        out += "    {\"name\": \"";
        out += json_escape(decl.name);
        out += "\", \"file\": \"";
        out += json_escape(decl.location.file_name());
        out += "\", \"line\": ";
        out += cc::to_string(decl.location.line());
        out += ", \"bucket\": \"";
        out += bucket_name(tc.bucket);
        out += "\", \"enabled\": ";
        out += tc.enabled ? "true" : "false";
        out += ", \"seed\": ";
        out += cc::to_string(tc.seed);
        out += ", \"name_matches\": ";
        out += config.name_matches(decl) ? "true" : "false";
        out += ", \"eligible\": ";
        out += config.would_run(decl) ? "true" : "false";
        out += "}";
    }
    out += first ? "]\n" : "\n  ]\n";

    out += "}\n";
    return out;
}
