#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <rules/cpp-style/blessed-includes/blessed_includes.hh>
#include <shaped-linter/config/lint_config.hh>
#include <shaped-linter/rules/engine.hh>

using namespace scl;

namespace
{
lint_config config_of(cc::string_view text)
{
    lint_config out;
    auto directives = load_include_directives(text, "");
    CC_ASSERT(directives.has_value(), "the test's config must parse");
    out.include_directives = cc::move(directives.value());
    out.nearest_config_path = "libs/x/.shaped-lint.yml";
    return out;
}

isize count_of(cc::span<finding const> findings, cc::string_view rule_id)
{
    auto n = isize(0);
    for (auto const& f : findings)
        if (f.rule_id == rule_id)
            ++n;
    return n;
}

constexpr cc::string_view k_deny_mutex
    = "rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use clean-core/thread/mutex.hh\n";
} // namespace

TEST("blessed includes - a file with no config above it is silent")
{
    // The corpus and every snippet elsewhere rely on this: an empty policy says nothing was decided here.
    auto const found = run_rules_on_text("#include <mutex>\n", "a.cc");
    CHECK(count_of(found, "blessed-includes") == 0);
}

TEST("blessed includes - an unblessed include is reported, and the hint says where to bless it")
{
    auto const cfg = config_of(k_deny_mutex);
    auto const found = run_rules_on_text("#include <charconv>\n", "a.cc", cfg);

    REQUIRE(count_of(found, "blessed-includes") == 1);
    CHECK(found[0].message.contains("<charconv>"));
    REQUIRE(found[0].suggested_hint.has_value());
    CHECK(found[0].suggested_hint.value().message.contains("libs/x/.shaped-lint.yml"));
}

TEST("blessed includes - a denied include hints the replacement instead")
{
    auto const cfg = config_of(k_deny_mutex);
    auto const found = run_rules_on_text("#include <mutex>\n", "a.cc", cfg);

    REQUIRE(count_of(found, "blessed-includes") == 1);
    CHECK(found[0].suggested_hint.value().message == "use clean-core/thread/mutex.hh");
}

TEST("blessed includes - nothing is ever rewritten unattended")
{
    // Replacing the include also rewrites every call site below it, which is a hint's job and never a fix's.
    auto const cfg = config_of(k_deny_mutex);
    auto const found = run_rules_on_text("#include <mutex>\n", "a.cc", cfg);

    REQUIRE(found.size() == 1);
    CHECK(!found[0].suggested_fix.has_value());
}

TEST("blessed includes - the caret sits on the include, not the line")
{
    auto const cfg = config_of(k_deny_mutex);
    auto const source = cc::string_view("#include <mutex>\n");
    auto const found = run_rules_on_text(source, "a.cc", cfg);

    REQUIRE(found.size() == 1);
    CHECK(source.subview({.start = isize(found[0].span.byte_begin), .end = isize(found[0].span.byte_end)}) == "<mutex>");
}

TEST("blessed includes - our own headers need no blessing")
{
    auto const cfg = config_of(k_deny_mutex);

    // A path says it is ours; so does a bare `.hh`, which is how the generated shader headers are spelled.
    auto const found = run_rules_on_text(
        "#include <clean-core/fwd.hh>\n#include <sv_shaders.hh>\n#include \"sibling.hh\"\n", "a.cc", cfg);
    CHECK(count_of(found, "blessed-includes") == 0);
}

TEST("blessed includes - a header a macro spells is left alone")
{
    auto const cfg = config_of(k_deny_mutex);
    auto const found = run_rules_on_text("#include CONFIG_HEADER\n", "a.cc", cfg);
    CHECK(count_of(found, "blessed-includes") == 0);
}

TEST("blessed includes - an allow silences it")
{
    auto const cfg
        = config_of("rules:\n  - kind: allow-include\n    value: <type_traits>\n    reason: compiler intrinsics\n");
    auto const found = run_rules_on_text("#include <type_traits>\n", "a.cc", cfg);
    CHECK(count_of(found, "blessed-includes") == 0);
}

TEST("blessed includes - headers and translation units are scoped by the config, not the rule")
{
    // The rule knows nothing about .hh vs .cc; a `files:` glob is what draws that line.
    auto const cfg = config_of("rules:\n  - kind: allow-include\n    value: <memory>\n    reason: sg owns shared_ptr\n "
                               "   files: '**/*.cc'\n");

    CHECK(count_of(run_rules_on_text("#include <memory>\n", "a.cc", cfg), "blessed-includes") == 0);
    CHECK(count_of(run_rules_on_text("#include <memory>\n", "a.hh", cfg), "blessed-includes") == 1);
}

TEST("blessed includes - an include inside an #if is still an include")
{
    // Directives are opaque to the linter, so a guarded include is read as live code — the parser's own stance.
    auto const cfg = config_of(k_deny_mutex);
    auto const found = run_rules_on_text("#if 0\n#include <mutex>\n#endif\n", "a.cc", cfg);
    CHECK(count_of(found, "blessed-includes") == 1);
}
