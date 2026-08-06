#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <shaped-linter/rules/engine.hh>

// Smoke tests for `qualified-primitive` — the scratchpad the rule was built in, and where an interesting regression gets pinned.
// Breadth lives in qualified_primitive.md, next to this file.
// See docs/coding-guidelines.md for which of the two a new case belongs in.

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
cc::string deleted_by_fix(cc::string_view source, cc::string_view path = "<memory>")
{
    auto const found = run_rules_on_text(source, path);
    auto const& f = only(found);
    REQUIRE(f.suggested_fix.has_value());
    auto const& edits = f.suggested_fix.value().edits;
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].replacement.empty());
    return cc::string(source.subview({.start = edits[0].span.byte_begin, .end = edits[0].span.byte_end}));
}

void expect_none(cc::string_view source, cc::string_view path = "<memory>")
{
    CHECK(run_rules_on_text(source, path).size() == 0);
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
    // Inside a namespace the linter has never heard of: the aliases are not reachable, and which namespace
    // should nominate them is the library's call, so the rule offers prose rather than an edit.
    auto const found = run_rules_on_text("namespace some_lib { void f(cc::u32 x); }");
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
    auto const found = run_rules_on_text("namespace some_lib { void f(cc::isize n); }");
    auto const& f = only(found);
    CHECK(f.message.contains("`cc::isize`"));
    CHECK(f.message.contains("`isize`"));
}

TEST("shaped-linter - qualified-primitive - a non-zero file id")
{
    // Regression: a whole-tree run gives each file its own id, and `span_text` asserts a span belongs to the buffer it is read from.
    // An anonymous namespace has no name, but that empty span is still this file's.
    // `run_rules_on_text` uses id 0 throughout, so only a real id catches a span left at 0.
    auto const buf = source_buffer::from_text(cc::string("namespace { void f(cc::u32 x); }\n"), "mem.cc", 7);
    auto const found = run_rules(buf);
    REQUIRE(found.size() == 1);
    CHECK(found[0].rule_id == "qualified-primitive");
    CHECK(found[0].span.file_id == 7);
}

// -------------------------------------------------------------------------------------------------------
// File scope: a `.cc` earns the using-directive, a header earns silence
// -------------------------------------------------------------------------------------------------------

namespace
{
/// The insertion edit of the first file-scope finding: its offset and its text.
/// Takes the findings by reference and returns a reference INTO them, so the caller must keep them alive —
/// passing a `run_rules_on_text(…)` temporary straight in would dangle.
text_edit const& insertion_of(cc::vector<finding> const& found)
{
    REQUIRE(found.size() >= 1);
    auto const& f = found[0];
    REQUIRE(f.suggested_fix.has_value());
    auto const& edits = f.suggested_fix.value().edits;
    REQUIRE(edits.size() == 2);

    // edits[0] drops the qualifier, edits[1] adds the directive.
    CHECK(edits[0].replacement.empty());
    CHECK(edits[1].span.byte_begin == edits[1].span.byte_end); // an insertion has an empty span
    return edits[1];
}
} // namespace

