#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `qualified-type-definition` — the scratchpad the rule was built in, and where an interesting regression gets pinned.
// Breadth lives in qualified_type_definition.md, next to this file.
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
        CHECK(f.rule_id == "qualified-type-definition");
        REQUIRE(f.suggested_fix.has_value());
    }
    return apply_edits(source, collect_fix_edits(found));
}

/// The one finding on `source`, requiring that it offers a hint rather than a fix.
finding const& reported_only(cc::span<finding const> found)
{
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "qualified-type-definition");
    CHECK(!found[0].suggested_fix.has_value());
    CHECK(found[0].suggested_hint.has_value());
    return found[0];
}

void expect_none(cc::string_view source, cc::string_view path = "a.hh")
{
    CHECK(run_rules_on_text(source, path).size() == 0);
}
} // namespace

TEST("shaped-linter - qualified-type-definition - a namespace of definitions is unwrapped whole")
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
    SECTION("the blank lines inside the braces go with them, rather than doubling up")
    {
        CHECK(fixed("#pragma once\n\nnamespace cc\n{\n\nstruct span\n{\n};\n\n} // namespace cc\n\nvoid f();\n")
              == "#pragma once\n\nstruct cc::span\n{\n};\n\nvoid f();\n");
    }
    SECTION("a body with no blank lines to spare is untouched either side")
    {
        CHECK(fixed("#pragma once\n\nnamespace cc\n{\nstruct span\n{\n};\n} // namespace cc\n\nvoid f();\n")
              == "#pragma once\n\nstruct cc::span\n{\n};\n\nvoid f();\n");
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

TEST("shaped-linter - qualified-type-definition - a mixed namespace is split around its types")
{
    SECTION("a function after the record keeps the namespace below it")
    {
        CHECK(fixed("namespace cc\n{\nstruct span\n{\n};\nvoid f();\n}\n")
              == "struct cc::span\n{\n};\n\nnamespace cc\n{\nvoid f();\n}\n");
    }
    SECTION("a function before the record keeps the namespace above it")
    {
        CHECK(fixed("namespace cc\n{\nvoid f();\nstruct span\n{\n};\n}\n")
              == "namespace cc\n{\nvoid f();\n} // namespace cc\n\nstruct cc::span\n{\n};\n");
    }
    SECTION("a record between two functions is lifted out of the middle")
    {
        CHECK(fixed("namespace cc\n{\nvoid f();\nstruct span\n{\n};\nvoid g();\n}\n")
              == "namespace cc\n{\nvoid f();\n} // namespace cc\n\nstruct cc::span\n{\n};\n\nnamespace cc\n{\nvoid "
                 "g();\n}\n");
    }
    SECTION("adjacent records move as one block, not one at a time")
    {
        CHECK(fixed("namespace cc\n{\nvoid f();\nstruct a\n{\n};\nstruct b\n{\n};\n}\n", 2)
              == "namespace cc\n{\nvoid f();\n} // namespace cc\n\nstruct cc::a\n{\n};\nstruct cc::b\n{\n};\n");
    }
    SECTION("an enum joins the run beside a record, and both are qualified")
    {
        CHECK(fixed("namespace cc\n{\nenum class e : u8\n{\n};\nstruct s\n{\n};\n}\n", 2)
              == "enum class cc::e : cc::u8\n{\n};\nstruct cc::s\n{\n};\n");
    }
    SECTION("an unscoped enum with no enum-base cannot be declared ahead of itself, and ends the run")
    {
        CHECK(fixed("namespace cc\n{\nenum e\n{\n};\nstruct s\n{\n};\n}\n")
              == "namespace cc\n{\nenum e\n{\n};\n} // namespace cc\n\nstruct cc::s\n{\n};\n");
    }
    SECTION("a forward declaration stays behind — a qualified name may not declare a new entity")
    {
        CHECK(fixed("namespace cc\n{\nstruct fwd;\nstruct s\n{\n};\n}\n")
              == "namespace cc\n{\nstruct fwd;\n} // namespace cc\n\nstruct cc::s\n{\n};\n");
    }
    SECTION("the record's doc comment moves with it")
    {
        CHECK(fixed("namespace cc\n{\nvoid f();\n/// docs\nstruct s\n{\n};\n}\n")
              == "namespace cc\n{\nvoid f();\n} // namespace cc\n\n/// docs\nstruct cc::s\n{\n};\n");
    }
    SECTION("a class-template specialization is qualified like any other definition")
    {
        CHECK(fixed("namespace tg\n{\nvoid f();\ntemplate <class T>\nstruct traits<T>\n{\n};\n}\n")
              == "namespace tg\n{\nvoid f();\n} // namespace tg\n\ntemplate <class T>\nstruct tg::traits<T>\n{\n};\n");
    }
}

TEST("shaped-linter - qualified-type-definition - an enum is a type like any other")
{
    SECTION("a bare enum-base is qualified too — it only ever resolved through the namespace")
    {
        CHECK(fixed("namespace sv\n{\nenum class layout_kind : u8\n{\n};\n}\n")
              == "enum class sv::layout_kind : sv::u8\n{\n};\n");
    }
    SECTION("the base takes the namespace's root, where the using-directive re-exporting it sits")
    {
        CHECK(fixed("namespace cc::console\n{\nenum class color : u8\n{\n};\n}\n")
              == "enum class cc::console::color : cc::u8\n{\n};\n");
    }
    SECTION("a base that resolves on its own is left as written")
    {
        CHECK(fixed("namespace cc\n{\nenum class e : int\n{\n};\n}\n") == "enum class cc::e : int\n{\n};\n");
        CHECK(fixed("namespace cc\n{\nenum class e : unsigned char\n{\n};\n}\n")
              == "enum class cc::e : unsigned char\n{\n};\n");
    }
    SECTION("an enum-base the linter cannot place is not rewritten on a guess")
    {
        expect_none("namespace cc\n{\nenum class e : my_alias\n{\n};\n}\n");
    }
    SECTION("a scoped enum needs no enum-base — `enum class e;` is a declaration the definition can refer back to")
    {
        CHECK(fixed("namespace cc\n{\nenum class e\n{\n};\n}\n") == "enum class cc::e\n{\n};\n");
    }
    SECTION("`enum struct` is the scoped form too")
    {
        CHECK(fixed("namespace cc\n{\nenum struct e\n{\n};\n}\n") == "enum struct cc::e\n{\n};\n");
    }
    SECTION("an unscoped enum fires once it has fixed its underlying type")
    {
        CHECK(fixed("namespace cc\n{\nenum e : u8\n{\n};\n}\n") == "enum cc::e : cc::u8\n{\n};\n");
    }
    SECTION("without an enum-base there is no opaque declaration to write, so it is left alone")
    {
        expect_none("namespace cc\n{\nenum e\n{\n};\n}\n");
    }
    SECTION("an anonymous enum has no name to qualify")
    {
        expect_none("namespace cc\n{\nenum : u8\n{\n    a\n};\n}\n");
    }
    SECTION("an enum definition that also declares a variable would move that variable to file scope")
    {
        expect_none("namespace cc\n{\nenum class e : u8\n{\n} v;\n}\n");
    }
    SECTION("a member enum belongs to its record, and is qualified through it")
    {
        CHECK(fixed("namespace cc\n{\nstruct s\n{\n    enum class e : u8\n    {\n    };\n};\n}\n")
              == "struct cc::s\n{\n    enum class e : u8\n    {\n    };\n};\n");
    }
    SECTION("an enum declared inside a function body belongs to the function")
    {
        expect_none("namespace cc\n{\nauto make()\n{\n    enum class e : u8\n    {\n    };\n    return e{};\n}\n}\n");
    }
    SECTION("an opaque declaration is what the rewrite depends on, and is never itself reported")
    {
        expect_none("namespace cc\n{\nenum class e : u8;\nenum class f;\n}\n");
    }
    SECTION("an enum already defined qualified")
    {
        expect_none("enum class cc::e : cc::u8\n{\n};\n");
    }
}

TEST("shaped-linter - qualified-type-definition - what cannot move stays, and ends the run")
{
    SECTION("an anonymous record is never reported")
    {
        expect_none("namespace cc\n{\nstruct\n{\n} anon;\n}\n");
    }
    SECTION("the named record beside it still moves, and the anonymous one keeps the namespace")
    {
        CHECK(fixed("namespace cc\n{\nstruct\n{\n} anon;\nstruct s\n{\n};\n}\n")
              == "namespace cc\n{\nstruct\n{\n} anon;\n} // namespace cc\n\nstruct cc::s\n{\n};\n");
    }
    SECTION("a definition that also declares a variable would move that variable's scope")
    {
        expect_none("namespace cc\n{\nstruct s\n{\n} the_one;\n}\n");
    }
    SECTION("it ends the run, so an adjacent record moves on its own")
    {
        CHECK(fixed("namespace cc\n{\nstruct s\n{\n} the_one;\nstruct b\n{\n};\n}\n")
              == "namespace cc\n{\nstruct s\n{\n} the_one;\n} // namespace cc\n\nstruct cc::b\n{\n};\n");
    }
    SECTION("a record declared inside a function body belongs to the function")
    {
        expect_none("namespace cc\n{\nauto make()\n{\n    struct seeder\n    {\n    };\n    return seeder{};\n}\n}\n");
    }
}

TEST("shaped-linter - qualified-type-definition - what it leaves alone")
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
