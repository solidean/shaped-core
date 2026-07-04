#include "listing_json.hh"

#include <clean-core/string/format.hh>
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
    out.appendf("{{\n  \"suite\": \"{}\",\n", json_escape(suite_name));

    out += "  \"filters\": [";
    for (cc::isize i = 0; i < config.filters.size(); ++i)
        out.appendf("{}\"{}\"", i == 0 ? "" : ", ", json_escape(config.filters[i]));
    out += "],\n";

    out.appendf("  \"selected_bucket\": \"{}\",\n", bucket_name(config.selected_bucket));
    out.appendf("  \"match_any_bucket\": {},\n", config.match_any_bucket);
    out.appendf("  \"run_disabled_tests\": {},\n", config.run_disabled_tests);
    out.appendf("  \"eligible_count\": {},\n", eligible_count);

    out += "  \"tests\": [";
    bool first = true;
    for (auto const& decl : registry.declarations)
    {
        auto const& tc = decl.test_config;

        out += first ? "\n" : ",\n";
        first = false;

        out.appendf("    {{\"name\": \"{}\", \"file\": \"{}\", \"line\": {}, \"bucket\": \"{}\", \"enabled\": {}, "
                    "\"seed\": {}, \"name_matches\": {}, \"eligible\": {}}}",
                    json_escape(decl.name), json_escape(decl.location.file_name()), decl.location.line(),
                    bucket_name(tc.bucket), tc.enabled, tc.seed, config.name_matches(decl), config.would_run(decl));
    }
    out += first ? "]\n" : "\n  ]\n";

    out += "}\n";
    return out;
}
