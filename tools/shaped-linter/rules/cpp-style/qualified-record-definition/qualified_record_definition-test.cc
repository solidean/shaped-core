#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `qualified-record-definition` — the scratchpad the rule was built in, and where an interesting regression gets pinned.
// Breadth lives in qualified_record_definition.md, next to this file.
// See docs/coding-guidelines.md for which of the two a new case belongs in.

using namespace scl;

namespace
{
/// The rewritten source, requiring exactly `count` findings and that all of them carry a fix.
cc::string fixed(cc::string_view source, isize count = 1, cc::string_view path = "a.hh")
{
    auto const found = run_rules_on_text(source, path);
    REQUIRE(found.size() == count);
    for (auto const& f : found)
    {
        CHECK(f.rule_id == "qualified-record-definition");
        REQUIRE(f.suggested_fix.has_value());
    }
    return apply_edits(source, collect_fix_edits(found));
}

/// The one finding on `source`, requiring that it offers a hint rather than a fix.
finding const& reported_only(cc::span<finding const> found)
{
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "qualified-record-definition");
    CHECK(!found[0].suggested_fix.has_value());
    CHECK(found[0].suggested_hint.has_value());
    return found[0];
}

void expect_none(cc::string_view source, cc::string_view path = "a.hh")
{
    CHECK(run_rules_on_text(source, path).size() == 0);
}
} // namespace

TEST("shaped-linter - qualified-record-definition - the fix unwraps the namespace")
{
    SECTION("one struct")
    {
        CHECK(fixed("namespace cc\n{\nstruct span\n{\n};\n}\n") == "struct cc::span\n{\n};\n");
    }
    SECTION("the closing brace takes its trailing comment with it")
    {
        CHECK(fixed("namespace cc\n{\nstruct span\n{\n};\n} // namespace cc\n") == "struct cc::span\n{\n};\n");
    }
    SECTION("every record in the namespace is qualified, and the wrapper is removed once")
    {
        CHECK(fixed("namespace cc\n{\nstruct a\n{\n};\nclass b\n{\n};\n}\n", 2)
              == "struct cc::a\n{\n};\nclass cc::b\n{\n};\n");
    }
    SECTION("a nested-name namespace keeps its spelling")
    {
        CHECK(fixed("namespace sg::dx12\n{\nstruct heap\n{\n};\n}\n") == "struct sg::dx12::heap\n{\n};\n");
    }
    SECTION("a template prefix stays in front of the qualified name")
    {
        CHECK(fixed("namespace cc\n{\ntemplate <class T>\nstruct span\n{\n};\n}\n")
              == "template <class T>\nstruct cc::span\n{\n};\n");
    }
    SECTION("a member record is the outer record's, and is never qualified itself")
    {
        CHECK(fixed("namespace cc\n{\nstruct outer\n{\n    struct inner\n    {\n    };\n};\n}\n")
              == "struct cc::outer\n{\n    struct inner\n    {\n    };\n};\n");
    }
    SECTION("an inner namespace is unwrapped with its own name only")
    {
        CHECK(fixed("namespace a\n{\nnamespace b\n{\nstruct x\n{\n};\n}\n}\n")
              == "namespace a\n{\nstruct b::x\n{\n};\n}\n");
    }
}

TEST("shaped-linter - qualified-record-definition - anything but definitions costs the fix, not the finding")
{
    SECTION("a function alongside the record")
    {
        auto const found = run_rules_on_text("namespace cc\n{\nstruct span\n{\n};\nvoid f();\n}\n", "a.hh");
        CHECK(reported_only(found).suggested_hint.value().message.contains("cc::span"));
    }
    SECTION("an enum alongside the record")
    {
        reported_only(run_rules_on_text("namespace cc\n{\nenum class e\n{\n};\nstruct s\n{\n};\n}\n", "a.hh"));
    }
    SECTION("a forward declaration cannot be qualified, so it blocks the unwrap")
    {
        reported_only(run_rules_on_text("namespace cc\n{\nstruct fwd;\nstruct s\n{\n};\n}\n", "a.hh"));
    }
    SECTION("a namespace-scope variable")
    {
        reported_only(run_rules_on_text("namespace cc\n{\nint k = 3;\nstruct s\n{\n};\n}\n", "a.hh"));
    }
    SECTION("a trailing declarator on the record itself")
    {
        reported_only(run_rules_on_text("namespace cc\n{\nstruct s\n{\n} the_one;\n}\n", "a.hh"));
    }
    SECTION("an anonymous record has no name to qualify")
    {
        reported_only(run_rules_on_text("namespace cc\n{\nstruct\n{\n} anon;\n}\n", "a.hh"));
    }
}

TEST("shaped-linter - qualified-record-definition - what it leaves alone")
{
    SECTION("a forward declaration is not a definition — this is what keeps fwd.hh clean")
    {
        expect_none("namespace cc\n{\nstruct span;\nclass string;\n}\n");
    }
    SECTION("impl and custom are meant to be opened")
    {
        expect_none("namespace cc::impl\n{\nstruct helper\n{\n};\n}\n");
        expect_none("namespace cc::custom\n{\nstruct point\n{\n};\n}\n");
    }
    SECTION("nesting inside impl counts as being inside it")
    {
        expect_none("namespace impl\n{\nnamespace parsing\n{\nstruct state\n{\n};\n}\n}\n");
    }
    SECTION("an anonymous namespace has no name to qualify with")
    {
        expect_none("namespace\n{\nstruct local\n{\n};\n}\n");
    }
    SECTION("a translation unit defines what it likes")
    {
        expect_none("namespace cc\n{\nstruct span\n{\n};\n}\n", "a.cc");
    }
    SECTION("a record already defined qualified")
    {
        expect_none("struct cc::span\n{\n};\n");
    }
}
