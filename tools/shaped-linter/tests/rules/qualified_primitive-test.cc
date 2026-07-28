#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `qualified-primitive` — the scratchpad the rule was built in, and where an interesting
// regression gets pinned. Breadth lives in tests/rules/corpus/qualified_primitive.md; see
// docs/coding-guidelines.md for which of the two a new case belongs in.

using namespace scl;

namespace
{
finding const& only(cc::span<finding const> found)
{
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "qualified-primitive");
    return found[0];
}

/// The one finding on `source`, requiring that it offers a fix, and the text that fix deletes.
cc::string deleted_by_fix(cc::string_view source)
{
    auto const found = run_rules_on_text(source);
    auto const& f = only(found);
    REQUIRE(f.suggested_fix.has_value());
    auto const& edits = f.suggested_fix.value().edits;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].replacement.empty());
    return cc::string(source.subview({.start = edits[0].span.byte_begin, .end = edits[0].span.byte_end}));
}

void expect_none(cc::string_view source)
{
    CHECK(run_rules_on_text(source).size() == 0);
}
} // namespace

TEST("shaped-linter - qualified-primitive - the fix deletes exactly the qualifier")
{
    SECTION("inside the namespace that re-exports the aliases")
    {
        CHECK(deleted_by_fix("namespace cc { void f(cc::u32 x); }") == "cc::");
    }
    SECTION("a nested namespace reaches them just as well")
    {
        CHECK(deleted_by_fix("namespace cc::impl { void f(cc::isize n); }") == "cc::");
    }
    SECTION("behind a file-scope using-directive")
    {
        CHECK(deleted_by_fix("using namespace cc::primitive_defines;\nvoid f(cc::byte b);\n") == "cc::");
    }
    SECTION("a leading global :: goes with it")
    {
        CHECK(deleted_by_fix("namespace sg { void f(::cc::u64 x); }") == "::cc::");
    }
    SECTION("odd spacing is handled by span, not by text")
    {
        CHECK(deleted_by_fix("namespace cc { void f(cc :: u32 x); }") == "cc :: ");
    }
}

TEST("shaped-linter - qualified-primitive - a namespace re-exporting through its own fwd.hh")
{
    // sg's `using namespace cc::primitive_defines;` lives in shaped-graphics/fwd.hh, which this linter
    // never sees — so `sg::u32` is reachable through the built-in list, and the fix still lands.
    CHECK(deleted_by_fix("namespace sg { void f(sg::u32 x); }") == "sg::");
    CHECK(deleted_by_fix("namespace sg { void f(cc::isize n); }") == "cc::");
}

TEST("shaped-linter - qualified-primitive - a namespace this file itself shows nominating them")
{
    auto const src = cc::string_view("namespace zzz { using namespace cc::primitive_defines; }\n"
                                     "namespace zzz { void f(zzz::u32 x); }\n");
    CHECK(deleted_by_fix(src) == "zzz::");
}

TEST("shaped-linter - qualified-primitive - out of scope: no fix, a prose hint instead")
{
    auto const found = run_rules_on_text("void f(cc::u32 x);");
    auto const& f = only(found);
    CHECK(!f.suggested_fix.has_value());
    REQUIRE(f.suggested_hint.has_value());
    CHECK(f.suggested_hint.value().edits.size() == 0);
    CHECK(f.suggested_hint.value().message.contains("using namespace cc::primitive_defines;"));
}

TEST("shaped-linter - qualified-primitive - in scope: a fix and no hint")
{
    auto const found = run_rules_on_text("namespace cc { void f(cc::u32 x); }");
    auto const& f = only(found);
    CHECK(f.suggested_fix.has_value());
    CHECK(!f.suggested_hint.has_value());
}

TEST("shaped-linter - qualified-primitive - the message names both spellings")
{
    auto const found = run_rules_on_text("void f(cc::isize n);");
    auto const& f = only(found);
    CHECK(f.message.contains("`cc::isize`"));
    CHECK(f.message.contains("`isize`"));
}

TEST("shaped-linter - qualified-primitive - a non-zero file id")
{
    // Regression: a whole-tree run gives each file its own id, and `span_text` asserts a span belongs to
    // the buffer it is read from. An anonymous namespace has no name, but that empty span is still this
    // file's — `run_rules_on_text` uses id 0 throughout, so only a real id catches a span left at 0.
    auto const buf = source_buffer::from_text(cc::string("namespace { void f(cc::u32 x); }\n"), "<mem>", 7);
    auto const found = run_rules(buf);
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "qualified-primitive");
    CHECK(found[0].span.file_id == 7);
}

TEST("shaped-linter - qualified-primitive - negatives never fire")
{
    SECTION("the bare spelling is the point of the rule")
    {
        expect_none("void f(u32 x, isize n, byte b);");
    }
    SECTION("a longer name that merely starts like an alias")
    {
        expect_none("void f(cc::byte_stream_builder& b, cc::u32string const& s);");
    }
    SECTION("a clean-core type is not a primitive alias")
    {
        expect_none("void f(cc::string const& s, cc::span<int> v);");
    }
    SECTION("std::byte is somebody else's alias")
    {
        expect_none("void f(std::byte b);");
    }
    SECTION("a namespace nobody re-exports through")
    {
        expect_none("void f(foo::u32 x);");
    }
    SECTION("a deeper qualification names a different entity")
    {
        expect_none("void f(a::cc::u32 x);");
    }
    SECTION("the alias used as a scope of its own")
    {
        expect_none("void f(cc::byte::inner x);");
    }
    SECTION("member access is not qualification")
    {
        expect_none("void f() { a.cc::u32; }");
    }
    SECTION("a comment, a string literal and a directive are single tokens")
    {
        expect_none("// cc::u32\n"
                    "/* cc::isize */\n"
                    "#define X cc::u32\n"
                    "char const* s = \"cc::byte\";\n");
    }
    SECTION("the declaration site itself spells them bare")
    {
        expect_none("namespace cc::primitive_defines { using isize = i64; }");
    }
}
