#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `qualified-type-definition` — the scratchpad the rule was built in, and where an interesting regression gets pinned.
// Breadth lives in qualified_type_definition.md, next to this file.
// See docs/coding-guidelines.md for which of the two a new case belongs in.

using namespace scl;

namespace
{
/// A header and its library's fwd.hh, both rewritten by the fix.
struct fixed_files
{
    cc::string header;
    cc::string fwd;
};

/// The header `source`, in a library whose fwd.hh holds `fwd_text`, rewritten by its findings' fixes.
/// The header includes that fwd.hh ahead of `source`, which the returned header leaves out again.
/// Requires exactly `count` findings and that all of them carry a fix.
fixed_files fix_both(cc::string_view source, isize count = 1, cc::string_view fwd_text = "#pragma once\n")
{
    constexpr cc::string_view include = "#include <cc/fwd.hh>\n";
    auto sm = source_manager();
    auto const& fwd = sm.add_from_text(cc::string(fwd_text), "lib/src/cc/fwd.hh");
    auto const& header = sm.add_from_text(cc::string(include) + source, "lib/src/cc/a.hh");
    auto const found = run_rules(header, all_rules(), empty_lint_config(), &sm);
    REQUIRE(found.size() == count);
    for (auto const& f : found)
    {
        CHECK(f.rule_id == "qualified-type-definition");
        REQUIRE(f.suggested_fix.has_value());
    }
    auto header_edits = cc::vector<text_edit>();
    auto fwd_edits = cc::vector<text_edit>();
    for (auto const& e : collect_fix_edits(found))
        (e.span.file_id == fwd.file_id() ? fwd_edits : header_edits).push_back(e);
    auto rewritten = apply_edits(header.text(), header_edits);
    REQUIRE(cc::string_view(rewritten).starts_with(include));
    return {.header = cc::string(cc::string_view(rewritten).subview(include.size())),
            .fwd = apply_edits(fwd.text(), fwd_edits)};
}

/// The rewritten header alone.
cc::string fixed(cc::string_view source, isize count = 1)
{
    return fix_both(source, count).header;
}

/// The rewritten fwd.hh alone.
cc::string fixed_fwd(cc::string_view source, cc::string_view fwd_text, isize count = 1)
{
    return fix_both(source, count, fwd_text).fwd;
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

TEST("shaped-linter - qualified-type-definition - the fix declares the type in the library's fwd.hh")
{
    SECTION("inside its namespace block, before the block closes")
    {
        CHECK(fixed_fwd("namespace cc\n{\nstruct span\n{\n};\n}\n", "#pragma once\n\nnamespace cc\n{\nstruct "
                                                                    "other;\n}\n")
              == "#pragma once\n\nnamespace cc\n{\nstruct other;\nstruct span;\n}\n");
    }
    SECTION("in a block of its own where fwd.hh opens none")
    {
        CHECK(fixed_fwd("namespace cc\n{\nstruct span\n{\n};\n}\n", "#pragma once\n")
              == "#pragma once\n\nnamespace cc\n{\nstruct span;\n} // namespace cc\n");
    }
    SECTION("a nested namespace is a block of its own, not its parent's")
    {
        CHECK(fixed_fwd("namespace cc::console\n{\nstruct style\n{\n};\n}\n", "namespace cc\n{\n}\n")
              == "namespace cc\n{\n}\n\nnamespace cc::console\n{\nstruct style;\n} // namespace cc::console\n");
    }
    SECTION("nothing is added where fwd.hh declares it already")
    {
        constexpr cc::string_view fwd = "namespace cc\n{\nstruct span;\n}\n";
        CHECK(fixed_fwd("namespace cc\n{\nstruct span\n{\n};\n}\n", fwd) == fwd);
    }
    SECTION("an enum keeps its enum-base, a template its head")
    {
        CHECK(fixed_fwd("namespace cc\n{\nenum class mode : u8\n{\n};\n}\n", "namespace cc\n{\n}\n")
              == "namespace cc\n{\nenum class mode : u8;\n}\n");
        CHECK(fixed_fwd("namespace cc\n{\nenum class mode : unsigned char\n{\n};\n}\n", "namespace cc\n{\n}\n")
              == "namespace cc\n{\nenum class mode : unsigned char;\n}\n");
        CHECK(fixed_fwd("namespace cc\n{\ntemplate <class T>\nstruct box\n{\n};\n}\n", "namespace cc\n{\n}\n")
              == "namespace cc\n{\ntemplate <class T>\nstruct box;\n}\n");
    }
    SECTION("a specialization needs no declaration: its primary template has one")
    {
        constexpr cc::string_view fwd = "namespace tg\n{\n}\n";
        CHECK(fix_both("namespace tg\n{\ntemplate <class T>\nstruct traits<box<T>>\n{\n};\n}\n", 1, "namespace tg\n{\n}\n")
                  .fwd
              == fwd);
    }
    SECTION("a header that does not include its fwd.hh gains the include")
    {
        auto sm = source_manager();
        (void)sm.add_from_text("namespace cc\n{\n}\n", "lib/src/cc/fwd.hh");
        auto const& header
            = sm.add_from_text("#pragma once\n\nnamespace cc\n{\nstruct span\n{\n};\n}\n", "lib/src/cc/sub/a.hh");
        auto const found = run_rules(header, all_rules(), empty_lint_config(), &sm);
        REQUIRE(found.size() == 1);
        REQUIRE(found[0].suggested_fix.has_value());
        auto edits = cc::vector<text_edit>();
        for (auto const& e : found[0].suggested_fix.value().edits)
            if (e.span.file_id == header.file_id())
                edits.push_back(e);
        CHECK(apply_edits(header.text(), edits) == "#pragma once\n#include <cc/fwd.hh>\n\nstruct cc::span\n{\n};\n");
    }
}

TEST("shaped-linter - qualified-type-definition - without a fwd.hh the finding says what to do, and fixes nothing")
{
    SECTION("no fwd.hh below the header's src/ folder")
    {
        auto sm = source_manager();
        auto const& header = sm.add_from_text("namespace cc\n{\nstruct span\n{\n};\n}\n", "lib/src/cc/a.hh");
        auto const found = run_rules(header, all_rules(), empty_lint_config(), &sm);
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_fix.has_value());
        CHECK(found[0].message.contains("no fwd.hh"));
        CHECK(found[0].message.contains("namespace cc"));
    }
    SECTION("a snippet linted alone, with no files to look in")
    {
        auto const found = run_rules_on_text("namespace cc\n{\nstruct span\n{\n};\n}\n", "a.hh");
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_fix.has_value());
    }
    SECTION("a template whose parameters have default arguments")
    {
        auto sm = source_manager();
        (void)sm.add_from_text("namespace cc\n{\n}\n", "lib/src/cc/fwd.hh");
        auto const& header = sm.add_from_text("#include <cc/fwd.hh>\nnamespace cc\n{\ntemplate <class T = int>\nstruct "
                                              "box\n{\n};\n}\n",
                                              "lib/src/cc/a.hh");
        auto const found = run_rules(header, all_rules(), empty_lint_config(), &sm);
        REQUIRE(found.size() == 1);
        CHECK(!found[0].suggested_fix.has_value());
        CHECK(found[0].message.contains("default argument"));
    }
}

TEST("shaped-linter - qualified-type-definition - a record in a buffer that is not file 0")
{
    // Regression: the fix path read the enum-base's TEXT to decide whether there was one.
    // A record has no enum-base, so that span is still default-constructed — file_id 0 — and asking a buffer
    // whose file_id is not 0 for its text tripped "span refers to a different file".
    //
    // Every other test here goes through run_rules_on_text, which always uses file_id 0, so the whole suite
    // missed it and only a real multi-file run crashed — on its second file onward.
    auto const source = cc::string_view("namespace cc\n{\nstruct span\n{\n};\nenum class mode : u8\n{\n};\n}\n");
    auto const buffer = source_buffer::from_text(cc::string(source), "a.hh", 7);

    auto const found = run_rules(buffer);
    REQUIRE(found.size() == 2);
    for (auto const& f : found)
        CHECK(f.rule_id == "qualified-type-definition");
}
