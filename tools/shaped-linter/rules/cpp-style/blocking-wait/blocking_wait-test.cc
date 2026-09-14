#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <rules/cpp-style/blocking-wait/blocking_wait.hh>
#include <shaped-linter/config/lint_config.hh>
#include <shaped-linter/rules/engine.hh>

using namespace scl;

namespace
{
lint_config config_of(cc::string_view text, cc::string_view base_dir = "")
{
    lint_config out;
    auto directives = load_include_directives(text, base_dir);
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

constexpr cc::string_view k_deny_in_tests = "rules:\n"
                                            "  - kind: deny-blocking-wait\n"
                                            "    value: [async_blocking_get, try_async_blocking_get]\n"
                                            "    reason: await it in an ASYNC_TEST instead\n"
                                            "    files: tests/**\n"
                                            "  - kind: allow-blocking-wait\n"
                                            "    value: async_blocking_get\n"
                                            "    reason: this test's subject is the blocking get\n"
                                            "    files: tests/blocking-get-test.cc\n";

constexpr cc::string_view k_call = "void f() { auto v = cc::async_blocking_get(node); }\n";
} // namespace

TEST("blocking wait - a file with no config above it is silent")
{
    CHECK(count_of(run_rules_on_text(k_call, "tests/a-test.cc"), "blocking-wait") == 0);
}

TEST("blocking wait - a denied call in a file the entry names is reported with its reason")
{
    auto const cfg = config_of(k_deny_in_tests);
    auto const found = run_rules_on_text(k_call, "tests/a-test.cc", cfg);

    REQUIRE(count_of(found, "blocking-wait") == 1);
    CHECK(found[0].message.contains("async_blocking_get"));
    REQUIRE(found[0].suggested_hint.has_value());
    CHECK(found[0].suggested_hint.value().message.contains("ASYNC_TEST"));
}

TEST("blocking wait - a file outside the denied scope, or allowed by name, is silent")
{
    auto const cfg = config_of(k_deny_in_tests);
    CHECK(count_of(run_rules_on_text(k_call, "src/a.cc", cfg), "blocking-wait") == 0);
    CHECK(count_of(run_rules_on_text(k_call, "tests/blocking-get-test.cc", cfg), "blocking-wait") == 0);
}

TEST("blocking wait - a comment or a string naming the call is not a call")
{
    auto const cfg = config_of(k_deny_in_tests);
    auto const text = "// cc::async_blocking_get(node) used to be here\nchar const* s = \"async_blocking_get\";\n";
    CHECK(count_of(run_rules_on_text(text, "tests/a-test.cc", cfg), "blocking-wait") == 0);
}

TEST("blocking wait - an include config does not switch the rule on, and a wait config does not bless includes")
{
    auto const include_only
        = config_of("rules:\n  - kind: deny-include\n    value: <mutex>\n    reason: use cc::mutex\n");
    CHECK(!include_only.checks_blocking_waits());
    CHECK(count_of(run_rules_on_text(k_call, "tests/a-test.cc", include_only), "blocking-wait") == 0);

    auto const wait_only = config_of(k_deny_in_tests);
    CHECK(!wait_only.checks_includes());
}
