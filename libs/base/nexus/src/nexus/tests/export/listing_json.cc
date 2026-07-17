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
    // Invocable tests are inert (never scheduled directly), so they never count as eligible.
    int eligible_count = 0;
    for (auto const& decl : registry.declarations)
        if (!decl.is_invocable() && config.would_run(decl))
            ++eligible_count;

    // Aliases run their fragments when a filter matches the alias name; a binary whose only match is an alias
    // must still be selected, so this count is reported alongside eligible_count (dev.py OR's the two).
    int eligible_alias_count = 0;
    for (auto const& alias : registry.aliases)
        if (config.alias_matches(alias))
            ++eligible_alias_count;

    cc::string out;
    out.appendf("{{\n  \"suite\": \"{}\",\n", json_escape(suite_name));

    out += "  \"filters\": [";
    for (cc::isize i = 0; i < config.filters.size(); ++i)
        out.appendf("{}\"{}\"", i == 0 ? "" : ", ", json_escape(config.filters[i]));
    out += "],\n";

    out.appendf("  \"selected_bucket\": \"{}\",\n", bucket_name(config.selected_bucket));
    out.appendf("  \"allow_cross_bucket_naming\": {},\n", config.allow_cross_bucket_naming);
    out.appendf("  \"run_disabled_tests\": {},\n", config.run_disabled_tests);
    out.appendf("  \"eligible_count\": {},\n", eligible_count);
    out.appendf("  \"eligible_alias_count\": {},\n", eligible_alias_count);

    out += "  \"tests\": [";
    bool first = true;
    for (auto const& decl : registry.declarations)
    {
        auto const& tc = decl.test_config;

        out += first ? "\n" : ",\n";
        first = false;

        // Invocable (inert) tests never run standalone, so they are reported as not eligible; they are
        // still listed (with invocable: true) so tooling can see them and not treat the binary as empty.
        bool const invocable = decl.is_invocable();
        bool const eligible = !invocable && config.would_run(decl);

        out.appendf("    {{\"name\": \"{}\", \"file\": \"{}\", \"line\": {}, \"bucket\": \"{}\", \"enabled\": {}, "
                    "\"seed\": {}, \"invocable\": {}, \"name_matches\": {}, \"eligible\": {}}}",
                    json_escape(decl.name), json_escape(decl.location.file_name()), decl.location.line(),
                    bucket_name(tc.bucket), tc.enabled, tc.seed, invocable, config.name_matches(decl), eligible);
    }
    out += first ? "]\n" : "\n  ]\n";

    // Aliases: pseudo test-names a filter can select (each expands to one or more scoped fragment runs).
    out += "  ,\"aliases\": [";
    bool first_alias = true;
    for (auto const& alias : registry.aliases)
    {
        out += first_alias ? "\n" : ",\n";
        first_alias = false;

        out.appendf("    {{\"name\": \"{}\", \"file\": \"{}\", \"line\": {}, \"fragment_count\": {}, "
                    "\"name_matches\": {}}}",
                    json_escape(alias.name), json_escape(alias.location.file_name()), alias.location.line(),
                    alias.fragments.size(), config.alias_matches(alias));
    }
    out += first_alias ? "]\n" : "\n  ]\n";

    out += "}\n";
    return out;
}