TEST("shaped-linter - qualified-primitive - an out-of-line body is already in its namespace")
{
    // `cc::async_thread_pool::try_get_work(…) { … }` sits at file scope, but the qualified declarator-id
    // carries `cc` into the body — so the bare name is reachable there and only the qualifier goes.
    SECTION("the body needs no using-directive")
    {
        CHECK(deleted_by_fix("#include <a.hh>\n"
                             "\n"
                             "void cc::pool::work()\n"
                             "{\n"
                             "    auto n = cc::u32(1);\n"
                             "}\n",
                             "pool.cc")
              == "cc::");
    }

    SECTION("nor do the parameters, which also come after the declarator-id")
    {
        CHECK(deleted_by_fix("#include <a.hh>\n"
                             "\n"
                             "void cc::hash128::create(cc::u64 seed)\n"
                             "{\n"
                             "}\n",
                             "hash128.cc")
              == "cc::");
    }

    SECTION("a constructor's member-init list does not undo it")
    {
        CHECK(deleted_by_fix("#include <a.hh>\n"
                             "\n"
                             "cc::pool::pool(cc::u32 n) : _n(n), _x(0)\n"
                             "{\n"
                             "}\n",
                             "pool.cc")
              == "cc::");
    }

    SECTION("but the return type in front of it does not")
    {
        // Written BEFORE the declarator-id, so it really is looked up at file scope.
        auto const found
            = run_rules_on_text("#include <a.hh>\n\ncc::u64 cc::hash_of(isize n)\n{\n    return 0;\n}\n", "hash.cc");
        REQUIRE(found.size() == 1);
        REQUIRE(found[0].suggested_fix.has_value());
        CHECK(found[0].suggested_fix.value().edits.size() == 2);
    }

    SECTION("an unqualified free function's body is NOT in that namespace")
    {
        // A nexus TEST(…) expands to exactly this shape, which is why it must keep earning the directive.
        auto const found
            = run_rules_on_text("#include <a.hh>\n\nvoid f(cc::span<int> s)\n{\n    cc::u32 x;\n}\n", "x.cc");
        REQUIRE(found.size() == 1);
        REQUIRE(found[0].suggested_fix.has_value());
        CHECK(found[0].suggested_fix.value().edits.size() == 2);
    }
}

TEST("shaped-linter - qualified-primitive - a header at file scope says nothing")
{
    // There is no safe edit: a file-scope using-directive in a header leaks the aliases into the global namespace of every TU that includes it.
    // `<memory>` has no extension, so it counts as a header too.
    expect_none("void f(cc::u32 x);");
    CHECK(run_rules_on_text("void f(cc::u32 x);", "flags.hh").size() == 0);
    CHECK(run_rules_on_text("#include <x.hh>\n\ntemplate <cc::isize N>\nstruct cc::flags;\n", "flags.hh").size() == 0);
}

TEST("shaped-linter - qualified-primitive - a .cc at file scope gains the directive")
{
    auto const src = cc::string_view("#include \"hash.hh\"\n"
                                     "\n"
                                     "#include <xxhash.h>\n"
                                     "\n"
                                     "cc::u64 hash_of(cc::span<cc::byte const> data);\n");
    auto const found = run_rules_on_text(src, "hash.cc");
    auto const& ins = insertion_of(found);

    CHECK(ins.replacement == "\nusing namespace cc::primitive_defines;\n");

    // The anchor is the end of the last prologue line, so the directive lands after `#include <xxhash.h>`.
    CHECK(src.subview({.start = 0, .end = isize(ins.span.byte_begin)}).ends_with("#include <xxhash.h>\n"));
}

TEST("shaped-linter - qualified-primitive - the anchor is the last directive, not the last include")
{
    SECTION("a balanced conditional block anchors past its #endif")
    {
        auto const src = cc::string_view("#include <a.hh>\n"
                                         "\n"
                                         "#ifdef CC_OS_WINDOWS\n"
                                         "#include <b.hh>\n"
                                         "#endif\n"
                                         "\n"
                                         "void f(cc::u32 x);\n");
        auto const found = run_rules_on_text(src, "x.cc");
        auto const& ins = insertion_of(found);
        CHECK(src.subview({.start = 0, .end = isize(ins.span.byte_begin)}).ends_with("#endif\n"));
    }

    SECTION("an unclosed conditional falls back to the last directive before it")
    {
        // Anchoring on the last directive would land inside the `#if`, defining the aliases for one configuration only.
        // The last depth-0 directive is before the conditional, and holds for all.
        auto const src = cc::string_view("#include <a.hh>\n"
                                         "\n"
                                         "#if CC_HAS_THREADS\n"
                                         "extern void g();\n"
                                         "\n"
                                         "void f(cc::u32 x);\n");
        auto const found = run_rules_on_text(src, "thread.cc");
        auto const& ins = insertion_of(found);
        CHECK(src.subview({.start = 0, .end = isize(ins.span.byte_begin)}) == "#include <a.hh>\n");
    }

    SECTION("an #include past the anchor offers nothing")
    {
        // `cc::primitive_defines` has to be declared before the directive nominates it, and the include
        // that declares it may well be the one nested in the conditional — unknowable from this file.
        expect_none("#include <macros.hh>\n"
                    "\n"
                    "#if CC_HAS_THREADS\n"
                    "#include <thread>\n"
                    "\n"
                    "void f(cc::u32 x);\n",
                    "thread.cc");
    }

    SECTION("a prologue with no common ground at all offers nothing")
    {
        // The very first directive opens the conditional, so every offset past it belongs to one branch.
        // The rule reports nothing rather than fixing the file into one platform.
        expect_none("#ifdef __EMSCRIPTEN__\n"
                    "#include <emscripten.h>\n"
                    "\n"
                    "void f(cc::u32 x);\n",
                    "web_runner.cc");
    }

    SECTION("no directives at all inserts at the very top")
    {
        auto const found = run_rules_on_text("void f(cc::u32 x);\n", "x.cc");
        auto const& ins = insertion_of(found);
        CHECK(ins.span.byte_begin == 0);
        CHECK(ins.replacement == "using namespace cc::primitive_defines;\n\n");
    }
}

