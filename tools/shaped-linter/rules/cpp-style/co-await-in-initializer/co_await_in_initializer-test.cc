#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <rules/cpp-style/co-await-in-initializer/co_await_in_initializer.hh>
#include <shaped-linter/rules/engine.hh>

using namespace scl;

namespace
{
cc::vector<finding> findings_of(cc::string_view text)
{
    auto out = cc::vector<finding>();
    for (auto const& f : run_rules_on_text(text, "src/a.cc"))
        if (f.rule_id == "co-await-in-initializer")
            out.push_back(f);
    return out;
}

cc::string_view underlined(cc::string_view text, finding const& f)
{
    return text.subview({.start = isize(f.span.byte_begin), .end = isize(f.span.byte_end)});
}
} // namespace

TEST("co-await-in-initializer - the finding underlines the co_await, in code and in a macro's body")
{
    constexpr cc::string_view in_code = "task f() { auto p = pair{seed(), co_await make()}; }\n";
    auto const code = findings_of(in_code);
    REQUIRE(code.size() == 1);
    CHECK(underlined(in_code, code[0]) == "co_await");

    constexpr cc::string_view in_macro = "#define M(x) pair{seed(), \\\n    co_await make(x)}\n";
    auto const macro = findings_of(in_macro);
    REQUIRE(macro.size() == 1);
    CHECK(underlined(in_macro, macro[0]) == "co_await");
}

TEST("co-await-in-initializer - the macro this rule exists for")
{
    constexpr cc::string_view text = "#define SG_ROUTES(entry)                                    \\\n"
                                     "    route_pair                                              \\\n"
                                     "    {                                                       \\\n"
                                     "        .text = co_await compiled(*ctx, text::entry),       \\\n"
                                     "        .native = c ? co_await a(entry) : co_await b(entry) \\\n"
                                     "    }\n";
    CHECK(findings_of(text).size() == 2);
}
