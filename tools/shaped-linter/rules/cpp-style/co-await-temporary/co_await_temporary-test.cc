#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <rules/cpp-style/co-await-temporary/co_await_temporary.hh>
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

isize count_of(cc::span<finding const> findings)
{
    auto n = isize(0);
    for (auto const& f : findings)
        if (f.rule_id == "co-await-temporary")
            ++n;
    return n;
}

constexpr cc::string_view k_deny_all = "rules:\n"
                                       "  - kind: deny-co-await-temporary\n"
                                       "    value: \"*\"\n"
                                       "    reason: the async may be its node's only owner\n"
                                       "  - kind: allow-co-await-temporary\n"
                                       "    value: bytes\n"
                                       "    reason: bytes() returns a copy of the handle its future keeps\n";

isize findings_in(cc::string_view body)
{
    auto const text = cc::format("cc::shared_async<int> f() {{ {} co_return 0; }}\n", body);
    return count_of(run_rules_on_text(text, "src/a.cc", config_of(k_deny_all)));
}
} // namespace

TEST("co-await-temporary - a file with no config above it is silent")
{
    CHECK(count_of(run_rules_on_text("void g() { auto const& x = co_await load(p); }\n", "src/a.cc")) == 0);
}

TEST("co-await-temporary - a reference bound to awaiting a call is reported with a hint")
{
    auto const found = run_rules_on_text("void g() { auto const& rows = co_await load(p); }\n", "src/a.cc", config_of(k_deny_all));
    REQUIRE(count_of(found) == 1);
    CHECK(found[0].message.contains("rows"));
    REQUIRE(found[0].suggested_hint.has_value());
    CHECK(found[0].suggested_hint.value().message.contains("rows_async"));
    CHECK(!found[0].suggested_fix.has_value());
}

TEST("co-await-temporary - the shapes that are calls")
{
    CHECK(findings_in("auto const& x = co_await sol::find<K>(t, false);") == 1);
    CHECK(findings_in("auto&& x = co_await make();") == 1);
    CHECK(findings_in("int const& x = co_await obj->fetch(1);") == 1);
    CHECK(findings_in("auto const& x = co_await [&] { return load(p); }();") == 1);
}

TEST("co-await-temporary - what is not a temporary stays quiet")
{
    CHECK(findings_in("auto const& x = co_await pending;") == 0);
    CHECK(findings_in("auto const& x = co_await state.pending;") == 0);
    CHECK(findings_in("auto const& x = co_await *pending;") == 0);
    CHECK(findings_in("auto const& x = co_await (pending);") == 0);
    CHECK(findings_in("auto const& x = co_await cc::move(pending);") == 0);
    CHECK(findings_in("auto x = co_await load(p);") == 0);         // a copy owns its value
    CHECK(findings_in("co_await load(p);") == 0);                  // nothing bound
    CHECK(findings_in("auto const& x = co_await written.bytes();") == 0); // allowed by name
}