TEST("shaped-linter - qualified-primitive - every file-scope finding carries the same insertion")
{
    // Each fix must be safe on its own, so the shared edit rides on all of them; `collect_fix_edits` is what merges the copies.
    // Byte-identical is the property that merge depends on.
    auto const found = run_rules_on_text("#include <a.hh>\n"
                                         "\n"
                                         "cc::u32 f();\n"
                                         "cc::isize g();\n"
                                         "cc::byte h();\n",
                                         "x.cc");
    REQUIRE(found.size() == 3);
    for (auto const& f : found)
    {
        REQUIRE(f.suggested_fix.has_value());
        auto const& edits = f.suggested_fix.value().edits;
        REQUIRE(edits.size() == 2);
        CHECK(edits[1].span.byte_begin == found[0].suggested_fix.value().edits[1].span.byte_begin);
        CHECK(edits[1].replacement == found[0].suggested_fix.value().edits[1].replacement);
    }

    CHECK(collect_fix_edits(found).size() == 4); // three deletions, one shared insertion
}

TEST("shaped-linter - qualified-primitive - an anonymous namespace is the file's own")
{
    // A nexus TEST(…) expands at column 0, so a test file has helpers in an anonymous namespace and bodies below it.
    // One file-scope directive reaches both, since lookup inside an unnamed namespace escapes to the global namespace — which is where the directive nominates.
    // So both findings carry the same fix.
    auto const found = run_rules_on_text("#include <a.hh>\n"
                                         "\n"
                                         "namespace\n"
                                         "{\n"
                                         "void helper(cc::u32 a);\n"
                                         "} // namespace\n"
                                         "\n"
                                         "void body(cc::isize b);\n",
                                         "x-test.cc");
    REQUIRE(found.size() == 2);

    for (auto const& f : found)
    {
        REQUIRE(f.suggested_fix.has_value());
        CHECK(f.suggested_fix.value().edits.size() == 2);
        CHECK(!f.suggested_hint.has_value());
    }
    CHECK(collect_fix_edits(found).size() == 3); // two deletions, one shared insertion
}

TEST("shaped-linter - qualified-primitive - a NAMED namespace still only gets a hint")
{
    // The difference that makes the anonymous case mechanical: a named namespace belongs to a library, and
    // the directive belongs in that library's fwd.hh — which is a call about the library, not this file.
    auto const found = run_rules_on_text("#include <a.hh>\n\nnamespace some_lib { void f(cc::u32 x); }\n", "x.cc");
    auto const& f = only(found);
    CHECK(!f.suggested_fix.has_value());
    CHECK(f.suggested_hint.has_value());
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
    SECTION("a using-declaration needs the qualified name it declares")
    {
        // `using i64;` is not valid C++, so this one has to stay exactly as written.
        expect_none("namespace cc { using cc::i64; }");
        expect_none("namespace cc { using ::cc::isize; }");
        expect_none("#include <a.hh>\n\nusing cc::u32;\n", "x.cc");
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
