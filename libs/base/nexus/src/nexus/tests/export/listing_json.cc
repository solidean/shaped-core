#include "listing_json.hh"

#include <babel-data/data/json.hh>
#include <nexus/fwd.hh> // also what puts the bare sized aliases in scope inside nx

namespace
{
namespace json = babel::json;

cc::string_view bucket_name(nx::config::test_bucket bucket)
{
    switch (bucket)
    {
    case nx::config::test_bucket::normal:
        return "normal";
    case nx::config::test_bucket::manual:
        return "manual";
    case nx::config::test_bucket::pgo_benchmark:
        return "pgo_benchmark";
    case nx::config::test_bucket::benchmark:
        return "benchmark";
    case nx::config::test_bucket::example:
        return "example";
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
        if (config.alias_filter_matches(alias))
            ++eligible_alias_count;

    // One entry per line: the listing is read by dev.py, but it is also read by a person chasing why a filter
    // selected nothing, and a field per line would bury a thousand tests.
    //
    // Every number here is a quantity a reader does arithmetic on — a seed is an int, a line a u32, a count a size —
    // so the default `number` policy is the right one.
    // A field that is an opaque id belongs in quotes at ITS call site, rather than under a document-wide policy that
    // would change a field's type for exactly the values that got large.
    auto w = json::string_writer({.indent = 2});

    {
        auto root = w.object();
        root.write("suite", suite_name);

        {
            auto filters = root.write_array("filters", json::layout::compact);
            for (auto const& filter : config.filters)
                filters.write(filter);
        }

        // The *resolved* reading of those filters, so a caller can say why nothing matched.
        root.write("filter_mode", config.matching_files ? "file" : "name");
        root.write("selected_bucket", bucket_name(config.selected_bucket));
        root.write("allow_cross_bucket_naming", config.allow_cross_bucket_naming);
        root.write("run_disabled_tests", config.run_disabled_tests);
        root.write("eligible_count", eligible_count);
        root.write("eligible_alias_count", eligible_alias_count);

        {
            auto tests = root.write_array("tests");
            for (auto const& decl : registry.declarations)
            {
                auto const& tc = decl.test_config;

                // Invocable (inert) tests never run standalone, so they are reported as not eligible; they are
                // still listed (with invocable: true) so tooling can see them and not treat the binary as empty.
                bool const invocable = decl.is_invocable();
                bool const eligible = !invocable && config.would_run(decl);

                auto t = tests.write_object(json::layout::compact);
                t.write("name", decl.name);
                t.write("file", decl.location.file_name());
                t.write("line", decl.location.line());
                t.write("bucket", bucket_name(tc.bucket));
                t.write("enabled", tc.enabled);
                t.write("seed", tc.seed);
                t.write("invocable", invocable);

                // A declared argument line is part of what a test IS, so a listing that omitted it could not explain
                // why a parametrized example behaved the way it did.
                // Empty when none was declared.
                t.write("args", tc.test_args != nullptr ? cc::string_view(tc.test_args) : cc::string_view());

                t.write("filter_matches", config.filter_matches(decl));
                t.write("eligible", eligible);
            }
        }

        // Aliases: pseudo test-names a filter can select (each expands to one or more scoped fragment runs).
        auto aliases = root.write_array("aliases");
        for (auto const& alias : registry.aliases)
        {
            auto a = aliases.write_object(json::layout::compact);
            a.write("name", alias.name);
            a.write("file", alias.location.file_name());
            a.write("line", alias.location.line());
            a.write("fragment_count", alias.fragments.size());
            a.write("filter_matches", config.alias_filter_matches(alias));
        }
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
